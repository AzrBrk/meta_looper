#pragma once

// ============================================================================
// type_safe_aligned_iterator —— 类型安全的对齐字节缓冲迭代器
//
// 这是 type_safe_aligned_iterator.hpp（meta_stream_head_only）在 meta_looper
// 上的移植版。公开 API 与旧版保持一致：
//
//   type_safe_aligned_iterator<Alignment, Ty...>         正向迭代器（operator* / ++ / ==）
//   type_safe_aligned_reverse_iterator<Alignment, Ty...> 反向迭代器（destroy 用）
//   type_safe_aligned_range<Alignment, Ty...>            begin/end/rbegin/rend 适配
//   aligned_iterator_storage<Ty...>                     单个槽的受检代理
//   initialize(range, args...)                          按布局构造全部元素
//   field_size<Alignment, Ty...>()                      整个布局的字节数
//
// Alignment：0 = 每格按 alignof 对齐（内存布局，默认）；给定 2 的幂则所有
// 槽位统一用这个边界（1 = packed，适合线上/文件格式）。
//
// 引用：Ty... 里可以直接写 T& / T const&，布局、掩码、流一律按
// stored_type_t<Ty>（即 std::reference_wrapper）计算；值层面不做隐式包装，
// 构造/赋值仍要用户显式写 std::ref / std::cref。
//
// 内部实现全部换成 meta_looper 这一套：
//
//   - 对齐偏移：meta_pipe 把类型流依次灌进 meta_aligned_iterator<>，
//     正向用 meta_ostream<> 收成“绝对偏移表”，反向用 meta_rostream<> 收成
//     “逆序的绝对坐标表”（偏移仍是相对起点的绝对值，只是顺序反了）；
//     两个迭代器各用自己的那张表，槽位号就是表的下标，不做任何跨表换算。
//   - 总大小 / 最大对齐：直接 meta_states::run<stream_states_co<>, ...>，
//     不再需要 meta_all_transfer / meta_object_construct 那一层；
//   - type_mask：把一个 meta_states_object 放在 rec 位上跑一次流。
//     旧库的 type_mask 依赖 MSO 的 changed_pred + stream_op 位，由那个对象
//     自己记录"哪一步变了"；新 MSO 只是 flags，所以这里直接拿 flags 当位图：
//        bits 0..127 : 命中位图（u128，第 i 位 = 第 i 个类型能当 T 用，
//                       即相同或者加 const 之后相同；去 const 的不算）
//     不需要计数字段：每步 flags = (flags << 1) | 命中位，走完 N 步后按 N 位
//     反转一次即可。这段位只属于这个 MSO，looper 不解释它们；looper 自己的
//     position/user/timer 布局仍在条件对象的 flags 里，互不干扰。
//     位图是 128 位，所以类型数上限为 128（aligned_iterator_storage 里有
//     static_assert；超过时直接拒绝而不是给出错误的 safe()）。
// ============================================================================

#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <istream>
#include <limits>
#include <new>
#include <ostream>
#include <tuple>
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
// 0) 引用归一化
// ============================================================================
// 模板层面写引用时自动换成 std::reference_wrapper：
//   Ty&         -> std::reference_wrapper<Ty>
//   Ty const&   -> std::reference_wrapper<Ty const>
// 只搬类型，不做值层面的包装：构造/赋值时仍然要用户自己写
// std::ref / std::cref，这样“这一格存的是引用”始终是显式的。
// 对非引用类型是恒等变换，所以可以重复套用。
template <class T>
using stored_type_t =
    std::conditional_t<std::is_reference_v<T>,
                       std::reference_wrapper<std::remove_reference_t<T>>, T>;

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

  // 两边都只是已经成型的类型，仍然走惰性选择，与库的规则保持一致。
  template <class this_type, class from_ins>
  using apply = select_if_t<(alignof(this_type) < alignof(from_ins)),
                            always_fn<from_ins>, always_fn<this_type>>;
};

using larger_alignment_calculator = meta_object<larger_alignment_f>;

namespace layout_details {

template <std::size_t Alignment, class... Ty>
using aligned_receiver_after_all =
    typename meta_states::run<meta_ios::stream_states_co<>,
                              meta_aligned_iterator<Alignment>,
                              meta_istream_list<Ty...>>::type::to;

template <class... Ty>
using largest_aligned_type =
    typename meta_states::run<meta_ios::stream_states_co<>,
                              larger_alignment_calculator,
                              meta_istream_list<Ty...>>::type::to_t;

}  // namespace layout_details

// 整个布局的字节数：最后一个槽位的 advance，再按布局的对齐边界收尾。
// Alignment == 0 → 用"最大类型对齐"收尾（内存布局，末尾 padding 算进去）；
// Alignment != 0 → 统一用这个边界收尾（1 = packed，没有尾部 padding）。
// 引用先归一化成 std::reference_wrapper（指针大小、指针对齐）。
template <std::size_t Alignment = 0, class... Ty>
consteval std::size_t field_size() {
  static_assert(sizeof...(Ty) > 0, "field_size requires at least one type");
  static_assert(Alignment == 0 || std::has_single_bit(Alignment),
                "field_size alignment must be 0 or a power of two");
  using end_seek = layout_details::aligned_receiver_after_all<
      Alignment, stored_type_t<Ty>...>;
  constexpr std::size_t raw_end = end_seek::type::advance_t::value;
  constexpr std::size_t align =
      Alignment == 0
          ? alignof(layout_details::largest_aligned_type<stored_type_t<Ty>...>)
          : Alignment;
  return raw_end + ((0 - raw_end) & (align - 1));
}

// ============================================================================
// 2) type_mask：把 MSO 放在 rec 位跑一次类型流
// ============================================================================
//
// 旧版用 same_states_iterator + transfer_until 得到一串"变化历史位"。
// 新版不需要 changed_pred：接收器自己就是位图，位图就是 MSO 自己的 flags。
//
// flags 已经是 128 位（meta_objects::u128），位图也用满 128 位：
// 不再开计数字段去算"第几位"，而是每步 flags = (flags << 1) | 命中位。
// 走完 N 个类型后得到的是反序位图（第 i 个类型落在 bit N-1-i），
// 最后按 N 位反转一次，即 bit i = 第 i 个类型命中。
// 类型上限因此只由位图宽度决定：64 -> 128。

namespace mask_details {

// 槽位上的类型 U 能不能当 T 用：只放行“加 const”这一个方向。
//   U = int,       T = int        -> 可以（完全一致）
//   U = int,       T = const int  -> 可以（& 到 const& 的限定转换）
//   U = const int, T = int        -> 不可以（会去掉 const，反向不放行）
// 引用本身没有 cv，const 加在所指类型上，所以统一 remove_reference 之后再判 const。
template <class T, class U>
inline constexpr bool const_convertible =
    std::is_same_v<std::remove_cvref_t<T>, std::remove_cvref_t<U>> &&
    (std::is_const_v<std::remove_reference_t<T>> ||
     !std::is_const_v<std::remove_reference_t<U>>);

// 累积一步：左移一位，低位写命中。N 步后得到反序位图。
template <class T>
struct convertible_type_mask_fn {
  template <class flags, class obj>
  struct apply {
    static constexpr bool match = const_convertible<T, meta_produce_t<obj>>;
    static constexpr u128 value = [] {
      u128 shifted = flags::value << 1;
      if constexpr (match) {
        shifted.low |= 1;
      }
      return shifted;
    }();
  };
};

template <class T>
using convertible_type_mask_o =
    meta_states_object<0, convertible_type_mask_fn<T>>;

// 64 位按位反转（SWAR），只在编译期算位图时用一次。
constexpr std::uint64_t reverse_bits64(std::uint64_t x) noexcept {
  x = ((x >> 1) & 0x5555'5555'5555'5555ull) |
      ((x & 0x5555'5555'5555'5555ull) << 1);
  x = ((x >> 2) & 0x3333'3333'3333'3333ull) |
      ((x & 0x3333'3333'3333'3333ull) << 2);
  x = ((x >> 4) & 0x0F0F'0F0F'0F0F'0F0Full) |
      ((x & 0x0F0F'0F0F'0F0F'0F0Full) << 4);
  x = ((x >> 8) & 0x00FF'00FF'00FF'00FFull) |
      ((x & 0x00FF'00FF'00FF'00FFull) << 8);
  x = ((x >> 16) & 0x0000'FFFF'0000'FFFFull) |
      ((x & 0x0000'FFFF'0000'FFFFull) << 16);
  return (x >> 32) | (x << 32);
}

// 把低 N 位（0 <= N <= 128）按位反转，结果仍在低 N 位。
// N <= 64 时只有 low 有意义；N > 64 时两半各自反转后按 N 对齐拼接。
template <std::size_t N>
constexpr u128 reverse_low_bits(u128 value) noexcept {
  if constexpr (N == 0) {
    return {};
  } else if constexpr (N <= 64) {
    return {reverse_bits64(value.low) >> (64 - N)};
  } else {
    // 两半分别反转后按 N 对齐；移位统一用 u128 的左移，
    // 这样 N == 128 时不会出现 64 位左移 64 位的 UB。
    return (u128{reverse_bits64(value.low)} << (N - 64)) |
           u128{reverse_bits64(value.high) >> (128 - N)};
  }
}

}  // namespace mask_details

// bit i 为 1 <=> Ty[i] 能当 T 用：完全相同，或者 Ty[i] 加上 const 之后等于 T。
// 加 const 的方向算匹配（& -> const&），去 const 的方向不匹配。
// typelist 里可以重复出现 T；上限 128 个类型（位图就是 flags 的 128 位）。
template <class T, class... Ty>
inline constexpr u128 type_mask_series = [] {
  using run_t = meta_states::run<meta_ios::stream_states_co<>,
                                 mask_details::convertible_type_mask_o<T>,
                                 meta_istream_list<Ty...>>;
  // 累积出来的是反序位图，按 N 位反转回正常顺序。
  return mask_details::reverse_low_bits<sizeof...(Ty)>(
      run_t::type::to_t::value);
}();

// 与 type_mask_series 同一套判据的“包内成员”判断：
// T 能不能从 Ty... 的某个槽位读出来（加 const 可以，去 const 不行）。
template <class T, class... Ty>
inline constexpr bool is_slot_convertible =
    (mask_details::const_convertible<T, Ty> || ...);

// ============================================================================
// 3) 单个槽的受检代理
// ============================================================================
// 判据是“槽位类型 U 能不能当 T 用”，只放行加 const 的方向：
//   U == T            -> 可以
//   U 加 const 后 == T -> 可以（& 到 const& 的限定转换）
//   U 去 const 后 == T -> 不可以（const& 到 & 的反向转换被拒绝）
// Ty... 里的引用先归一化成 std::reference_wrapper（stored_type_t），
// 所以 aligned_iterator_storage<int&> 等价于 <std::reference_wrapper<int>>。
template <class... Ty>
struct aligned_iterator_storage {
  // type_mask_series 现在用满 flags 的 128 位；超过 128 个类型时位图表达不了。
  // 这里直接拒绝，而不是静默给出错误的 safe()。
  static_assert(sizeof...(Ty) <= 128,
                "aligned_iterator_storage: at most 128 types "
                "(type_mask_series is a 128-bit mask)");

  enum class error {
    null_pointer, type_not_safe, index_exceed,
    unsupported_operation, arithmetic_overflow, division_by_zero
  };

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
        case error::unsupported_operation:
          return "aligned_iterator_storage: unsupported slot operation";
        case error::arithmetic_overflow:
          return "aligned_iterator_storage: arithmetic write exceeds slot range";
        case error::division_by_zero:
          return "aligned_iterator_storage: division by zero";
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

  // 拷贝构造显式 default：operator[] 按值返回代理本来就依赖它，而删除了
  // copy assignment 后隐式拷贝构造会被 clang 报 -Wdeprecated-copy。
  // 一旦声明了拷贝构造，默认构造与两参数聚合式构造也要显式补回来。
  aligned_iterator_storage() = default;
  aligned_iterator_storage(std::byte* base, std::size_t type_index) noexcept
      : ptr(base), m_type_index(type_index) {}
  aligned_iterator_storage(aligned_iterator_storage const&) = default;

  // T 在该槽位上是否被允许。T 必须是 Ty... 中的一种（或加 const 后的版本）；
  // 越界/空指针抛异常。
  template <class T>
  bool safe() const {
    if constexpr (!slot_type_allowed<T>()) {
      static_assert(slot_type_allowed<T>(),
                    "T must be one of aligned_iterator_storage's types "
                    "(or a const-qualified version of one)");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      return slot_mask_hit<T>();
    }
  }

  // 在当前槽位上用 placement new 开始 T 的生命周期。
  template <class T>
  bool emplace(T&& val) {
    using value_type = std::remove_cvref_t<T>;
    if constexpr (!slot_type_allowed<value_type>()) {
      static_assert(slot_type_allowed<value_type>(),
                    "T must be one of aligned_iterator_storage's types");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      if (!slot_mask_hit<value_type>()) {
        throw exception(error::type_not_safe);
      }
      ::new (static_cast<void*>(ptr)) value_type(std::forward<T>(val));
      return true;
    }
  }

  // 受检地取当前槽位上的 T&（允许加 const：int 槽位可以读成 const int&）。
  template <class T>
  operator T&() const {
    if constexpr (!slot_type_allowed<T>()) {
      static_assert(slot_type_allowed<T>(),
                    "T must be one of aligned_iterator_storage's types "
                    "(or a const-qualified version of one)");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      if (!slot_mask_hit<T>()) {
        throw exception(error::type_not_safe);
      }
      return *reinterpret_cast<T*>(ptr);
    }
  }

  // 受检地给当前槽位上已经构造的 T 赋值。
  // 写入不走“加 const”的匹配（T 必须是槽位类型本身，不能是它的 const 版本）。
  template <class T>
  void operator=(T const& val) {
    using value_type = std::remove_cv_t<T>;
    if constexpr (!slot_type_allowed<value_type>()) {
      static_assert(slot_type_allowed<value_type>(),
                    "T must be one of aligned_iterator_storage's types");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      if (!slot_mask_hit<value_type>()) {
        throw exception(error::type_not_safe);
      }
      *reinterpret_cast<value_type*>(ptr) = val;
    }
  }

  template <class T>
  T& as() const {
    if (!safe<T>()) {
      throw exception(error::type_not_safe);
    }
    return *std::launder(reinterpret_cast<std::remove_reference_t<T>*>(ptr));
  }

  template <class U> requires std::same_as<U, std::uint64_t>
  double operator+(U k) const {
    check_runtime();
    return operations::add[m_type_index](ptr, k);
  }

  template <class U> requires std::same_as<U, std::uint64_t>
  double operator-(U k) const {
    check_runtime();
    return operations::subtract[m_type_index](ptr, k);
  }

  template <class U> requires std::same_as<U, std::uint64_t>
  double operator*(U k) const {
    check_runtime();
    return operations::multiply[m_type_index](ptr, k);
  }

  template <class U> requires std::same_as<U, std::uint64_t>
  double operator/(U k) const {
    check_runtime();
    return operations::divide[m_type_index](ptr, k);
  }

  template <class U> requires std::same_as<U, std::uint64_t>
  std::partial_ordering operator<=>(U k) const {
    check_runtime();
    return operations::compare[m_type_index](ptr, k);
  }

  template <class U> requires std::same_as<U, std::uint64_t>
  bool operator==(U k) const {
    return (*this <=> k) == 0;
  }

  template <class U> requires std::same_as<U, std::uint64_t>
  aligned_iterator_storage& operator+=(U k) {
    check_runtime();
    operations::add_assign[m_type_index](ptr, k);
    return *this;
  }

  template <class U> requires std::same_as<U, std::uint64_t>
  aligned_iterator_storage& operator-=(U k) {
    check_runtime();
    operations::subtract_assign[m_type_index](ptr, k);
    return *this;
  }

 private:
  void check_runtime() const {
    if (m_type_index >= sizeof...(Ty)) {
      throw exception(error::index_exceed);
    }
    if (ptr == nullptr) {
      throw exception(error::null_pointer);
    }
  }

  template <class T>
  struct op_set {
    template <char Op>
    static double arithmetic(std::byte* slot, std::uint64_t k) {
      if constexpr (std::is_arithmetic_v<T>) {
        if constexpr (Op == '/') {
          if (k == 0) {
            throw exception(error::division_by_zero);
          }
        }
        const double value = static_cast<double>(
            *std::launder(reinterpret_cast<T*>(slot)));
        const double rhs = static_cast<double>(k);
        if constexpr (Op == '+') return value + rhs;
        if constexpr (Op == '-') return value - rhs;
        if constexpr (Op == '*') return value * rhs;
        if constexpr (Op == '/') return value / rhs;
      } else {
        throw exception(error::unsupported_operation);
      }
    }

    static std::partial_ordering compare(std::byte* slot, std::uint64_t k) {
      if constexpr (std::is_arithmetic_v<T>) {
        const auto value = *std::launder(reinterpret_cast<T*>(slot));
        if constexpr (std::is_floating_point_v<T>) {
          if (std::isnan(value)) return std::partial_ordering::unordered;
          if (value < 0) return std::partial_ordering::less;
          // Check before converting: UINT64_MAX rounds up to 2^64 in double.
          if (value >= static_cast<T>(0x1p64L)) {
            return std::partial_ordering::greater;
          }
          const auto integer = static_cast<std::uint64_t>(value);
          if (integer < k) return std::partial_ordering::less;
          if (integer > k) return std::partial_ordering::greater;
          return value == static_cast<T>(integer)
              ? std::partial_ordering::equivalent
              : std::partial_ordering::greater;
        } else {
          if constexpr (std::is_signed_v<T>) {
            if (value < 0) return std::partial_ordering::less;
          }
          return static_cast<std::uint64_t>(value) <=> k;
        }
      } else {
        throw exception(error::unsupported_operation);
      }
    }

    template <bool Subtract>
    static void update(std::byte* slot, std::uint64_t k) {
      if constexpr (std::is_arithmetic_v<T> && !std::is_const_v<T>) {
        auto& target = *std::launder(reinterpret_cast<T*>(slot));
        using value_type = std::remove_cv_t<T>;
        const value_type value = target;
        if constexpr (std::is_integral_v<T>) {
          bool negative = false;
          auto magnitude = static_cast<std::uint64_t>(value);
          if constexpr (std::is_signed_v<T>) {
            if (value < 0) {
              negative = true;
              magnitude = std::uint64_t{0} - magnitude;
            }
          }
          // Sign/magnitude arithmetic avoids signed overflow and rhs narrowing.
          if (negative == Subtract) {
            if (k > std::numeric_limits<std::uint64_t>::max() - magnitude) {
              throw exception(error::arithmetic_overflow);
            }
            magnitude += k;
          } else if (k > magnitude) {
            magnitude = k - magnitude;
            negative = !negative;
          } else {
            magnitude -= k;
          }
          if (negative && magnitude != 0) {
            if constexpr (std::is_signed_v<T>) {
              constexpr auto limit = std::uint64_t{0} -
                  static_cast<std::uint64_t>(std::numeric_limits<T>::min());
              if (magnitude > limit) {
                throw exception(error::arithmetic_overflow);
              }
              target = static_cast<value_type>(
                  -1 - static_cast<value_type>(magnitude - 1));
            } else {
              throw exception(error::arithmetic_overflow);
            }
          } else {
            if (magnitude > static_cast<std::uint64_t>(
                                std::numeric_limits<T>::max())) {
              throw exception(error::arithmetic_overflow);
            }
            target = static_cast<value_type>(magnitude);
          }
        } else {
          const value_type rhs = static_cast<value_type>(k);
          const value_type result = Subtract ? value - rhs : value + rhs;
          if (std::isfinite(value) && !std::isfinite(result)) {
            throw exception(error::arithmetic_overflow);
          }
          target = result;
        }
      } else {
        throw exception(error::unsupported_operation);
      }
    }
  };

  template <class... Ops>
  struct operation_tables {
    using arithmetic_fn = double (*)(std::byte*, std::uint64_t);
    using compare_fn = std::partial_ordering (*)(std::byte*, std::uint64_t);
    using update_fn = void (*)(std::byte*, std::uint64_t);

    inline static constexpr std::array<arithmetic_fn, sizeof...(Ops)> add = {
        &Ops::template arithmetic<'+'>...};
    inline static constexpr std::array<arithmetic_fn, sizeof...(Ops)> subtract = {
        &Ops::template arithmetic<'-'>...};
    inline static constexpr std::array<arithmetic_fn, sizeof...(Ops)> multiply = {
        &Ops::template arithmetic<'*'>...};
    inline static constexpr std::array<arithmetic_fn, sizeof...(Ops)> divide = {
        &Ops::template arithmetic<'/'>...};
    inline static constexpr std::array<compare_fn, sizeof...(Ops)> compare = {
        &Ops::compare...};
    inline static constexpr std::array<update_fn, sizeof...(Ops)> add_assign = {
        &Ops::template update<false>...};
    inline static constexpr std::array<update_fn, sizeof...(Ops)> subtract_assign = {
        &Ops::template update<true>...};
  };

  using op_sets =
      typename exp_list<stored_type_t<Ty>...>::template for_each<op_set>;
  using operations = typename op_sets::template to<operation_tables>;

  // 槽位判据统一走这里：Ty... 先归一化（Ty& -> std::reference_wrapper<Ty>）。
  template <class T>
  static constexpr bool slot_type_allowed() {
    return is_slot_convertible<T, stored_type_t<Ty>...>;
  }

  // 当前槽位能否当 T 用（只看位图；调用方已保证 index 在范围内）。
  // <= 64 个类型时仍然是一条 64 位移位（与 64 位旧版完全一致）；
  // 更宽的列表才用高低两半取位，index & 63 同时完成"减 64"。
  template <class T>
  bool slot_mask_hit() const {
    constexpr u128 mask = type_mask_series<T, stored_type_t<Ty>...>;
    if constexpr (sizeof...(Ty) <= 64) {
      return ((mask.low >> m_type_index) & 1) != 0;
    } else {
      const std::uint64_t half = m_type_index < 64 ? mask.low : mask.high;
      return ((half >> (m_type_index & 63)) & 1) != 0;
    }
  }
};

// ============================================================================
// 4) 正向 / 反向迭代器
// ============================================================================

// 旧写法：meta_pipe<...>::all_to<meta_aligned_iterator<>>::run<meta_ostream<>>::type::to_t
// 新写法完全相同——pipe 就是按这个形状实现的。
// Ty... 先归一化：写 int& 等价于写 std::reference_wrapper<int>。
template <std::size_t Alignment = 0, class... Ty>
using aligned_offset_stream =
    typename meta_pipe<meta_istream_list<stored_type_t<Ty>...>>
        ::template all_to<meta_aligned_iterator<Alignment>>
        ::template run<meta_ostream<>>::type::to_t;

// 反向偏移流：meta_rostream 把每一步的 seek_to 从前面插入，所以它给出的列表
// 已经是“逆序的绝对坐标表”：第 j 项就是第 j 个反向槽位的 seek_to，
// value 是相对缓冲区起点的绝对偏移，type 是该槽位的类型，且自带 destroy。
template <std::size_t Alignment = 0, class... Ty>
using reverse_aligned_offset_stream =
    typename meta_pipe<meta_istream_list<stored_type_t<Ty>...>>
        ::template all_to<meta_aligned_iterator<Alignment>>
        ::template run<meta_rostream<>>::type::to::type;

// 布局能不能安全访问：Alignment 是否保证每一格都对上 alignof(T)，或者目标本身
// 允许未对齐访问。弱对齐 CPU（老 ARM/MIPS/SPARC 等）上 packed 布局直接编译期报错，
// 而不是运行时 fault。判据与 meta_iostream::layout_is_aligned 一致：alignof(T) | Alignment。
// 注意：type_safe_aligned_range 走的是引用语义（get/emplace 直接解引用），所以它
// 确实需要这个检查；线上数据请用 meta_wire_iterator（按值 + 字节拷贝，不解引用）。
template <std::size_t Alignment, class... Ty>
inline constexpr bool layout_access_ok =
    meta_iostream::unaligned_access_ok ||
    (meta_iostream::layout_is_aligned<stored_type_t<Ty>, Alignment> && ...);

namespace layout_detail {

// 对齐布局的静态事实：两个迭代器共用这一份，地址一律按“绝对偏移”取。
template <std::size_t Alignment, class... Ty>
struct aligned_layout {
  // offsets[i] 是第 i 个槽位相对缓冲起点的“绝对”字节偏移（不是相邻差值）。
  static constexpr auto offsets =
      to_meta_array_t<aligned_offset_stream<Alignment, Ty...>>::array();
  static constexpr std::size_t size = sizeof...(Ty);
  // 布局总字节数 = 最后一个槽位之后的位置，也是 end / rend 的位置。
  static constexpr std::size_t total_size = field_size<Alignment, Ty...>();

  static constexpr std::size_t offset_at(std::size_t index) {
    return index == size ? total_size : offsets[index];
  }
};

// 槽位坐标表：seek_to 自己就带着绝对偏移与类型，
// 析构时直接让它干活，不必再跑一遍类型流去反查“这一格是哪种类型”。
template <class SeekList, class IndexSeq>
struct slot_table;

template <class... Seek, std::size_t... I>
struct slot_table<exp_list<Seek...>, std::index_sequence<I...>> {
  static void destroy_at(std::size_t index, std::byte* base_ptr) {
    // 命中的那一格会被销毁；fold 的布尔结果本身没有用途，显式丢掉。
    // 这里不能靠 [[maybe_unused]]（它修饰不了语句），clang 的 -Wunused-value
    // 需要的是把结果转成 void。
    (void)((index == I ? (Seek{}.destroy(base_ptr), true) : false) || ...);
  }
};

// 坐标表里每一项都记得自己那一格的类型（seek_to<绝对偏移, T>::type）。
template <class Seek>
using seek_type = typename Seek::type;

}  // namespace layout_detail

template <std::size_t Alignment = 0, class... Ty>
class type_safe_aligned_iterator {
  static_assert(sizeof...(Ty) > 0,
                "type_safe_aligned_iterator requires at least one type");
  static_assert(layout_access_ok<Alignment, Ty...>,
                "type_safe_aligned_iterator: packed layout on a target without "
                "unaligned access (use meta_wire_iterator for wire data, or "
                "alignment >= alignof(T))");

  using layout = layout_detail::aligned_layout<Alignment, Ty...>;

  std::byte* base_ptr_{nullptr};
  // storage_.m_type_index 是唯一索引；storage_.ptr 始终 = base + offsets[槽位号]，
  // 即“绝对偏移缓存”，不再用相邻差值累加。
  aligned_iterator_storage<Ty...> storage_{};

  void refresh() {
    storage_.ptr = base_ptr_ == nullptr
                       ? nullptr
                       : base_ptr_ + layout::offset_at(storage_.m_type_index);
  }

  // 前进一个槽位：只查绝对偏移表重新定位，不拿“后一个偏移 - 前一个偏移”累加。
  type_safe_aligned_iterator& advance() {
    using storage_type = aligned_iterator_storage<Ty...>;
    if (storage_.m_type_index >= layout::size) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    if (base_ptr_ == nullptr) {
      throw typename storage_type::exception(storage_type::error::null_pointer);
    }
    ++storage_.m_type_index;
    refresh();
    return *this;
  }

 public:
  // 构造函数是 explicit 的：必须同时给出缓冲区起点和槽位号。
  // type_index == sizeof...(Ty) 表示 end()；空指针保留到访问槽位时才诊断。
  explicit type_safe_aligned_iterator(std::byte* base_ptr,
                                      std::size_t type_index) : base_ptr_(base_ptr) {
    if (type_index > layout::size) {
      using storage_type = aligned_iterator_storage<Ty...>;
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    storage_.m_type_index = type_index;
    refresh();
  }

  // 解引用给出当前槽位的受检代理（ptr 已按绝对偏移定位）。
  aligned_iterator_storage<Ty...>& operator*() { return storage_; }

  // 前缀 ++：移动到对齐布局里的下一个槽位。
  type_safe_aligned_iterator& operator++() { return advance(); }

  bool operator==(type_safe_aligned_iterator const& other) const {
    return base_ptr_ == other.base_ptr_ &&
           storage_.m_type_index == other.storage_.m_type_index;
  }

  bool operator!=(type_safe_aligned_iterator const& other) const {
    return !(*this == other);
  }
};

// 反向迭代器：直接吃 meta_rostream 给出的“逆序绝对坐标表”。
//   - 状态就是反向序号：++ 递增、rend 递增到 sizeof...(Ty)，与 std::reverse_iterator 同语义；
//   - 地址取同一张表的 offsets[j]（绝对偏移），不经过正向表；
//   - 受检代理按“逆序类型列表”实例化，所以槽位号 j 就是它位图的位号，
//     不需要把 j 换算成正向序号；
//   - destroy 直接让该槽位的 seek_to 干活。
template <std::size_t Alignment = 0, class... Ty>
class type_safe_aligned_reverse_iterator {
  static_assert(sizeof...(Ty) > 0,
                "type_safe_aligned_reverse_iterator requires at least one type");
  static_assert(layout_access_ok<Alignment, Ty...>,
                "type_safe_aligned_reverse_iterator: packed layout on a target "
                "without unaligned access (use meta_wire_iterator for wire data, "
                "or alignment >= alignof(T))");

  using seek_list = reverse_aligned_offset_stream<Alignment, Ty...>;
  static constexpr auto offsets = to_meta_array_t<seek_list>::array();
  // 逆序类型列表：反向槽位 j <-> aligned_iterator_storage<Ty_reversed...> 的第 j 位。
  using reversed_types =
      typename seek_list::template for_each<layout_detail::seek_type>;
  using proxy_type = typename reversed_types::template to<aligned_iterator_storage>;
  using slot_table =
      layout_detail::slot_table<seek_list, std::index_sequence_for<Ty...>>;

  std::byte* base_ptr_{nullptr};
  // 反向序号：0 = 布局的最后一格，sizeof...(Ty) = rend。
  std::size_t reverse_index_{0};
  // operator* 物化的受检代理（引用语义，所以留作成员）。
  proxy_type storage_{};

 public:
  explicit type_safe_aligned_reverse_iterator(std::byte* base_ptr,
                                              std::size_t reverse_index)
      : base_ptr_(base_ptr), reverse_index_(reverse_index) {
    if (reverse_index > sizeof...(Ty)) {
      using storage_type = proxy_type;
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
  }

  // 解引用：第 j 项本身就是第 j 个反向槽位的绝对偏移，槽位号也原样就是 j。
  proxy_type& operator*() {
    if (reverse_index_ == sizeof...(Ty)) {
      storage_.ptr = base_ptr_;
    } else {
      storage_.ptr = base_ptr_ == nullptr
                         ? nullptr
                         : base_ptr_ + offsets[reverse_index_];
    }
    storage_.m_type_index = reverse_index_;
    return storage_;
  }

  // 销毁当前槽位上的对象：坐标对象自己带偏移与类型。
  void destroy() {
    using storage_type = proxy_type;
    if (reverse_index_ >= sizeof...(Ty)) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    if (base_ptr_ == nullptr) {
      throw typename storage_type::exception(storage_type::error::null_pointer);
    }
    slot_table::destroy_at(reverse_index_, base_ptr_);
  }

  type_safe_aligned_reverse_iterator& operator++() {
    using storage_type = proxy_type;
    if (reverse_index_ >= sizeof...(Ty)) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    if (base_ptr_ == nullptr) {
      throw typename storage_type::exception(storage_type::error::null_pointer);
    }
    ++reverse_index_;
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
// Alignment 和迭代器/field_size 是同一个参数：0 = 每格 alignof（内存布局），
// 2 的幂 = 统一边界（1 = packed）。initialize / begin / rbegin 都跟着它走。
template <std::size_t Alignment = 0, class... Ty>
class type_safe_aligned_range {
  static_assert(layout_access_ok<Alignment, Ty...>,
                "type_safe_aligned_range: packed layout on a target without "
                "unaligned access (use meta_wire_iterator for wire data, or "
                "alignment >= alignof(T))");

 public:
  // base_ptr 必须指向足够容纳整个 Ty... 布局的存储。
  // Ty... 里可以直接写引用（如 int&）：布局与存储都按
  // std::reference_wrapper 算，但值层面仍要传 std::ref / std::cref。
  explicit type_safe_aligned_range(std::byte* base_ptr) : base_ptr_(base_ptr) {}

  type_safe_aligned_iterator<Alignment, Ty...> begin() const {
    return type_safe_aligned_iterator<Alignment, Ty...>(base_ptr_, 0);
  }

  type_safe_aligned_iterator<Alignment, Ty...> end() const {
    return type_safe_aligned_iterator<Alignment, Ty...>(base_ptr_,
                                                       sizeof...(Ty));
  }

  type_safe_aligned_reverse_iterator<Alignment, Ty...> rbegin() const {
    return type_safe_aligned_reverse_iterator<Alignment, Ty...>(base_ptr_, 0);
  }

  type_safe_aligned_reverse_iterator<Alignment, Ty...> rend() const {
    return type_safe_aligned_reverse_iterator<Alignment, Ty...>(
        base_ptr_, sizeof...(Ty));
  }

  std::byte* data() const { return base_ptr_; }

  // 槽位数（布局里的元素个数）。
  static constexpr std::size_t size() noexcept { return sizeof...(Ty); }

  // ---- 运行期下标：r[i] ----
  // 返回“受检代理”**按值**（两个机器字：ptr + 槽位号）。
  //
  // 为什么不返回 Ty_i&：这一格是异构的，range 的元素没有一个统一的类型。
  // 返回引用就必须在 range 里存一个可变的代理成员（const 对象里的可变状态、
  // 两个迭代器共享同一块代理、并发下互相踩），所以按值给。
  //
  // 这与 range 语义不冲突：std::array 同时有 operator[] 和 std::get<I>，
  // 这里只是把“运行期均匀访问”和“编译期精确类型访问”分开两个入口
  //（运行期给不出 Ty_i& —— 见下面的 get<I>）。
  //
  // 解引用后仍然是同一套检查：越界在 operator[] 就抛，空指针/类型不符由代理抛。
  aligned_iterator_storage<Ty...> operator[](std::size_t index) const {
    using storage_type = aligned_iterator_storage<Ty...>;
    if (index >= sizeof...(Ty)) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    // 迭代器构造里做的就是“按绝对偏移表定位”，与 ++ 走的是同一张表。
    type_safe_aligned_iterator<Alignment, Ty...> it(base_ptr_, index);
    return *it;  // 拷贝出来：it 是临时对象，引用不能带出去
  }

  // 类型已知、下标仍在运行期：唯一匹配先验证槽位，再用常量绝对偏移访问。
  // 异常优先级与代理一致：越界、空指针、类型不匹配。
  template <class T>
  T& at(std::size_t index) const {
    using storage_type = aligned_iterator_storage<Ty...>;
    using exception = typename storage_type::exception;
    if constexpr (!is_slot_convertible<T, stored_type_t<Ty>...>) {
      static_assert(is_slot_convertible<T, stored_type_t<Ty>...>,
                    "T must be one of aligned_iterator_storage's types "
                    "(or a const-qualified version of one)");
    } else {
      constexpr u128 mask = type_mask_series<T, stored_type_t<Ty>...>;
      constexpr bool unique =
          (mask.high == 0 && std::has_single_bit(mask.low)) ||
          (mask.low == 0 && std::has_single_bit(mask.high));
      if constexpr (unique) {
        constexpr std::size_t slot = mask.low != 0
            ? std::countr_zero(mask.low)
            : 64 + std::countr_zero(mask.high);
        // 命中唯一槽位也证明未越界；失败分支仍按原优先级报告。
        if (index != slot || base_ptr_ == nullptr) {
          throw exception(index >= sizeof...(Ty) ? storage_type::error::index_exceed
              : base_ptr_ == nullptr ? storage_type::error::null_pointer
                                    : storage_type::error::type_not_safe);
        }
        using cursor =
            typename aligned_offset_stream<Alignment, Ty...>::template at<slot>;
        return *std::launder(reinterpret_cast<std::remove_reference_t<T>*>(
            cursor{}.address_of(base_ptr_)));
      } else {
        return static_cast<T&>((*this)[index]);
      }
    }
  }

  // ---- 编译期下标：r.get<I>() / get<I>(r) / 结构化绑定 ----
  // 返回该槽位上的**精确类型引用**（Ty... 归一化后的那个类型）。
  // 游标就是 slot_at<I> 用的同一个 seek_to：base + 常量偏移 + launder，
  // 没有任何运行期检查，也不会因为 Alignment 而多一次查表。
  //
  // 注意 const：range 是视图，constness 不往元素上传播（与 std::span 一致，
  // data() const 本来就返回可写指针），所以 const range 上 get<I>() 仍然是 T&。
  template <std::size_t I>
  auto& get() const noexcept {
    static_assert(I < sizeof...(Ty), "type_safe_aligned_range::get<I>: slot index out of range");
    using cursor = typename aligned_offset_stream<Alignment, Ty...>::template at<I>;
    return cursor{}.get(base_ptr_);
  }

 private:
  std::byte* base_ptr_;
};

// 第 I 格的游标（就是那一格的 seek_to）：偏移是编译期常量，运行时只剩一次
// `base + value`。索引是编译期常量时不要用运行期迭代器挪 I 步。
template <std::size_t I, class Range>
struct slot_cursor;

template <std::size_t I, std::size_t Alignment, class... Ty>
struct slot_cursor<I, type_safe_aligned_range<Alignment, Ty...>> {
  static_assert(I < sizeof...(Ty), "slot_cursor: slot index out of range");
  using type = typename aligned_offset_stream<Alignment, Ty...>::template at<I>;
};

template <std::size_t I, class Range>
using slot_cursor_t = typename slot_cursor<I, Range>::type;

// 便捷写法：slot_at<I>(range).get(range.data()) / .address_of(range.data())
// range 只参与类型推导，运行期没有任何额外代价。
template <std::size_t I, class Range>
constexpr slot_cursor_t<I, Range> slot_at(Range const&) noexcept {
  return {};
}

// get<I>(range)：编译期下标的 ADL 入口，等价于 range.get<I>()。
// 结构化绑定与泛型代码走这个（配合文件末尾的 tuple_size / tuple_element）。
template <std::size_t I, std::size_t Alignment, class... Ty>
auto& get(type_safe_aligned_range<Alignment, Ty...> const& range) noexcept {
  static_assert(I < sizeof...(Ty), "get<I>(range): slot index out of range");
  return range.template get<I>();
}


// ============================================================================
// 6) 构造全部元素（异常时按逆序清理已经构造的部分）
// ============================================================================
template <std::size_t Alignment, class... Ty, class... Args>
void initialize(type_safe_aligned_range<Alignment, Ty...> const& range,
                Args&&... args) {
  static_assert(sizeof...(Ty) == sizeof...(Args),
                "initialize requires one argument for each range element");
  auto* data = range.data();
  // 引用槽位归一化成 std::reference_wrapper：
  // 所以这里必须传 std::ref(x) / std::cref(x)，传 x 本身会构造失败。
  meta_states::run<meta_ios::stream_states_co<>,
                   meta_aligned_iterator<Alignment>,
                   meta_istream_list<stored_type_t<Ty>...>>::for_each_forward(
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
                   meta_istream_list<stored_type_t<Ty>...>>::for_each(
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
                   meta_istream_list<stored_type_t<Ty>...>>::for_each(
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

// ============================================================================
// 7) tuple-like 协议：结构化绑定 / std::tuple_size / std::tuple_element
// ============================================================================
// type_safe_aligned_range 同时是两样东西：
//   - range：begin/end/rbegin/rend + 运行期 r[i]（均匀的受检代理）
//   - tuple-like：编译期 get<I>(r)（精确类型引用，零检查）
// std::array 就是同一个形状 —— operator[] 与 get<I> 并存，两者不冲突，
// 因为“运行期均匀访问”和“编译期异构访问”本就是两个不同的入口。
//
// 提供下面两个特化之后，结构化绑定可以直接绑到缓冲区里的真实对象：
//   auto& [magic, version, kind, flags, device, ts, value, length] = range;
//
// 视图语义：const range 的元素仍然可写（data() const 本来就返回可写指针，
// 与 std::span 一致），所以 const 版本映射到同一个元素类型，而不是 const T。
// 这也让 auto const& [..] = range; 的绑定仍然是可写的，与 get<I>() 行为一致。
namespace std {

template <std::size_t Alignment, class... Ty>
struct tuple_size<type_safe_aligned::type_safe_aligned_range<Alignment, Ty...>>
    : integral_constant<std::size_t, sizeof...(Ty)> {};

template <std::size_t Alignment, class... Ty>
struct tuple_size<
    type_safe_aligned::type_safe_aligned_range<Alignment, Ty...> const>
    : tuple_size<type_safe_aligned::type_safe_aligned_range<Alignment, Ty...>> {
};

template <std::size_t I, std::size_t Alignment, class... Ty>
struct tuple_element<
    I, type_safe_aligned::type_safe_aligned_range<Alignment, Ty...>> {
  static_assert(I < sizeof...(Ty), "tuple_element: slot index out of range");
  // at<I> 就是那一格的 seek_to，它的 type 是归一化后的存储类型。
  using type = typename type_safe_aligned::aligned_offset_stream<
      Alignment, Ty...>::template at<I>::type;
};

template <std::size_t I, std::size_t Alignment, class... Ty>
struct tuple_element<
    I, type_safe_aligned::type_safe_aligned_range<Alignment, Ty...> const>
    : tuple_element<I,
                    type_safe_aligned::type_safe_aligned_range<Alignment,
                                                              Ty...>> {};

}  // namespace std
