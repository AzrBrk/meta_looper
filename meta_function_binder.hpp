#pragma once

// ============================================================================
// meta_function_binder.hpp: a binder whose argument stack lives in a type_safe_aligned_range.
//
// It mirrors select_type's exp_function_binder, but the stack is no exp_node list:
//
//   Old design: every argument allocated a node at construction (shared_ptr<ref_wrapper<T>>
//   + shared_ptr<next>), and binding or reading walked that list at run time. An
//   exp_function_binder<int, double, std::string> was several hundred bytes and
//   cost N heap allocations to construct.
//
//   Current design: arguments are laid out directly in one inline std::byte
//   buffer, using the layout computed by type_safe_aligned_range (field_size /
//   aligned_offset_stream are that offset table). Binding performs
//   placement new / assignment at a run-time slot index, and calling expands
//
// Therefore:
//   - no heap allocation and no pointer chasing;
//   - the binder size is the stack bytes plus a liveness bitmap, a cursor and the callable;
//   - arguments are constructed on first bind (default constructibility is not
//     required); after a call the cursor resets and repeated calls never reconstruct.
//
// Argument slot mapping (only decides what the stack stores; calls restore the signature):
//   int                -> int                  (stored by value)
//   int&               -> int*                 (address; dereferenced on call)
//   int const&         -> int                  (a copy; passed as const&)
//   std::string&&      -> std::string          (a copy; moved on call)
//
// As in type_safe_aligned_range, the first template parameter is Alignment:
//   0 (default) = each cell aligned by alignof (memory layout)
//   1           = packed, no padding in the stack (smaller; weak targets rejected)
//
// Main API:
//   meta_bind::bind(f)                 deduce the signature from a free function, lambda or functor
//   meta_bind::bind_as<R(L...)>(f)     explicit signature when deduction fails (generic lambda)
//   meta_bind::bind(obj, &T::member)   bind a member function (obj must outlive the binder)
//   binder.bind(arg) / bind_a_lot(...) write the next slot(s) in order
//   binder() / binder.apply_func()     call; the cursor returns to zero
//   binder[i] / binder.at<T>(i)        read or write an already bound argument
//   binder >> / istream >> binder      read the next slot from a stream
//
// Every failure throws meta_bind::exception with an error code; a slot is never
// silently written wrongly.
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
#include <type_traits>
#include <utility>

#include "meta_invoke_protocols.hpp"
#include "meta_utilities.hpp"
#include "type_safe_aligned_iterator.hpp"

namespace meta_bind {

using namespace exp_utilities;
using namespace type_safe_aligned;

template <std::size_t Alignment, class F, class R, class... L>
class function_binder;

template <class... Binders>
class function_series;

// ============================================================================
// 0) Errors
// ============================================================================
enum class error {
  argument_overflow,  // more arguments bound than the signature allows
  index_exceed,       // slot or function index out of range
  type_mismatch,      // the argument type does not match the current slot
  not_fully_bound,    // called while slots are still unbound
  argument_not_live,  // accessed a slot that is not constructed yet
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
// 1) Slot types and call expressions
// ============================================================================

template <class T>
struct is_reference_wrapper : std::false_type {};
template <class T>
struct is_reference_wrapper<std::reference_wrapper<T>> : std::true_type {};
template <class T>
inline constexpr bool is_reference_wrapper_v =
    is_reference_wrapper<std::remove_cvref_t<T>>::value;

// Placeholder slot for an empty signature: type_safe_aligned_range needs one type.
struct no_argument_slot {};

template <class... T>
inline constexpr std::size_t max_alignof_v = [] {
  std::size_t result = 1;
  ((result = alignof(T) > result ? alignof(T) : result), ...);
  return result;
}();

// Parameter -> the type actually stored in the stack.
template <class L>
struct slot_type_impl {
  using type = std::remove_cv_t<L>;
};
// Non-const lvalue reference: store the address, dereference on call, writes reach the caller.
template <class T>
struct slot_type_impl<T&> {
  using type = T*;
};
// const lvalue reference: keep a copy (as the original library: the caller need
// not outlive the binder).
template <class T>
struct slot_type_impl<T const&> {
  using type = std::remove_cv_t<T>;
};
// Rvalue reference: keep a copy and move it out on every call (one-shot).
template <class T>
struct slot_type_impl<T&&> {
  using type = std::remove_cv_t<T>;
};

template <class L>
using slot_type_t = typename slot_type_impl<L>::type;

// Whether this cell is a reference parameter (the stack stores a T*).
template <class L>
inline constexpr bool is_ref_slot_v =
    std::is_lvalue_reference_v<L> &&
    !std::is_const_v<std::remove_reference_t<L>>;

// Store a call argument into its slot: references take the address, values construct in place.
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

// Whether this argument can be written into that cell. Compile-time test; run
// time only dispatches on the index.
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
      // Lvalues only: binding an rvalue to a non-const reference would dangle immediately.
      return std::is_lvalue_reference_v<Arg> &&
             std::is_convertible_v<arg_type*, pointed*>;
    }
  } else {
    using slot = slot_type_t<L>;
    return std::is_constructible_v<slot, Arg> ||
           std::is_assignable_v<slot&, Arg>;
  }
}();

// Call arguments: values and const& pass as lvalue references (the function
// copies per its signature),
// rvalue references move, reference parameters dereference.
// Move-only value parameters (unique_ptr, ...) pass as rvalues, moving the stored value away;
// later calls see the moved-from state and the caller decides whether to bind again.
template <class L, class S>
decltype(auto) call_argument(S& slot) {
  if constexpr (is_ref_slot_v<L>) {
    // S is remove_reference_t<L>*, so dereferencing returns to the caller object.
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
// 2) Argument-stack layout: delegated to type_safe_aligned_range / field_size
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

  // Offset cursor of cell I (seek_to<offset, S>): compile-time offset, run time only adds.
  template <std::size_t I>
  using cursor =
      typename aligned_offset_stream<Alignment, S...>::template at<I>;
};

// ============================================================================
// 3) Signature decomposition
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

// Function pointers (including noexcept): exp_function_info only accepts plain ones,
// so a wider deduction lives here.
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

// Member functions (const / noexcept): a separate trait that does not pollute exp_function_info.
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

// Member function -> a small lambda capturing the object address and the member pointer.
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
  template <class...>
  friend class function_series;

  static_assert(sizeof...(L) <= 64,
                "function_binder supports at most 64 arguments "
                "(the liveness map is a 64-bit mask)");

 public:
  using return_type = R;
  using callable_type = F;
  using argument_types = exp_list<L...>;

  static constexpr std::size_t argument_count = sizeof...(L);
  static constexpr std::size_t arg_count = argument_count;

 private:
  // An empty signature is propped up by a placeholder slot; the binder still reports 0 parameters.
  // select_if_t is a lazy thunk: only the selected branch is instantiated.
  using slot_list = ::select_if_t<
      (argument_count == 0),
      ::always_fn<exp_list<detail::no_argument_slot>>,
      ::always_fn<exp_list<detail::slot_type_t<L>...>>>;
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
  // Whether an argument type can be written into any cell of this binder.
  template <class Arg>
  static constexpr bool accepts_any =
      accepts_any_impl<Arg>(std::make_index_sequence<argument_count>{});

  // Every argument fits (used by series for compile-time filtering).
  template <class... Args>
  static constexpr bool accepts_all() {
    return (accepts_any<Args> && ...);
  }

  // Whether cell I accepts Arg (used by the flat binding of series).
  template <std::size_t I, class Arg>
  static constexpr bool slot_accepts() {
    return detail::slot_bindable_v<exp_select<I, exp_list<L...>>, Arg>;
  }

  static constexpr std::size_t stack_bytes = stack_type::bytes;

  // ---- Construction / destruction ----
  function_binder() requires std::default_initializable<F> = default;

  explicit function_binder(F callable) : callable_(std::move(callable)) {}

  function_binder(function_binder const& other)
    requires(std::is_copy_constructible_v<F> && copyable_slots)
      : callable_(other.callable_), cursor_(other.cursor_),
        front_feed_(other.front_feed_) {
    try {
      copy_live(other, std::make_index_sequence<argument_count>{});
    } catch (...) {
      destroy_live();
      throw;
    }
  }

  function_binder(function_binder&& other)
    requires(std::is_move_constructible_v<F> && movable_slots)
      : callable_(std::move(other.callable_)), cursor_(other.cursor_),
        front_feed_(other.front_feed_) {
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
    front_feed_ = other.front_feed_;
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
    front_feed_ = other.front_feed_;
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

  // ---- Callable ----
  F& callable() noexcept { return callable_; }
  F const& callable() const noexcept { return callable_; }

  // ---- Size / state ----
  static constexpr std::size_t argc_count() noexcept { return argument_count; }
  std::size_t current_index() const noexcept { return cursor_; }
  std::size_t bound_count() const noexcept {
    return static_cast<std::size_t>(std::popcount(live_));
  }
  bool is_bound(std::size_t index) const noexcept {
    return index < argument_count && ((live_ >> index) & 1) != 0;
  }

  void reset_argc_stack() noexcept { cursor_ = 0; }

  auto live_arguments() & {
    if (live_ != full_mask) throw exception{error::not_fully_bound};
    return range_type{storage_.data()};
  }

  auto live_arguments() const& {
    if (live_ != full_mask) throw exception{error::not_fully_bound};
    using const_slots = select_if_t<argument_count == 0,
      always_fn<exp_list<detail::no_argument_slot const>>,
      always_fn<exp_list<detail::slot_type_t<L> const...>>>;
    return typename detail::aligned_slot_layout<Alignment,
      const_slots>::range_type{const_cast<std::byte*>(storage_.data())};
  }

  void rebind_back(std::size_t count) {
    if (count > cursor_) throw exception{error::index_exceed};
    cursor_ = static_cast<std::uint16_t>(cursor_ - count);
  }

  void clear() noexcept {
    destroy_live();
    cursor_ = 0;
  }

  // ---- Binding ----
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

  // Bind a specific slot index (used by the flat argument allocation of series).
  // Unlike bind, the cursor only advances to at least I+1 and never overwrites another slot.
  template <std::size_t I, class Arg>
  void bind_at(Arg&& arg) {
    static_assert(I < argument_count, "function_binder::bind_at: slot out of range");
    static_assert(detail::slot_bindable_v<arg_type<I>, Arg&&>,
                  "function_binder::bind_at: argument type does not match the slot");
    bind_slot<I>(std::forward<Arg>(arg));
    if (cursor_ < I + 1) cursor_ = static_cast<std::uint16_t>(I + 1);
  }

  template <class Arg>
  void bind_at_runtime(std::size_t index, Arg&& arg) {
    if (index >= argument_count) throw exception{error::index_exceed};
    bind_dispatch<0>(index, std::forward<Arg>(arg));
    if (cursor_ < index + 1) cursor_ = static_cast<std::uint16_t>(index + 1);
  }

  // Named slot: set(ui::create_arg::title, "Hello").
  // Goes through bind_slot construction/assignment, so std::string slots accept string literals.
  template <class Enum, class Arg>
    requires std::is_enum_v<Enum>
  void set(Enum index, Arg&& arg) {
    bind_at_runtime(static_cast<std::size_t>(index),
                    std::forward<Arg>(arg));
  }

  template <auto Index, class Arg>
    requires(std::is_enum_v<decltype(Index)> ||
             std::is_integral_v<decltype(Index)>)
  void set(Arg&& arg) {
    bind_at<static_cast<std::size_t>(Index)>(std::forward<Arg>(arg));
  }

  // Component-chain input slot: the series overwrites slot 0 with the previous
  // stage's return value instead of appending at the cursor. Off for a plain binder.
  template <class Arg>
  void bind_front(Arg&& arg) {
    static_assert(argument_count > 0,
                  "function_binder::bind_front: empty signature");
    bind_at<0>(std::forward<Arg>(arg));
  }

  function_binder& as_front_feed(bool value = true) & noexcept {
    front_feed_ = value;
    return *this;
  }

  function_binder&& as_front_feed(bool value = true) && noexcept {
    front_feed_ = value;
    return std::move(*this);
  }

  bool front_feed() const noexcept { return front_feed_; }

  // ---- Calling ----
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

  // ---- Run-time index: checked proxy (same shape as type_safe_aligned_range) ----
  proxy_type operator[](std::size_t index) {
    if constexpr (argument_count == 0) {
      throw exception{error::index_exceed};
    } else {
      return range_type{storage_.data()}[index];
    }
  }

  template <class Enum>
    requires std::is_enum_v<Enum>
  proxy_type operator[](Enum index) {
    return (*this)[static_cast<std::size_t>(index)];
  }

  // Run-time slot access with a compile-time type: binder.at<std::string>(1) += "...";
  template <class T>
  T& at(std::size_t index) {
    if constexpr (argument_count == 0) {
      throw exception{error::index_exceed};
    } else {
      return range_type{storage_.data()}.template at<T>(index);
    }
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

  template <std::size_t I, class Arg>
  void emplace_empty_slot(Arg&& arg) {
    static_assert(detail::slot_bindable_v<arg_type<I>, Arg&&>);
    if constexpr (detail::is_ref_slot_v<arg_type<I>>) {
      ::new (static_cast<void*>(slot_address<I>())) slot_type<I>(
          detail::slot_address_from<arg_type<I>>(std::forward<Arg>(arg)));
    } else {
      ::new (static_cast<void*>(slot_address<I>()))
          slot_type<I>(std::forward<Arg>(arg));
    }
    live_ |= bit<I>;
    cursor_ = static_cast<std::uint16_t>(I + 1);
  }

  template <std::size_t I>
  void move_slot(function_binder& other) {
    if ((other.live_ & bit<I>) != 0) {
      ::new (static_cast<void*>(slot_address<I>()))
          slot_type<I>(std::move(other.template slot_reference<I>()));
      live_ |= bit<I>;
    }
  }

  // Callable assignment: a lambda's operator= is often deleted (when it captures),
  // so when assignment is unavailable: construct a temporary, destroy the old
  // object, then move it in. A throwing construction leaves *this intact.
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
            // Constructible but not assignable: destroy and reconstruct in place.
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
        // Assignable only (std::string can be assigned from many types it cannot be
// constructed from):
        // a slot that is not constructed yet cannot be written and is a type mismatch.
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

  // Read the slot from a stream: read in place when constructed, otherwise default
// construct it, read, then mark it live.
  template <std::size_t I>
  void read_slot(std::istream& is) {
    using slot = slot_type<I>;
    if constexpr (detail::is_ref_slot_v<arg_type<I>>) {
      throw exception{error::type_mismatch};  // never read into reference parameters
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

  // Declaration order: callable first, then the stack, which gets its own alignas so
// every cell lines up.
  [[no_unique_address]] F callable_;
  alignas(stack_type::alignment)
      std::array<std::byte, stack_type::bytes> storage_{};
  std::uint64_t live_{0};
  std::uint16_t cursor_{0};
  bool front_feed_{false};
};

// ============================================================================
// 5) Factories
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

// Deduction failed (generic lambda / overloaded operator()): emit an explicit compile-time error.
template <std::size_t Alignment = 0, class F>
  requires(!detail::callable_deducible<std::decay_t<F>>)
void bind(F&&) = delete;

template <class Signature>
struct is_signature : std::false_type {};
template <class R, class... L>
struct is_signature<R(L...)> : std::true_type {};

// Explicit signature -> binder type: bind_as<R(L...)>(f) takes this path.
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

// Explicit-signature entry for generic lambdas / overloaded operator().
// The signature is the first template argument: bind_as<R(L...)>(f); for a
// bind_as<R(L...), 1>(f)。
template <class Signature, std::size_t Alignment = 0, class F>
  requires is_signature<Signature>::value
auto bind_as(F&& function) {
  using callable = std::decay_t<F>;
  using binder_type =
      typename signature_binder<Signature>::template binder<Alignment, callable>;
  return binder_type{std::forward<F>(function)};
}

// Member function: the binder only stores the object address and the member pointer;
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

