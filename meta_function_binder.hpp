#pragma once

// ============================================================================
// meta_function_binder.hpp —— 参数栈放在 type_safe_aligned_range 上的绑定器
//
// 对标 select_type 的 exp_function_binder，但参数栈不再是 exp_node 链表：
//
//   旧写法：每个参数在构造时 make_shared 一个节点（shared_ptr<ref_wrapper<T>>
//           + shared_ptr<next>），绑定/取值靠运行期沿链表递归；一个
//           exp_function_binder<int, double, std::string> 是几百字节，
//           构造时还要 N 次堆分配。
//
//   新写法：参数按“对齐布局”直接排在一块内联 std::byte 缓冲里，布局由本仓库
//           的 type_safe_aligned_range 计算（field_size / aligned_offset_stream
//           就是那张偏移表）；绑定用一个运行期槽位号在这块缓冲上做
//           placement new / 赋值，调用时用编译期下标 get<I> 直接展开。
//
// 于是：
//   - 没有任何堆分配，也没有指针 chase；
//   - binder 的大小 = 参数栈字节数 + 存活位图 + 游标 + 可调用对象；
//   - 参数在第一次 bind 时才构造（不要求默认可构造），调用后游标归零，
//     改某个参数用 operator[] / at<T>，重复调用不会重新构造。
//
// 参数槽映射（只影响栈里“存什么”，调用时按原签名还原）：
//   int                -> int                  （按值保存）
//   int&               -> int*                 （存地址，调用时解引用）
//   int const&         -> int                  （保存一份值，调用时传 const&）
//   std::string&&      -> std::string          （保存值，调用时 move）
//
// 与 type_safe_aligned_range 一致，第一个模板参数是 Alignment：
//   0（默认）= 每格按 alignof 对齐（内存布局）
//   1       = packed，参数栈没有 padding（栈更小，弱对齐目标会在编译期拒绝）
//
// 主要 API：
//   meta_bind::bind(f)                 从自由函数 / lambda / 函数对象推导签名
//   meta_bind::bind_as<R(L...)>(f)     签名推导不出来时（泛型 lambda）显式给
//   meta_bind::bind(obj, &T::member)   绑定成员函数（obj 必须在 binder 之前活着）
//   binder.bind(arg) / bind_a_lot(...) 依次写入下一个槽位
//   binder() / binder.apply_func()     调用；游标归零
//   binder[i] / binder.at<T>(i)        读/写已绑定的参数
//   binder >> / istream >> binder      从流读入下一个槽位
//
// 失败一律抛 meta_bind::exception（带 error code），不会静默写错槽位。
// ============================================================================

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <istream>
#include <memory>
#include <new>
#include <ostream>
#include <tuple>
#include <type_traits>
#include <utility>

#include "meta_utilities.hpp"
#include "type_safe_aligned_iterator.hpp"

namespace meta_bind {

using namespace exp_utilities;
using namespace type_safe_aligned;

template <std::size_t Alignment, class F, class R, class... L>
class function_binder;

// ============================================================================
// 0) 错误
// ============================================================================
enum class error {
  argument_overflow,  // 绑定的参数比签名允许的多
  index_exceed,       // 下标（槽位 / 函数序号）越界
  type_mismatch,      // 实参类型与当前槽位不匹配
  not_fully_bound,    // 还有槽位没有绑定就调用了
  argument_not_live,  // 访问了尚未构造的槽位
};

class exception : public std::exception {
 public:
  explicit exception(error code) noexcept : code_(code) {}
  error code() const noexcept { return code_; }

  const char* what() const noexcept override {
    switch (code_) {
      case error::argument_overflow:
        return "meta_bind: argument stack overflow";
      case error::index_exceed:
        return "meta_bind: index exceeds the binder/series";
      case error::type_mismatch:
        return "meta_bind: argument type does not match the slot";
      case error::not_fully_bound:
        return "meta_bind: not every argument slot is bound";
      case error::argument_not_live:
        return "meta_bind: argument slot is not constructed";
    }
    return "meta_bind: unknown error";
  }

 private:
  error code_;
};

namespace detail {

// ============================================================================
// 1) 槽位类型与调用表达式
// ============================================================================

template <class T>
struct is_reference_wrapper : std::false_type {};
template <class T>
struct is_reference_wrapper<std::reference_wrapper<T>> : std::true_type {};
template <class T>
inline constexpr bool is_reference_wrapper_v =
    is_reference_wrapper<std::remove_cvref_t<T>>::value;

// 空签名的占位槽：type_safe_aligned_range 至少要一个类型。
struct no_argument_slot {};

template <class... T>
inline constexpr std::size_t max_alignof_v = [] {
  std::size_t result = 1;
  ((result = alignof(T) > result ? alignof(T) : result), ...);
  return result;
}();

// 形参 -> 栈里实际保存的类型。
template <class L>
struct slot_type_impl {
  using type = std::remove_cv_t<L>;
};
// 非 const 左值引用：栈里放地址，调用时解引用，写操作能回到调用者。
template <class T>
struct slot_type_impl<T&> {
  using type = T*;
};
// const 左值引用：栈里保存一份值（与原库一致：不要求调用者活过 binder）。
template <class T>
struct slot_type_impl<T const&> {
  using type = std::remove_cv_t<T>;
};
// 右值引用：保存值，每次调用 move 出去（一次性）。
template <class T>
struct slot_type_impl<T&&> {
  using type = std::remove_cv_t<T>;
};

template <class L>
using slot_type_t = typename slot_type_impl<L>::type;

// 这一格是不是“引用参数”（栈里存的是 T*）。
template <class L>
inline constexpr bool is_ref_slot_v =
    std::is_lvalue_reference_v<L> &&
    !std::is_const_v<std::remove_reference_t<L>>;

// 把调用实参装进槽位：引用参数写地址，值参数原样构造。
template <class L, class Arg>
auto* slot_address_from(Arg&& arg) {
  using pointed = std::remove_reference_t<L>;
  using arg_type = std::remove_cvref_t<Arg>;
  if constexpr (is_reference_wrapper_v<arg_type>) {
    return static_cast<pointed*>(std::addressof(arg.get()));
  } else if constexpr (std::is_pointer_v<arg_type>) {
    return static_cast<pointed*>(arg);
  } else {
    return static_cast<pointed*>(std::addressof(arg));
  }
}

// 这个实参能不能写进这一格。编译期判据，运行期只做槽位号分派。
template <class L, class Arg>
inline constexpr bool slot_bindable_v = [] {
  if constexpr (is_ref_slot_v<L>) {
    using pointed = std::remove_reference_t<L>;
    using arg_type = std::remove_cvref_t<Arg>;
    if constexpr (is_reference_wrapper_v<arg_type>) {
      return std::is_convertible_v<typename arg_type::type*, pointed*>;
    } else if constexpr (std::is_pointer_v<arg_type>) {
      return std::is_convertible_v<arg_type, pointed*>;
    } else {
      // 只接受左值：把右值绑定到非 const 引用会立刻悬垂。
      return std::is_lvalue_reference_v<Arg> &&
             std::is_convertible_v<arg_type*, pointed*>;
    }
  } else {
    using slot = slot_type_t<L>;
    return std::is_constructible_v<slot, Arg> ||
           std::is_assignable_v<slot&, Arg>;
  }
}();

// 调用时的实参：值/const& 传左值引用（函数按签名自己 copy），
// 右值引用 move，引用参数解引用。
// 值参数里只能移动的类型（unique_ptr 等）按右值传；这会把栈里的值 move 走，
// 后续再调用拿到的是 moved-from 状态，调用方自己决定要不要重新 bind。
template <class L, class S>
decltype(auto) call_argument(S& slot) {
  if constexpr (is_ref_slot_v<L>) {
    // S 是 remove_reference_t<L>*，解引用就回到调用者对象。
    return *slot;
  } else if constexpr (std::is_rvalue_reference_v<L>) {
    return static_cast<S&&>(slot);
  } else if constexpr (!std::is_copy_constructible_v<S>) {
    return static_cast<S&&>(slot);
  } else {
    return static_cast<S&>(slot);
  }
}

template <class T>
concept stream_extractable = requires(std::istream& input, T& value) {
  input >> value;
};

template <std::size_t N, std::size_t... I>
constexpr auto make_reverse_sequence(std::index_sequence<I...>) noexcept {
  return std::index_sequence<(N - 1 - I)...>{};
}

template <std::size_t N>
using reverse_index_sequence =
    decltype(make_reverse_sequence<N>(std::make_index_sequence<N>{}));

// ============================================================================
// 2) 参数栈布局：全部交给 type_safe_aligned_range / field_size
// ============================================================================
template <std::size_t Alignment, class List>
struct aligned_slot_layout;

template <std::size_t Alignment, class... S>
struct aligned_slot_layout<Alignment, exp_list<S...>> {
  using range_type = type_safe_aligned_range<Alignment, S...>;
  using proxy_type = aligned_iterator_storage<S...>;
  static constexpr std::size_t slot_count = sizeof...(S);
  static constexpr std::size_t bytes = field_size<Alignment, S...>();
  static constexpr std::size_t alignment = max_alignof_v<S...>;

  template <std::size_t I>
  using slot = exp_select<I, exp_list<S...>>;

  // 第 I 格的 offset 游标（seek_to<offset, S>）：编译期偏移，运行期只剩加法。
  template <std::size_t I>
  using cursor =
      typename aligned_offset_stream<Alignment, S...>::template at<I>;
};

// ============================================================================
// 3) 签名拆分
// ============================================================================
template <class R, class Args>
struct binder_from_arguments;

template <class R, class... L>
struct binder_from_arguments<R, exp_list<L...>> {
  template <std::size_t Alignment, class F>
  using binder = function_binder<Alignment, F, R, L...>;
};

template <class R, class Args>
struct to_signature;

template <class R, class... L>
struct to_signature<R, exp_list<L...>> {
  using type = R(L...);
};

// 函数指针（含 noexcept）：exp_function_info 只认普通函数指针，
// 这里补一份更宽的推导。
template <class F>
struct function_pointer_traits {};

template <class R, class... L>
struct function_pointer_traits<R (*)(L...)> {
  using return_type = R;
  using argument_types = exp_list<L...>;
};

template <class R, class... L>
struct function_pointer_traits<R (*)(L...) noexcept> {
  using return_type = R;
  using argument_types = exp_list<L...>;
};

template <class F>
concept callable_deducible =
    (std::is_pointer_v<std::remove_cvref_t<F>> &&
     std::is_function_v<std::remove_pointer_t<std::remove_cvref_t<F>>>) ||
    requires { typename exp_function_info<std::remove_cvref_t<F>>::return_type; };

// 成员函数（含 const / noexcept）：单独一套 traits，不污染 exp_function_info。
template <class F>
struct member_function_traits {};

template <class T, class R, class... L>
struct member_function_traits<R (T::*)(L...)> {
  using return_type = R;
  using argument_types = exp_list<L...>;
};

template <class T, class R, class... L>
struct member_function_traits<R (T::*)(L...) const> {
  using return_type = R;
  using argument_types = exp_list<L...>;
};

template <class T, class R, class... L>
struct member_function_traits<R (T::*)(L...) noexcept> {
  using return_type = R;
  using argument_types = exp_list<L...>;
};

template <class T, class R, class... L>
struct member_function_traits<R (T::*)(L...) const noexcept> {
  using return_type = R;
  using argument_types = exp_list<L...>;
};

template <class F>
concept member_deducible =
    requires { typename member_function_traits<std::remove_cvref_t<F>>::return_type; };

template <std::size_t Alignment, class F, bool IsFunctionPointer>
struct binder_for_select;

template <std::size_t Alignment, class F>
struct binder_for_select<Alignment, F, true> {
  using traits = function_pointer_traits<F>;
  using type = typename binder_from_arguments<
      typename traits::return_type,
      typename traits::argument_types>::template binder<Alignment, F>;
};

template <std::size_t Alignment, class F>
struct binder_for_select<Alignment, F, false> {
  using info = exp_function_info<F>;
  using type = typename binder_from_arguments<
      typename info::return_type,
      typename info::argument_types>::template binder<Alignment, F>;
};

template <std::size_t Alignment, class F>
using binder_for_t = typename binder_for_select<Alignment, std::decay_t<F>,
                                                std::is_pointer_v<std::decay_t<F>>>::type;

// 成员函数 -> 一个只捕获对象地址和成员指针的小 lambda。
template <class Signature>
struct member_callable;

template <class R, class... L>
struct member_callable<R(L...)> {
  template <class T, class F>
  static auto make(T& object, F member) {
    return [pointer = std::addressof(object), member](L... args) -> R {
      return std::invoke(member, *pointer, std::forward<L>(args)...);
    };
  }
};

}  // namespace detail

// ============================================================================
// 4) function_binder
// ============================================================================
template <std::size_t Alignment, class F, class R, class... L>
class function_binder {
  static_assert(sizeof...(L) <= 64,
                "function_binder supports at most 64 arguments "
                "(the liveness map is a 64-bit mask)");

 public:
  using return_type = R;
  using callable_type = F;

  static constexpr std::size_t argument_count = sizeof...(L);
  static constexpr std::size_t arg_count = argument_count;

 private:
  // 空签名用占位槽撑起 range；对外仍然报告 0 个参数。
  using slot_list =
      std::conditional_t<argument_count == 0, exp_list<detail::no_argument_slot>,
                         exp_list<detail::slot_type_t<L>...>>;
  using stack_type = detail::aligned_slot_layout<Alignment, slot_list>;
  using range_type = typename stack_type::range_type;
  using proxy_type = typename stack_type::proxy_type;

  template <std::size_t I>
  using slot_type = typename stack_type::template slot<I>;
  template <std::size_t I>
  using cursor_type = typename stack_type::template cursor<I>;
  template <std::size_t I>
  using arg_type = exp_select<I, exp_list<L...>>;

  static constexpr std::uint64_t make_full_mask() noexcept {
    if constexpr (argument_count == 0) {
      return 0;
    } else if constexpr (argument_count == 64) {
      return ~std::uint64_t{0};
    } else {
      return (std::uint64_t{1} << argument_count) - 1;
    }
  }
  static constexpr std::uint64_t full_mask = make_full_mask();

  template <std::size_t I>
  static constexpr std::uint64_t bit = std::uint64_t{1} << I;

  static constexpr bool copyable_slots =
      (std::is_copy_constructible_v<detail::slot_type_t<L>> && ...);
  static constexpr bool movable_slots =
      (std::is_move_constructible_v<detail::slot_type_t<L>> && ...);

  template <class Arg, std::size_t... I>
  static constexpr bool accepts_any_impl(std::index_sequence<I...>) {
    return (detail::slot_bindable_v<arg_type<I>, Arg> || ...);
  }

 public:
  // 某个实参类型能不能写进这个 binder 的任意一格。
  template <class Arg>
  static constexpr bool accepts_any =
      accepts_any_impl<Arg>(std::make_index_sequence<argument_count>{});

  // 所有实参都写得进去（series 用来做编译期筛选）。
  template <class... Args>
  static constexpr bool accepts_all() {
    return (accepts_any<Args> && ...);
  }

  // 第 I 格能否接受 Arg（series 的扁平绑定用）。
  template <std::size_t I, class Arg>
  static constexpr bool slot_accepts() {
    return detail::slot_bindable_v<exp_select<I, exp_list<L...>>, Arg>;
  }

  static constexpr std::size_t stack_bytes = stack_type::bytes;

  // ---- 构造 / 析构 ----
  function_binder() requires std::default_initializable<F> = default;

  explicit function_binder(F callable) : callable_(std::move(callable)) {}

  function_binder(function_binder const& other)
    requires(std::is_copy_constructible_v<F> && copyable_slots)
      : callable_(other.callable_), cursor_(other.cursor_) {
    try {
      copy_live(other, std::make_index_sequence<argument_count>{});
    } catch (...) {
      destroy_live();
      throw;
    }
  }

  function_binder(function_binder&& other)
    requires(std::is_move_constructible_v<F> && movable_slots)
      : callable_(std::move(other.callable_)), cursor_(other.cursor_) {
    try {
      move_live(other, std::make_index_sequence<argument_count>{});
    } catch (...) {
      destroy_live();
      throw;
    }
  }

  function_binder& operator=(function_binder const& other)
    requires(std::is_copy_constructible_v<F> && copyable_slots) {
    if (this == &other) return *this;
    destroy_live();
    cursor_ = other.cursor_;
    reset_callable(other.callable_);
    try {
      copy_live(other, std::make_index_sequence<argument_count>{});
    } catch (...) {
      destroy_live();
      throw;
    }
    return *this;
  }

  function_binder& operator=(function_binder&& other)
    requires(std::is_move_constructible_v<F> && movable_slots) {
    if (this == &other) return *this;
    destroy_live();
    cursor_ = other.cursor_;
    reset_callable(std::move(other.callable_));
    try {
      move_live(other, std::make_index_sequence<argument_count>{});
    } catch (...) {
      destroy_live();
      throw;
    }
    return *this;
  }

  ~function_binder() { destroy_live(); }

  // ---- 可调用对象 ----
  F& callable() noexcept { return callable_; }
  F const& callable() const noexcept { return callable_; }

  // ---- 大小 / 状态 ----
  static constexpr std::size_t argc_count() noexcept { return argument_count; }
  std::size_t current_index() const noexcept { return cursor_; }
  std::size_t bound_count() const noexcept {
    return static_cast<std::size_t>(std::popcount(live_));
  }
  bool is_bound(std::size_t index) const noexcept {
    return index < argument_count && ((live_ >> index) & 1) != 0;
  }

  void reset_argc_stack() noexcept { cursor_ = 0; }

  void rebind_back(std::size_t count) {
    if (count > cursor_) throw exception{error::index_exceed};
    cursor_ = static_cast<std::uint16_t>(cursor_ - count);
  }

  void clear() noexcept {
    destroy_live();
    cursor_ = 0;
  }

  // ---- 绑定 ----
  template <class Arg>
  void bind(Arg&& arg) {
    static_assert(argument_count != 0,
                  "function_binder: a zero-argument binder has no slot to bind");
    static_assert(accepts_any<Arg&&>,
                  "function_binder: this argument type is not bindable to any "
                  "slot of the signature");
    if (cursor_ >= argument_count) throw exception{error::argument_overflow};
    bind_dispatch<0>(cursor_, std::forward<Arg>(arg));
    ++cursor_;
  }

  template <class... Args>
  void bind_a_lot(Args&&... args) {
    (bind(std::forward<Args>(args)), ...);
  }

  // 指定槽位号绑定（series 的扁平参数分配用）。
  // 与 bind 不同：游标只前进到“至少 I+1”，不会覆盖别的槽位。
  template <std::size_t I, class Arg>
  void bind_at(Arg&& arg) {
    static_assert(I < argument_count, "function_binder::bind_at: slot out of range");
    static_assert(detail::slot_bindable_v<arg_type<I>, Arg&&>,
                  "function_binder::bind_at: argument type does not match the slot");
    bind_slot<I>(std::forward<Arg>(arg));
    if (cursor_ < I + 1) cursor_ = static_cast<std::uint16_t>(I + 1);
  }

  // ---- 调用 ----
  R apply_func() {
    if (live_ != full_mask) throw exception{error::not_fully_bound};
    cursor_ = 0;
    if constexpr (argument_count == 0) {
      if constexpr (std::is_void_v<R>) {
        std::invoke(callable_);
      } else {
        return std::invoke(callable_);
      }
    } else {
      return invoke_slots(std::make_index_sequence<argument_count>{});
    }
  }

  R operator()() { return apply_func(); }

  template <class... Args>
  R operator()(Args&&... args) {
    static_assert(sizeof...(Args) != 0,
                  "function_binder: use operator()() to call without rebinding");
    static_assert(sizeof...(Args) <= argument_count,
                  "function_binder: too many arguments for this signature");
    bind_a_lot(std::forward<Args>(args)...);
    return apply_func();
  }

  // ---- 运行期下标：受检代理（与 type_safe_aligned_range 同形）----
  proxy_type operator[](std::size_t index) {
    if constexpr (argument_count == 0) {
      throw exception{error::index_exceed};
    } else {
      return range_type{storage_.data()}[index];
    }
  }

  // 编译期已知类型的运行期槽位访问：binder.at<std::string>(1) += "...";
  template <class T>
  T& at(std::size_t index) {
    return static_cast<T&>((*this)[index]);
  }

  friend std::istream& operator>>(std::istream& is, function_binder& binder) {
    if constexpr (argument_count == 0) {
      throw exception{error::argument_overflow};
    } else {
      if (binder.cursor_ >= argument_count) {
        throw exception{error::argument_overflow};
      }
      binder.read_dispatch<0>(is, binder.cursor_);
      ++binder.cursor_;
      return is;
    }
  }

 private:
  template <std::size_t I>
  std::byte* slot_address() noexcept {
    return cursor_type<I>{}.address_of(storage_.data());
  }

  template <std::size_t I>
  auto& slot_reference() noexcept {
    return cursor_type<I>{}.get(storage_.data());
  }

  template <std::size_t I>
  auto const& slot_reference() const noexcept {
    return cursor_type<I>{}.get(storage_.data());
  }

  template <std::size_t I>
  void destroy_slot() noexcept {
    if ((live_ & bit<I>) != 0) {
      cursor_type<I>{}.destroy(storage_.data());
      live_ &= ~bit<I>;
    }
  }

  template <std::size_t... I>
  void destroy_live_impl(std::index_sequence<I...>) noexcept {
    (destroy_slot<I>(), ...);
  }

  void destroy_live() noexcept {
    if constexpr (argument_count > 0) {
      destroy_live_impl(detail::reverse_index_sequence<argument_count>{});
    }
  }

  template <std::size_t I>
  void copy_slot(function_binder const& other) {
    if ((other.live_ & bit<I>) != 0) {
      ::new (static_cast<void*>(slot_address<I>()))
          slot_type<I>(other.template slot_reference<I>());
      live_ |= bit<I>;
    }
  }

  template <std::size_t I>
  void move_slot(function_binder& other) {
    if ((other.live_ & bit<I>) != 0) {
      ::new (static_cast<void*>(slot_address<I>()))
          slot_type<I>(std::move(other.template slot_reference<I>()));
      live_ |= bit<I>;
    }
  }

  // 可调用对象的赋值：lambda 的 operator= 常常是 deleted（带捕获时），
  // 所以不能赋值时就“先构造临时对象，再销毁旧对象、move 进来”：
  // 临时对象构造失败时 *this 还没被改动。
  template <class Source>
  void reset_callable(Source&& source) {
    if constexpr (std::is_assignable_v<F&, Source&&>) {
      callable_ = std::forward<Source>(source);
    } else {
      static_assert(std::is_nothrow_move_constructible_v<F>,
                    "function_binder: callable is not assignable and not "
                    "nothrow-move-constructible; use an assignable callable");
      F temporary(std::forward<Source>(source));
      std::destroy_at(std::addressof(callable_));
      ::new (static_cast<void*>(std::addressof(callable_)))
          F(std::move(temporary));
    }
  }

  template <std::size_t... I>
  void copy_live(function_binder const& other, std::index_sequence<I...>) {
    (copy_slot<I>(other), ...);
  }

  template <std::size_t... I>
  void move_live(function_binder& other, std::index_sequence<I...>) {
    (move_slot<I>(other), ...);
  }

  template <std::size_t I, class Arg>
  void bind_slot(Arg&& arg) {
    static_assert(detail::slot_bindable_v<arg_type<I>, Arg&&>,
                  "function_binder: argument type does not match the slot");
    if constexpr (detail::is_ref_slot_v<arg_type<I>>) {
      auto* pointer = detail::slot_address_from<arg_type<I>>(std::forward<Arg>(arg));
      if ((live_ & bit<I>) != 0) {
        slot_reference<I>() = pointer;
      } else {
        ::new (static_cast<void*>(slot_address<I>())) slot_type<I>(pointer);
        live_ |= bit<I>;
      }
    } else {
      using slot = slot_type<I>;
      constexpr bool can_construct = std::is_constructible_v<slot, Arg&&>;
      constexpr bool can_assign = std::is_assignable_v<slot&, Arg&&>;
      if constexpr (can_construct) {
        if ((live_ & bit<I>) != 0) {
          if constexpr (can_assign) {
            slot_reference<I>() = std::forward<Arg>(arg);
          } else {
            // 有构造、无赋值：销毁后原地重建。
            cursor_type<I>{}.destroy(storage_.data());
            live_ &= ~bit<I>;
            ::new (static_cast<void*>(slot_address<I>()))
                slot(std::forward<Arg>(arg));
            live_ |= bit<I>;
          }
        } else {
          ::new (static_cast<void*>(slot_address<I>()))
              slot(std::forward<Arg>(arg));
          live_ |= bit<I>;
        }
      } else if constexpr (can_assign) {
        // 只能赋值（例如 std::string 可以从很多类型赋值但不能从它们构造）：
        // 槽位还没构造时无法写入，按类型不匹配处理。
        if ((live_ & bit<I>) == 0) throw exception{error::type_mismatch};
        slot_reference<I>() = std::forward<Arg>(arg);
      } else {
        throw exception{error::type_mismatch};
      }
    }
  }

  template <std::size_t I, class Arg>
  void bind_dispatch(std::size_t index, Arg&& arg) {
    if (index == I) {
      if constexpr (detail::slot_bindable_v<arg_type<I>, Arg&&>) {
        bind_slot<I>(std::forward<Arg>(arg));
        return;
      } else {
        throw exception{error::type_mismatch};
      }
    }
    if constexpr (I + 1 < argument_count) {
      bind_dispatch<I + 1>(index, std::forward<Arg>(arg));
    } else {
      throw exception{error::type_mismatch};
    }
  }

  // 从流读当前槽位：已经构造过就直接读；否则先默认构造再读（读完就 mark live）。
  template <std::size_t I>
  void read_slot(std::istream& is) {
    using slot = slot_type<I>;
    if constexpr (detail::is_ref_slot_v<arg_type<I>>) {
      throw exception{error::type_mismatch};  // 不往引用参数里读
    } else if constexpr (detail::stream_extractable<slot>) {
      if ((live_ & bit<I>) != 0) {
        is >> slot_reference<I>();
      } else if constexpr (std::is_default_constructible_v<slot>) {
        slot temporary{};
        is >> temporary;
        bind_slot<I>(std::move(temporary));
      } else {
        throw exception{error::type_mismatch};
      }
    } else {
      throw exception{error::type_mismatch};
    }
  }

  template <std::size_t I>
  void read_dispatch(std::istream& is, std::size_t index) {
    if (index == I) {
      read_slot<I>(is);
      return;
    }
    if constexpr (I + 1 < argument_count) {
      read_dispatch<I + 1>(is, index);
    } else {
      throw exception{error::index_exceed};
    }
  }

  template <std::size_t... I>
  R invoke_slots(std::index_sequence<I...>) {
    if constexpr (std::is_void_v<R>) {
      std::invoke(
          callable_,
          detail::call_argument<arg_type<I>>(slot_reference<I>())...);
    } else {
      return std::invoke(
          callable_,
          detail::call_argument<arg_type<I>>(slot_reference<I>())...);
    }
  }

  // 声明顺序：先可调用对象，再参数栈。参数栈单独 alignas，保证每一格对得上。
  [[no_unique_address]] F callable_;
  alignas(stack_type::alignment)
      std::array<std::byte, stack_type::bytes> storage_{};
  std::uint64_t live_{0};
  std::uint16_t cursor_{0};
};

// ============================================================================
// 5) 工厂
// ============================================================================
template <std::size_t Alignment = 0, class F>
  requires detail::callable_deducible<std::decay_t<F>>
auto bind(F&& function) {
  using callable = std::decay_t<F>;
  static_assert(!std::is_member_pointer_v<callable>,
                "meta_bind::bind: member functions need bind(object, member)");
  using binder_type = detail::binder_for_t<Alignment, callable>;
  return binder_type{std::forward<F>(function)};
}

// 签名推导不出来（泛型 lambda / 重载 operator()）：给出明确的编译错误。
template <std::size_t Alignment = 0, class F>
  requires(!detail::callable_deducible<std::decay_t<F>>)
void bind(F&&) = delete;

template <class Signature>
struct is_signature : std::false_type {};
template <class R, class... L>
struct is_signature<R(L...)> : std::true_type {};

// 显式签名 -> binder 类型：bind_as<R(L...)>(f) 会走这里。
template <class Signature>
struct signature_binder {
  static_assert(is_signature<Signature>::value,
                "meta_bind: Signature must be written as R(L...)");
};
template <class R, class... L>
struct signature_binder<R(L...)> {
  template <std::size_t Alignment, class F>
  using binder = function_binder<Alignment, F, R, L...>;
};

// 泛型 lambda / 重载 operator() 的显式签名入口。
// 签名放在第一个模板参数：bind_as<R(L...)>(f)；需要 packed 栈时写
// bind_as<R(L...), 1>(f)。
template <class Signature, std::size_t Alignment = 0, class F>
  requires is_signature<Signature>::value
auto bind_as(F&& function) {
  using callable = std::decay_t<F>;
  using binder_type =
      typename signature_binder<Signature>::template binder<Alignment, callable>;
  return binder_type{std::forward<F>(function)};
}

// 成员函数：binder 里只存“对象地址 + 成员指针”，参数栈照旧走 aligned_range。
template <std::size_t Alignment = 0, class T, class F>
  requires detail::member_deducible<F>
auto bind_member(T& object, F member) {
  using member_type = std::remove_cvref_t<F>;
  using traits = detail::member_function_traits<member_type>;
  using signature = typename detail::to_signature<
      typename traits::return_type,
      typename traits::argument_types>::type;
  auto callable = detail::member_callable<signature>::make(object, member);
  using binder_type =
      typename signature_binder<signature>::template binder<Alignment,
                                                            decltype(callable)>;
  return binder_type{std::move(callable)};
}

template <std::size_t Alignment = 0, class T, class F>
  requires detail::member_deducible<F>
auto bind(T& object, F member) {
  return bind_member<Alignment>(object, member);
}

}  // namespace meta_bind

