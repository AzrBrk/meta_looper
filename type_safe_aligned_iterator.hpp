#pragma once

// ============================================================================
// type_safe_aligned_iterator —— 类型安全的对齐字节缓冲迭代器
//
// 这是 type_safe_aligned_iterator.hpp（meta_stream_head_only）在 meta_looper
// 上的移植版。公开 API 与旧版保持一致：
//
//   type_safe_aligned_iterator<Ty...>         正向迭代器（operator* / ++ / ==）
//   type_safe_aligned_reverse_iterator<Ty...> 反向迭代器（destroy 用）
//   type_safe_aligned_range<Ty...>            begin/end/rbegin/rend 适配
//   aligned_iterator_storage<Ty...>           单个槽的受检代理
//   initialize(range, args...)                按对齐布局构造全部元素
//   field_size<Ty...>()                       整个布局的字节数
//
// 内部实现全部换成 meta_looper 这一套：
//
//   - 对齐偏移：meta_pipe 把类型流依次灌进 meta_aligned_iterator，
//     再用 meta_ostream<> / meta_rostream<> 收集——与旧库同一种写法；
//   - 总大小 / 最大对齐：直接 meta_states::run<stream_states_co<>, ...>，
//     不再需要 meta_all_transfer / meta_object_construct 那一层；
//   - type_mask：把一个 meta_states_object 放在 rec 位上跑一次流。
//     旧库的 type_mask 依赖 MSO 的 changed_pred + stream_op 位，由那个对象
//     自己记录"哪一步变了"；新 MSO 只是 flags，所以这里直接用自己的字段布局：
//        bits 0..15  : 已接收的步数（u16）
//        bits 16..79 : 命中位图（u64，第 i 位 = 第 i 个类型是 T）
//     这两个字段只属于这个 MSO，looper 不解释它们；looper 自己的 position/
//     user/timer 布局仍在条件对象的 flags 里，互不干扰。
//     位图是 64 位，所以类型数上限为 64（aligned_iterator_storage 里有
//     static_assert；超过时右移会越界，直接拒绝而不是给出错误的 safe()）。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <exception>
#include <istream>
#include <memory>
#include <new>
#include <ostream>
#include <type_traits>
#include <utility>

#include "meta_ios.hpp"
#include "meta_stream.hpp"

namespace type_safe_aligned {

using namespace exp_utilities;
using namespace meta_iostream;
using namespace meta_objects;
using namespace meta_states;
using namespace meta_ios;

// ============================================================================
// 1) 布局计算
// ============================================================================

// 累积"对齐要求最大的那个类型"：payload 是一个类型本身，而不是列表。
struct larger_alignment_f {
  // 必须用真实类型初始化：不能接受 no_arguments 哨兵，否则初始 payload 会变成
  // 哨兵类型，第一次比较的对象就错了。
  template <class from_ins>
    requires (!no_arguments<from_ins>)
  using initialize = from_ins;

  template <class this_type, class from_ins>
  using apply = std::conditional_t<alignof(this_type) < alignof(from_ins),
                                   from_ins, this_type>;
};

using larger_alignment_calculator = meta_object<larger_alignment_f>;

namespace layout_details {

template <class... Ty>
using aligned_receiver_after_all =
    typename meta_states::run<meta_ios::stream_states_co<>,
                              meta_aligned_iterator,
                              meta_istream_list<Ty...>>::type::to;

template <class... Ty>
using largest_aligned_type =
    typename meta_states::run<meta_ios::stream_states_co<>,
                              larger_alignment_calculator,
                              meta_istream_list<Ty...>>::type::to_t;

}  // namespace layout_details

// 整个布局的字节数：先把类型流走成"最后一个 seek_to"，再用最大对齐类型推进一次。
// （对齐是按类型对齐要求计算的，末尾 padding 属于布局的一部分。）
template <class... Ty>
consteval std::size_t field_size() {
  static_assert((!std::is_reference_v<Ty> && ...),
                "can not calculate size for reference types");

  using end_seek = layout_details::aligned_receiver_after_all<Ty...>;
  using largest_aligned_t = layout_details::largest_aligned_type<Ty...>;
  return meta_invoke<end_seek, largest_aligned_t>::type::value;
}

// ============================================================================
// 2) type_mask：把 MSO 放在 rec 位跑一次类型流
// ============================================================================
//
// 旧版用 same_states_iterator + transfer_until 得到一串"变化历史位"。
// 新版不需要 changed_pred：接收器每一步把自己的 counter +1，命中时在
// 对应位置置位即可。步数就是 looper 的轮次，位图就是 MSO 自己的 flags。

namespace mask_details {

inline constexpr std::size_t count_shift = 0;  // u16
inline constexpr std::size_t mask_shift = 16;  // u64（最多 64 个类型）

// 第 Index 位（Index >= 64 时不再置位，避免移位越界）。
template <bool Match, std::size_t Index>
struct mask_bit : std::integral_constant<u64, 0> {};
template <std::size_t Index>
  requires (Index < 64)
struct mask_bit<true, Index>
    : std::integral_constant<u64, (u64{1} << Index)> {};

template <class T>
struct same_type_mask_fn {
  template <class flags, class obj>
  struct apply {
    static constexpr bool match = std::is_same_v<T, meta_produce_t<obj>>;
    static constexpr std::size_t index = flags::template get<u16, count_shift>;
    static constexpr u64 bit = mask_bit<match, index>::value;

    static constexpr auto value = [] {
      using counted = typename flags::template inc<u16, count_shift>;
      if constexpr (bit == 0) {
        return counted::value;
      } else {
        return counted::template field_or<u64, mask_shift, bit>::value;
      }
    }();
  };
};

template <class T>
using same_type_mask_o = meta_states_object<0, same_type_mask_fn<T>>;

}  // namespace mask_details

// 第 i 位为 1 <=> Ty[i] 是 T。typelist 里可以重复出现 T。
template <class T, class... Ty>
inline constexpr std::uint64_t type_mask_series = [] {
  using run_t = meta_states::run<meta_ios::stream_states_co<>,
                                 mask_details::same_type_mask_o<T>,
                                 meta_istream_list<Ty...>>;
  return run_t::type::to_t::template get<u64, mask_details::mask_shift>;
}();

// ============================================================================
// 3) 单个槽的受检代理
// ============================================================================
template <class... Ty>
struct aligned_iterator_storage {
  // type_mask_series 用 64 位位图；超过 64 个类型时右移会越界（UB），
  // 而且位图本身也表达不了。这里直接拒绝，而不是静默给出错误的 safe()。
  static_assert(sizeof...(Ty) <= 64,
                "aligned_iterator_storage: at most 64 types "
                "(type_mask_series is a 64-bit mask)");

  enum class error { null_pointer, type_not_safe, index_exceed };

  class exception : public std::exception {
   public:
    explicit exception(error code) noexcept : code_(code) {}
    error code() const noexcept { return code_; }

    const char* what() const noexcept override {
      switch (code_) {
        case error::null_pointer:
          return "aligned_iterator_storage: null pointer";
        case error::type_not_safe:
          return "aligned_iterator_storage: type is not safe at this index";
        case error::index_exceed:
          return "aligned_iterator_storage: index exceeds type list";
      }
      return "aligned_iterator_storage: unknown error";
    }

   private:
    error code_;
  };

  std::byte* ptr{nullptr};
  std::size_t m_type_index{0};

  // 禁止 *it1 = *it2 这种“把一个槽代理赋给另一个槽代理”的写法：
  // 隐式 copy assignment 会静默复制 ptr/索引（等于重绑定迭代器），而不是把值
  // 写到目标槽。要写值请用 *it = value；要复制代理请显式拷贝构造。
  aligned_iterator_storage& operator=(aligned_iterator_storage const&) = delete;

  // T 在该槽位上是否被允许。T 必须是 Ty... 中的一种；越界/空指针抛异常。
  template <class T>
  bool safe() const {
    if constexpr (!exp_try_find<T, exp_list<Ty...>>::value) {
      static_assert(exp_try_find<T, exp_list<Ty...>>::value,
                    "T must be one of aligned_iterator_storage's types");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      return (type_mask_series<T, Ty...> >> m_type_index) & 1;
    }
  }

  // 在当前槽位上用 placement new 开始 T 的生命周期。
  template <class T>
  bool emplace(T&& val) {
    using value_type = std::remove_cvref_t<T>;
    if constexpr (!exp_try_find<value_type, exp_list<Ty...>>::value) {
      static_assert(exp_try_find<value_type, exp_list<Ty...>>::value,
                    "T must be one of aligned_iterator_storage's types");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      if (!((type_mask_series<value_type, Ty...> >> m_type_index) & 1)) {
        throw exception(error::type_not_safe);
      }
      ::new (static_cast<void*>(ptr)) value_type(std::forward<T>(val));
      return true;
    }
  }

  // 受检地取当前槽位上的 T&。
  template <class T>
  operator T&() const {
    if constexpr (!exp_try_find<T, exp_list<Ty...>>::value) {
      static_assert(exp_try_find<T, exp_list<Ty...>>::value,
                    "T must be one of aligned_iterator_storage's types");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      if (!((type_mask_series<T, Ty...> >> m_type_index) & 1)) {
        throw exception(error::type_not_safe);
      }
      return *reinterpret_cast<T*>(ptr);
    }
  }

  // 受检地给当前槽位上已经构造的 T 赋值。
  template <class T>
  void operator=(T const& val) {
    if constexpr (!exp_try_find<T, exp_list<Ty...>>::value) {
      static_assert(exp_try_find<T, exp_list<Ty...>>::value,
                    "T must be one of aligned_iterator_storage's types");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      if (!((type_mask_series<T, Ty...> >> m_type_index) & 1)) {
        throw exception(error::type_not_safe);
      }
      *reinterpret_cast<T*>(ptr) = val;
    }
  }
};

// ============================================================================
// 4) 正向 / 反向迭代器
// ============================================================================

// 旧写法：meta_pipe<...>::all_to<meta_aligned_iterator>::run<meta_ostream<>>::type::to_t
// 新写法完全相同——pipe 就是按这个形状实现的。
template <class... Ty>
using aligned_offset_stream =
    typename meta_pipe<meta_istream_list<Ty...>>
        ::template all_to<meta_aligned_iterator>
        ::template run<meta_ostream<>>::type::to_t;

template <class... Ty>
using reverse_aligned_offset_stream =
    typename meta_pipe<meta_istream_list<Ty...>>
        ::template all_to<meta_aligned_iterator>
        ::template run<meta_rostream<>>::type::to::type;

template <class... Ty>
class type_safe_aligned_iterator {
  static_assert(sizeof...(Ty) > 0,
                "type_safe_aligned_iterator requires at least one type");

  using offset_array_type = aligned_offset_stream<Ty...>;
  static constexpr auto offsets = to_meta_array_t<offset_array_type>::array();
  // 整个布局的字节数 = 最后一个槽位之后的位置。
  static constexpr std::size_t total_size = field_size<Ty...>();

  aligned_iterator_storage<Ty...> storage_{};

  static constexpr std::size_t offset_at(std::size_t index) {
    return index == sizeof...(Ty) ? total_size : offsets[index];
  }

  // 前进一个槽位：指针按对齐偏移差移动。
  type_safe_aligned_iterator& advance() {
    using storage_type = aligned_iterator_storage<Ty...>;
    if (storage_.m_type_index >= sizeof...(Ty)) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    if (storage_.ptr == nullptr) {
      throw typename storage_type::exception(storage_type::error::null_pointer);
    }
    const auto current_offset = offset_at(storage_.m_type_index);
    const auto next_offset = offset_at(storage_.m_type_index + 1);
    storage_.ptr += next_offset - current_offset;
    ++storage_.m_type_index;
    return *this;
  }

 public:
  // 构造函数是 explicit 的：必须同时给出缓冲区起点和槽位号。
  // type_index == sizeof...(Ty) 表示 end()；空指针保留到访问槽位时才诊断。
  explicit type_safe_aligned_iterator(std::byte* base_ptr,
                                      std::size_t type_index) {
    if (type_index > sizeof...(Ty)) {
      using storage_type = aligned_iterator_storage<Ty...>;
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    storage_.ptr = base_ptr == nullptr ? nullptr : base_ptr + offset_at(type_index);
    storage_.m_type_index = type_index;
  }

  // 解引用给出当前槽位的受检代理。
  aligned_iterator_storage<Ty...>& operator*() { return storage_; }

  // 前缀 ++：移动到对齐布局里的下一个槽位。
  type_safe_aligned_iterator& operator++() { return advance(); }

  bool operator==(type_safe_aligned_iterator const& other) const {
    return storage_.ptr == other.storage_.ptr &&
           storage_.m_type_index == other.storage_.m_type_index;
  }

  bool operator!=(type_safe_aligned_iterator const& other) const {
    return !(*this == other);
  }
};

template <class... Ty>
class type_safe_aligned_reverse_iterator {
  static_assert(sizeof...(Ty) > 0,
                "type_safe_aligned_reverse_iterator requires at least one type");

  using reverse_aligned_ptr_list = reverse_aligned_offset_stream<Ty...>;
  static constexpr auto offsets =
      to_meta_array_t<reverse_aligned_ptr_list>::array();

  aligned_iterator_storage<Ty...> storage_{};
  std::byte* base_ptr_{nullptr};
  std::size_t reverse_index_{0};

  void set_position(std::size_t reverse_index) {
    reverse_index_ = reverse_index;
    if (reverse_index_ == sizeof...(Ty)) {
      storage_.ptr = base_ptr_;
      storage_.m_type_index = sizeof...(Ty);
      return;
    }
    storage_.ptr = base_ptr_ == nullptr ? nullptr : base_ptr_ + offsets[reverse_index_];
    storage_.m_type_index = sizeof...(Ty) - reverse_index_ - 1;
  }

 public:
  explicit type_safe_aligned_reverse_iterator(std::byte* base_ptr,
                                              std::size_t reverse_index)
      : base_ptr_(base_ptr) {
    if (reverse_index > sizeof...(Ty)) {
      using storage_type = aligned_iterator_storage<Ty...>;
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    set_position(reverse_index);
  }

  aligned_iterator_storage<Ty...>& operator*() { return storage_; }

  // 销毁当前槽位上的对象（类型由索引决定）。
  void destroy() {
    using storage_type = aligned_iterator_storage<Ty...>;
    if (reverse_index_ >= sizeof...(Ty)) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    if (storage_.ptr == nullptr) {
      throw typename storage_type::exception(storage_type::error::null_pointer);
    }
    meta_states::run<meta_ios::stream_states_co<>, meta_iterator,
                     meta_istream_list<Ty...>>::for_each(
        [this]<class Stream>(Stream stream) {
          using current_type = typename Stream::to_t;
          if (stream.index() == storage_.m_type_index) {
            std::destroy_at(reinterpret_cast<current_type*>(storage_.ptr));
          }
        });
  }

  type_safe_aligned_reverse_iterator& operator++() {
    using storage_type = aligned_iterator_storage<Ty...>;
    if (reverse_index_ >= sizeof...(Ty)) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    if (storage_.ptr == nullptr) {
      throw typename storage_type::exception(storage_type::error::null_pointer);
    }
    set_position(reverse_index_ + 1);
    return *this;
  }

  bool operator==(type_safe_aligned_reverse_iterator const& other) const {
    return base_ptr_ == other.base_ptr_ && reverse_index_ == other.reverse_index_;
  }

  bool operator!=(type_safe_aligned_reverse_iterator const& other) const {
    return !(*this == other);
  }
};

// ============================================================================
// 5) range 适配
// ============================================================================
template <class... Ty>
class type_safe_aligned_range {
  static_assert((!std::is_reference_v<Ty> && ...),
                "type_safe_aligned_range does not accept reference types; "
                "use std::reference_wrapper<T>");

 public:
  // base_ptr 必须指向足够容纳整个 Ty... 布局的存储。
  explicit type_safe_aligned_range(std::byte* base_ptr) : base_ptr_(base_ptr) {}

  type_safe_aligned_iterator<Ty...> begin() const {
    return type_safe_aligned_iterator<Ty...>(base_ptr_, 0);
  }

  type_safe_aligned_iterator<Ty...> end() const {
    return type_safe_aligned_iterator<Ty...>(base_ptr_, sizeof...(Ty));
  }

  type_safe_aligned_reverse_iterator<Ty...> rbegin() const {
    return type_safe_aligned_reverse_iterator<Ty...>(base_ptr_, 0);
  }

  type_safe_aligned_reverse_iterator<Ty...> rend() const {
    return type_safe_aligned_reverse_iterator<Ty...>(base_ptr_, sizeof...(Ty));
  }

  std::byte* data() const { return base_ptr_; }

 private:
  std::byte* base_ptr_;
};

// ============================================================================
// 6) 构造全部元素（异常时按逆序清理已经构造的部分）
// ============================================================================
template <class... Ty, class... Args>
void initialize(type_safe_aligned_range<Ty...> const& range, Args&&... args) {
  static_assert(sizeof...(Ty) == sizeof...(Args),
                "initialize requires one argument for each range element");
  auto* data = range.data();
  meta_states::run<meta_ios::stream_states_co<>, meta_aligned_iterator,
                   meta_istream_list<Ty...>>::for_each_forward(
      [&range, data]<class Stream, class Arg>(Stream ptr_stream, Arg&& value) {
        try {
          ptr_stream.object().emplace(data, std::forward<Arg>(value));
        } catch (...) {
          // ptr_stream.index() 是本轮（0-based）的槽位号；前 index 个元素已经
          // 构造好，逆序销毁它们，跳过当前与之后（未构造）的槽位。
          constexpr auto index = ptr_stream.index();
          auto destroy_iter = range.rbegin();
          for (std::size_t i = 0; i < sizeof...(Ty) - index; ++i) {
            ++destroy_iter;
          }
          for (; destroy_iter != range.rend(); ++destroy_iter) {
            destroy_iter.destroy();
          }
          throw;
        }
      },
      std::forward<Args>(args)...);
}

// ============================================================================
// 7) 按当前槽位的类型读写
// ============================================================================
template <class... Ty>
std::ostream& operator<<(std::ostream& os,
                         aligned_iterator_storage<Ty...> const& storage) {
  using storage_type = aligned_iterator_storage<Ty...>;
  if (storage.m_type_index >= sizeof...(Ty)) {
    throw typename storage_type::exception(storage_type::error::index_exceed);
  }
  meta_states::run<meta_ios::stream_states_co<>, meta_iterator,
                   meta_istream_list<Ty...>>::for_each(
      [&os, &storage]<class Stream>(Stream s) {
        using current_type = typename Stream::to_t;
        if (os && s.index() == storage.m_type_index &&
            storage.template safe<current_type>()) {
          os << static_cast<current_type&>(storage);
        }
      });
  return os;
}

template <class... Ty>
std::istream& operator>>(std::istream& is,
                         aligned_iterator_storage<Ty...>& storage) {
  using storage_type = aligned_iterator_storage<Ty...>;
  if (storage.m_type_index >= sizeof...(Ty)) {
    throw typename storage_type::exception(storage_type::error::index_exceed);
  }
  meta_states::run<meta_ios::stream_states_co<>, meta_iterator,
                   meta_istream_list<Ty...>>::for_each(
      [&is, &storage]<class Stream>(Stream s) {
        using current_type = typename Stream::to_t;
        if (is && s.index() == storage.m_type_index &&
            storage.template safe<current_type>()) {
          is >> static_cast<current_type&>(storage);
        }
      });
  return is;
}

}  // namespace type_safe_aligned
