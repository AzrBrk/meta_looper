#pragma once

#include <bit>

#include "meta_invoke_protocols.hpp"
#include "meta_uint128.hpp"
#include "meta_segment_bank.hpp"

namespace meta_objects {

using namespace meta_invoke_protocols;

namespace meta_objects_details {
struct meta_empty {
  static constexpr int value = 0;
};
struct meta_empty_fn {
  template <class T, class...>
  using apply = T;
};
// Unlike meta_empty, which is a valid payload used by meta_empty_o, this tag
// represents an object that has not yet been initialized. Do not inspect it as
// a concrete value or apply operations that require a complete payload type.
struct meta_uninitialized {};
}  // namespace meta_objects_details
namespace meta_objects_functions_details{
  struct no_arguments_{};
  struct not_this{};
};


//suggest function use no arguments
template<class T>
concept no_arguments = std::is_same_v<T, meta_objects_functions_details::no_arguments_>;

template<class T>
concept this_ref = !std::is_same_v<T, meta_objects_functions_details::not_this>;

namespace initialize_details {


template<class F>
concept no_arguments_initializable = requires { typename F::template initialize<meta_objects_functions_details::no_arguments_>; };

template <template <class...> class initializer_shape>
struct meta_initializer_container {};

template <class F>
concept has_initializer = requires{ typename meta_initializer_container<F::template initialize>; };

template<class F>
concept has_ret = requires{ typename meta_initializer_container<F::template apply_ret>; };

template<class F>
concept object_initializer = has_initializer<F> && is_meta_function_v<F> && !has_ret<F>;

template<class F>
concept ret_object_initializer = has_initializer<F> && is_meta_function_v<F> && has_ret<F>;

template <class F>
struct initialized {
  template <class OBJ, class... Args>
  using apply = meta_invoke<F, OBJ, Args...>;
};

template <class F, class... Args>
struct initialize_t {
  using type = typename F::template initialize<Args...>;
};

//provide no arguments if the initializer can be invoked with no arguments
template<no_arguments_initializable F, class... Args>
struct initialize_t<F, Args...> {
  using type = typename F::template initialize<meta_objects_functions_details::no_arguments_>;
};
template<class F, class... Args>
using initialize = typename initialize_t<F, Args...>::type;
}  // namespace initialize_details

using initialize_details::has_initializer;
using initialize_details::initialize;
using meta_invoke_protocols::meta_function_template_container;

template<class T>
concept meta_object_t = requires{
  typename meta_function_template_container<T::template meta_set>;
  typename meta_function_template_container<T::template apply>;
  typename T::function;
};

template<class T>
constexpr bool is_uninitialized_meta_object = (meta_object_t<T> && !exp_utilities::has_type<T>);
template<class T>
concept uninitialized_meta_object_t = meta_object_t<T> && !exp_utilities::has_type<T>;

template<class T>
constexpr bool is_initialized_meta_object = (meta_object_t<T> && exp_utilities::has_type<T>);
template<class T>
concept initialized_meta_object_t = meta_object_t<T> && exp_utilities::has_type<T>;



using initialize_details::object_initializer;
using meta_objects_details::meta_uninitialized;


namespace meta_object_initialize_details{
  using initialize_details::no_arguments_initializable;
  template<class OBJ> struct meta_object_initialize_impl;

  template<meta_object_t OBJ>
    requires (!has_initializer<typename OBJ::function>)
  struct meta_object_initialize_impl<OBJ>
  {
    using type = OBJ;
  };
  template<meta_object_t OBJ>
    requires (no_arguments_initializable<typename OBJ::function>)
  struct meta_object_initialize_impl<OBJ>
  {
    using type = typename OBJ::template meta_set<initialize<typename OBJ::function>>;
  };
  // Keep the original object's meta_set protocol, but never update its old payload.
  template<meta_object_t OBJ>
    requires (has_initializer<typename OBJ::function> &&
              !no_arguments_initializable<typename OBJ::function>)
  struct meta_object_initialize_impl<OBJ>
  {
    // Naming the wrapper "type" would expose an injected ::type payload name.
    struct deferred_object{
      template<class ...init_args>
      using apply = typename OBJ::template meta_set<
          initialize<typename OBJ::function, init_args...>>;
      template<class T>
      using meta_set = typename OBJ::template meta_set<T>;
      using function = OBJ::function;
    };
    using type = deferred_object;
  };
}

// Reinitialize the payload, defer parameterized initialization, or preserve
// objects whose function has no initializer.
// Invoke the deferred result with meta_invoke<result, init_args...>.
template<meta_object_t OBJ>
using meta_object_initialize = 
  typename meta_object_initialize_details::meta_object_initialize_impl<OBJ>::type; 

// Primary declaration for the meta-object forms defined below.
template<class ...>
struct meta_object;

// An initializer-only meta_object creates its initial payload from invocation
// arguments by calling F::initialize.
template<object_initializer F>
struct meta_object<F>{
  using function = F;
  template<class ...Args>
  using apply = meta_object<initialize<function, Args...>,  function>;
  template<class ANOTHER_OBJ>
  using meta_set = meta_object<ANOTHER_OBJ, F>;
};

// Binds a payload to a meta-function. Each invocation applies F to the current
// payload and returns a new meta_object containing the resulting payload.
template <class OBJ, class F>
struct meta_object<OBJ, F> {
  using function = F;
  using type = OBJ;
  template <class... Arg>
  using apply = meta_object<meta_invoke<F, OBJ, Arg...>, function>;

  template <class ANOTHER_OBJ>
  using meta_set = meta_object<ANOTHER_OBJ, function>;
};

using initialize_details::ret_object_initializer;
// Primary declaration for meta_ret_object's initializer and bound forms.
template<class...>
struct meta_ret_object;

template<object_initializer F, class Ret>
struct meta_ret_object<F, Ret>{
  using function = F;
  using ret = meta_uninitialized;
  template<class ...Args>
  using apply = meta_ret_object<initialize<function, Args...>, function, Ret>;

  template<class ANOTHER_OBJ>
  using meta_set = meta_ret_object<ANOTHER_OBJ, F, Ret>;
};
// A meta_ret_object carries both its current payload and a derived result.
// Ret maps the payload to ::ret, allowing the object to expose a value distinct
// from its update state. F controls state updates; Ret controls result
// projection. An initializer can use meta_uninitialized until its first
// invocation establishes a concrete payload.
template <class OBJ, class F, class Ret>
struct meta_ret_object <OBJ, F, Ret>{
  using function = F;
  using ret = meta_invoke<Ret, OBJ>;
  using type = OBJ;
  template <class... Arg>
  using apply = meta_ret_object<meta_invoke<F, OBJ, Arg...>, F, Ret>;

  template <class ANOTHER_OBJ>
  using meta_set = meta_ret_object<ANOTHER_OBJ, F, Ret>;
};

template<class T>
concept meta_ret_object_t = meta_object_t<T> &&requires{
  typename T::ret;
};


namespace meta_timer_object_details {
struct meta_break_signal : std::false_type {};

template <class Fn, class DF>
struct meta_break_if {
  template <class T>
  using apply =
      std::conditional_t<meta_invoke<Fn, T>::value, meta_break_signal, DF>;
};
struct meta_always_continue {
  template <class T>
  struct apply : std::false_type {};
};
template <class MTO, class... Arg>
struct stop_forward_next_if_break_f_is_true {
  using type = MTO;
};
template <template <std::size_t, class...> class meta_timer_template,
          std::size_t times, object_initializer F, class break_f, class... Arg>
  requires(meta_timer_template<times, F, break_f>::timer::value )
struct stop_forward_next_if_break_f_is_true<
    meta_timer_template<times, F, break_f>, Arg...> {
  using type =
      meta_timer_template<times - 1, initialize<F, Arg...>, F, break_f>;
};
// Keep the template-template parameter variadic. meta_timer_object is declared
// `template <std::size_t, class...>`; spelling a fixed four-parameter template
// here makes Clang reject the binding while deducing it inside a partial
// specialization (P0522 territory). The specialization then silently stops
// matching, the primary template returns the timer unchanged, and neither the
// countdown nor the payload advances from the second round on. GCC accepts both
// spellings, so the failure only shows up on Clang, as "the type never changes".
// Design notes: meta_objects.md.
template <template <std::size_t, class...> class meta_timer_template,
          std::size_t times, class OBJ, class F, class break_f, class... Arg>
  requires(!std::is_same_v<
           typename meta_timer_template<times, OBJ, F, break_f>::timer,
           meta_break_signal>)
struct stop_forward_next_if_break_f_is_true<
    meta_timer_template<times, OBJ, F, break_f>, Arg...> {
  using type =
      meta_timer_template<times - 1, meta_invoke<F, OBJ, Arg...>, F, break_f>;
};
struct timer_receiver {
  template <class timer, class...>
  struct apply : timer {};
};
using meta_timer_cond_o =
    meta_object<void, timer_receiver>;
}  // namespace meta_timer_object_details

// A countdown wrapper that applies F while its break predicate allows the
// timer to advance. It intentionally has no initializer form: the payload and
// remaining count are supplied when the wrapper is created.
using meta_always_continue = meta_timer_object_details::meta_always_continue;
using timer_co = meta_timer_object_details::meta_timer_cond_o;

template <std::size_t times, class ...>
struct meta_timer_object;

using meta_timer_object_details::stop_forward_next_if_break_f_is_true;
using meta_timer_object_details::meta_break_if;

template<std::size_t times, class F, class break_f>
struct meta_timer_object<times, F, break_f>{
  using function = F;
  //there is no type yet, the break_f can't work until initialized
  using timer = std::integral_constant<bool, (times > 0)>;
  template<class ...Args>
  using apply = 
    typename stop_forward_next_if_break_f_is_true<meta_timer_object, Args...>::type;
  template<class OBJ>
  using meta_set = meta_timer_object<times, OBJ, F, break_f>;
};

template <std::size_t times, class OBJ, class F,
          class break_f> 
struct meta_timer_object<times, OBJ, F, break_f> {
  // Emit a break signal when break_f accepts the current timer value;
  // otherwise preserve the default result indicating whether time remains.
  using timer = 
    meta_invoke<meta_break_if<break_f, std::integral_constant<bool, (times > 0)>>, OBJ>;

  using type = OBJ;
  using function = F;
  template <class... Arg>
  using apply =
      typename stop_forward_next_if_break_f_is_true<meta_timer_object, Arg...>::type;

  template <class ANOTHER_OBJ>
  using meta_set = meta_timer_object<times, ANOTHER_OBJ, F, break_f>;

  template <size_t reset_time>
  using reset = meta_timer_object<reset_time, OBJ, F, break_f>;
};
template<class T>
concept meta_timer_object_t = meta_object_t<T> && requires { typename T::timer; };

namespace meta_timer_object_details {
//convert meta_object to meta_timer_object
template <class OBJ, size_t N, class break_f>
struct To_Timer {};

using initialize_details::no_arguments_initializable;
//for uninitialized meta_object_t
template<object_initializer F, std::size_t N, class break_f>
struct To_Timer<meta_object<F>, N, break_f>{
  using type = meta_timer_object<N, F, break_f>;
};

template <class obj, class F, size_t N, class break_f>
struct To_Timer<meta_object<obj, F>, N, break_f> {
  using type = meta_timer_object<N, obj, F, break_f>;
};


template <class mo, class F>
struct Break_If {};
template <size_t N, class OBJ, class MO_F, class F>
struct Break_If<meta_timer_object<N, OBJ, MO_F>, F> {
  using type = meta_timer_object<N, OBJ, MO_F, F>;
};
}  // namespace meta_timer_object_details

template <class OBJ, size_t N,
          class break_f = meta_timer_object_details::meta_always_continue>
using to_timer =
    typename meta_timer_object_details::To_Timer<OBJ, N, break_f>::type;

// Returns the same timer wrapper with BF installed as its break predicate.
template <class MTO, class BF>
using break_if =
    exp_utilities::get_type<meta_timer_object_details::Break_If<MTO, BF>>;

using meta_empty_o = meta_object<meta_objects_details::meta_empty,
                                 meta_objects_details::meta_empty_fn>;

// Identification tag for meta_empty_o. It is an ordinary meta object with no
// special invocation rules; upper layers use this concept to detect an empty
// slot or "nothing connected".
template<class T>
concept meta_empty_object_t = std::is_same_v<T, meta_empty_o>;

namespace meta_op_bits {

using u64 = std::uint64_t;
using u16 = std::uint16_t;
using u8  = std::uint8_t;

template<u64 flags>
using u64_int_t = std::integral_constant<u64, flags>;

template<u128 flags>
using u128_int_t = std::integral_constant<u128, flags>;

// ===== flags transformation steps: unary meta functions over u128 values =====
// Unconditional set and clear.
template<u128 new_bits>
struct set_f{
  template<class flags_t>
  using apply = u128_int_t<u128{flags_t::value} | new_bits>;
};
template<u128 new_bits>
struct deact_f{
  template<class flags_t>
  using apply = u128_int_t<u128{flags_t::value} & ~new_bits>;
};
// Conditional set and clear: returns flags_t unchanged when active is false.
// The bit operations are evaluated on values (constant folding), so one type
// instantiation wraps the result.
template<u128 new_bits, bool active>
struct set_if_f{
  template<class flags_t>
  using apply = u128_int_t<active ? (u128{flags_t::value} | new_bits)
                                  : u128{flags_t::value}>;
};
template<u128 new_bits, bool active>
struct deact_if_f{
  template<class flags_t>
  using apply = u128_int_t<active ? (u128{flags_t::value} & ~new_bits)
                                  : u128{flags_t::value}>;
};

// ===== Field width / mask traits (bool counts as a single bit) =====
namespace flags_field_details {
  // Field width in bits: 1 for bool, sizeof(T) * 8 otherwise.
  template <class T>
  struct field_bits : std::integral_constant<std::size_t, sizeof(T) * 8> {};
  template <>
  struct field_bits<bool> : std::integral_constant<std::size_t, 1> {};

  template <class T>
  constexpr std::size_t field_bits_v = field_bits<T>::value;

  // All-ones field value before shifting: 1 for bool, T(-1) otherwise.
  template <class T>
  constexpr u128 field_ones =
      field_bits_v<T> == 128 ? ~u128{0} : ((u128{1} << field_bits_v<T>) - 1);

  // Field mask shifted to position shift.
  template <class T, std::size_t shift>
    requires (field_bits_v<T> <= 128 && shift <= 128 - field_bits_v<T>)
  constexpr u128 field_mask = field_ones<T> << shift;

  // 64-bit carrier: the same field model, bounded to one word. shift + width
  // beyond 64 is rejected at the definition, not left as undefined behaviour.
  template <class T>
  constexpr u64 field_ones64 =
      field_bits_v<T> == 64 ? ~u64{0} : ((u64{1} << field_bits_v<T>) - 1);

  template <class T, std::size_t shift>
    requires (field_bits_v<T> <= 64 && shift <= 64 - field_bits_v<T>)
  constexpr u64 field_mask64 = field_ones64<T> << shift;
}  // namespace flags_field_details

template<auto Flags>
struct flags_operator;

template<u128 flags>
struct flags_operator<flags> : u128_int_t<flags>
{
  constexpr operator u128() const noexcept { return flags; }
  // Single-bit tests.
  template<u128 bit>
  static constexpr bool has      = (flags & bit) == bit;   // all bits set
  template<u128 bit>
  static constexpr bool has_any  = (flags & bit) != 0;     // any bit set
  template<u128 bit>
  static constexpr bool test     = has_any<bit>;                // common alias

  // Whole-value tests.
  static constexpr bool is_empty = (flags == 0);
  static constexpr bool any      = (flags != 0);
  static constexpr int  count    = std::popcount(flags.low) + std::popcount(flags.high);

  
  // ===== Raw bit operations over u128 masks =====
  template<u128 bit>
  using opSet = flags_operator<flags | bit>;
  template<u128 bit>
  using opDeact = flags_operator<flags & ~bit>;
  template<u128 bit>
  using opToggle = flags_operator<flags ^ bit>;

  template<u128 bit, bool con = true>
  using opToggle_if = flags_operator<con ? (flags ^ bit) : flags>;

  // ===== Field access: type + offset =====
  // Mask of the field at this position (bool is a single bit).
  template<class uintegral_type, std::size_t shift>
  static constexpr u128 mask = flags_field_details::field_mask<uintegral_type, shift>;

  // Field value shifted down to bit 0 and cast back to the field type.
  template<class uintegral_type, std::size_t shift>
  static constexpr uintegral_type get =
      static_cast<uintegral_type>((flags & mask<uintegral_type, shift>) >> shift);
  // Field-level tests.
  template<class T, std::size_t shift>
  static constexpr bool field_any = (get<T, shift> != 0);
  template<class T, std::size_t shift, T v>
  static constexpr bool field_eq  = (get<T, shift> == v);
  template<class T, std::size_t shift>
  static constexpr bool field_none = (get<T, shift> == 0);

  // ===== Field writes: set<T, shift, value> =====
  // Writes value into the field starting at shift, masking out older bits.
  // The arithmetic runs on values (set_value); only the result is wrapped in a
  // flags type.
  template<class uintegral_type, std::size_t shift, uintegral_type value>
  static constexpr u128 set_value =
      (flags & ~mask<uintegral_type, shift>) |
      ((static_cast<u128>(value) << shift) & mask<uintegral_type, shift>);

  template<class uintegral_type, std::size_t shift, uintegral_type value>
  using set = flags_operator<set_value<uintegral_type, shift, value>>;

  // ===== Field clear: deact<T, shift> =====
  // Clears the field starting at shift, leaving every other bit alone.
  template<class uintegral_type, std::size_t shift>
  static constexpr u128 deact_value = flags & ~mask<uintegral_type, shift>;

  template<class uintegral_type, std::size_t shift>
  using deact = flags_operator<deact_value<uintegral_type, shift>>;

  // ===== Conditional field writes and clears =====
  template<bool con, class uintegral_type, std::size_t shift, uintegral_type value>
  using set_if =
      flags_operator<con ? set_value<uintegral_type, shift, value> : flags>;
  template<bool con, class T, std::size_t shift, T v_true, T v_false>
  using set_if_else = set<T, shift, con ? v_true : v_false>;

  // Conditional bit selector: sets one bit group and clears the other.
  template<bool con, u128 bit_true, u128 bit_false>
  using select = flags_operator<con ? (flags | bit_true) & ~bit_false
                                    : (flags | bit_false) & ~bit_true>;
  template<bool con, class uintegral_type, std::size_t shift>
  using deact_if =
      flags_operator<con ? deact_value<uintegral_type, shift> : flags>;
  // Field-level bitwise operations.
  template<class T, std::size_t shift, T v>
  using field_or  = set<T, shift, static_cast<T>(get<T, shift> | v)>;
  template<class T, std::size_t shift, T v>
  using field_and = set<T, shift, static_cast<T>(get<T, shift> & v)>;
  template<class T, std::size_t shift, T v>
  using field_xor = set<T, shift, static_cast<T>(get<T, shift> ^ v)>;

  // Field increment / decrement (counter fields, for example).
  template<class T, std::size_t shift, T delta = 1>
  using inc = set<T, shift, static_cast<T>(get<T, shift> + delta)>;
  template<class T, std::size_t shift, T delta = 1>
  using dec = set<T, shift, static_cast<T>(get<T, shift> - delta)>;

  // Value-level add and subtract: treat flags as one big integer and add or
  // subtract delta at shift.
  //   add_at / sub_at: flags ± (delta << shift)
  // Unlike inc / dec these do not mask the result:
  //   inc / dec go through set<...>, so overflow stays inside the field and
  //   never touches a neighbour;
  //   add_at / sub_at let carries and borrows leak into adjacent fields.
  // Use them when no boundary protection is needed, or to keep flags as a plain
  // integer accumulator. delta is a template argument; for a run-time amount
  // write flags + (u128{delta} << shift) directly.
  template<std::size_t shift, u128 delta = 1>
  using add_at = flags_operator<flags + (delta << shift)>;
  template<std::size_t shift, u128 delta = 1>
  using sub_at = flags_operator<flags - (delta << shift)>;
  // F is a constexpr unary function T(T).
  template<class T, std::size_t shift, auto F>
  using update = set<T, shift,
      static_cast<T>(F(get<T, shift>))>;
  // Example: update<u8, 4, [](u8 v){ return v + 1; }>
  constexpr bool operator==(const flags_operator& other) const noexcept {
    return flags == other.value;
  }
  constexpr bool operator!=(const flags_operator& other) const noexcept {
    return !(*this == other);
  }
  // Raw comparison helpers.
  template<u128 other>
  static constexpr bool eq = (flags == other);
  template<u128 other>
  static constexpr bool ne = (flags != other);
  template<u128 other> using opOr  = flags_operator<flags | other>;
  template<u128 other> using opAnd = flags_operator<flags & other>;
  template<u128 other> using opXor = flags_operator<flags ^ other>;
  using opNot = flags_operator<~flags>;

  // Merge with another flags_operator instance.
  template<class Other>
  using merge     = flags_operator<flags | Other::value>;
  template<class Other>
  using intersect = flags_operator<flags & Other::value>;
  template<class Other>
  using diff      = flags_operator<flags & ~u128{Other::value}>;   // A without B

  using reset = flags_operator<u128{0}>;
  template<u128 keep>
  using keep_only = flags_operator<flags & keep>;
};
// ===== 64-bit carrier =====
// Same protocol, one word. There is deliberately no ::high: a driver that uses
// this carrier must not be written as if it had 128 bits.
template<u64 flags>
struct flags_operator<flags> : u64_int_t<flags>
{
  constexpr operator u64() const noexcept { return flags; }

  template<u64 bit>
  static constexpr bool has      = (flags & bit) == bit;
  template<u64 bit>
  static constexpr bool has_any  = (flags & bit) != 0;
  template<u64 bit>
  static constexpr bool test     = has_any<bit>;

  static constexpr bool is_empty = (flags == 0);
  static constexpr bool any      = (flags != 0);
  static constexpr int  count    = std::popcount(flags);

  template<u64 bit>
  using opSet = flags_operator<flags | bit>;
  template<u64 bit>
  using opDeact = flags_operator<flags & ~bit>;
  template<u64 bit>
  using opToggle = flags_operator<flags ^ bit>;
  template<u64 bit, bool con = true>
  using opToggle_if = flags_operator<con ? (flags ^ bit) : flags>;

  template<class uintegral_type, std::size_t shift>
  static constexpr u64 mask =
      flags_field_details::field_mask64<uintegral_type, shift>;

  template<class uintegral_type, std::size_t shift>
  static constexpr uintegral_type get =
      static_cast<uintegral_type>((flags & mask<uintegral_type, shift>) >> shift);

  template<class T, std::size_t shift>
  static constexpr bool field_any = (get<T, shift> != 0);
  template<class T, std::size_t shift, T v>
  static constexpr bool field_eq  = (get<T, shift> == v);
  template<class T, std::size_t shift>
  static constexpr bool field_none = (get<T, shift> == 0);

  template<class uintegral_type, std::size_t shift, uintegral_type value>
  static constexpr u64 set_value =
      (flags & ~mask<uintegral_type, shift>) |
      ((static_cast<u64>(value) << shift) & mask<uintegral_type, shift>);

  template<class uintegral_type, std::size_t shift, uintegral_type value>
  using set = flags_operator<set_value<uintegral_type, shift, value>>;

  template<class uintegral_type, std::size_t shift>
  static constexpr u64 deact_value = flags & ~mask<uintegral_type, shift>;

  template<class uintegral_type, std::size_t shift>
  using deact = flags_operator<deact_value<uintegral_type, shift>>;

  template<bool con, class uintegral_type, std::size_t shift, uintegral_type value>
  using set_if =
      flags_operator<con ? set_value<uintegral_type, shift, value> : flags>;
  template<bool con, class T, std::size_t shift, T v_true, T v_false>
  using set_if_else = set<T, shift, con ? v_true : v_false>;

  template<bool con, u64 bit_true, u64 bit_false>
  using select = flags_operator<con ? (flags | bit_true) & ~bit_false
                                    : (flags | bit_false) & ~bit_true>;
  template<bool con, class uintegral_type, std::size_t shift>
  using deact_if =
      flags_operator<con ? deact_value<uintegral_type, shift> : flags>;

  template<class T, std::size_t shift, T v>
  using field_or  = set<T, shift, static_cast<T>(get<T, shift> | v)>;
  template<class T, std::size_t shift, T v>
  using field_and = set<T, shift, static_cast<T>(get<T, shift> & v)>;
  template<class T, std::size_t shift, T v>
  using field_xor = set<T, shift, static_cast<T>(get<T, shift> ^ v)>;

  template<class T, std::size_t shift, T delta = 1>
  using inc = set<T, shift, static_cast<T>(get<T, shift> + delta)>;
  template<class T, std::size_t shift, T delta = 1>
  using dec = set<T, shift, static_cast<T>(get<T, shift> - delta)>;

  template<std::size_t shift, u64 delta = 1>
  using add_at = flags_operator<flags + (delta << shift)>;
  template<std::size_t shift, u64 delta = 1>
  using sub_at = flags_operator<flags - (delta << shift)>;

  template<class T, std::size_t shift, auto F>
  using update = set<T, shift, static_cast<T>(F(get<T, shift>))>;

  constexpr bool operator==(const flags_operator& other) const noexcept {
    return flags == other.value;
  }
  constexpr bool operator!=(const flags_operator& other) const noexcept {
    return !(*this == other);
  }

  template<u64 other>
  static constexpr bool eq = (flags == other);
  template<u64 other>
  static constexpr bool ne = (flags != other);
  template<u64 other> using opOr  = flags_operator<flags | other>;
  template<u64 other> using opAnd = flags_operator<flags & other>;
  template<u64 other> using opXor = flags_operator<flags ^ other>;
  using opNot = flags_operator<~flags>;

  template<class Other>
  using merge     = flags_operator<flags | static_cast<u64>(Other::value)>;
  template<class Other>
  using intersect = flags_operator<flags & static_cast<u64>(Other::value)>;
  template<class Other>
  using diff      = flags_operator<flags & ~static_cast<u64>(Other::value)>;

  using reset = flags_operator<u64{0}>;
  template<u64 keep>
  using keep_only = flags_operator<flags & keep>;
};

template<class T>
struct is_flags_operator : std::false_type {};
template<u128 flags>
struct is_flags_operator<flags_operator<flags>> : std::true_type {};
template<u64 flags>
struct is_flags_operator<flags_operator<flags>> : std::true_type {};
template<auto flags> requires is_segment_bank_v<decltype(flags)>
struct is_flags_operator<bank_flags_operator<flags>> : std::true_type {};
template <class T>
constexpr bool is_flags_operator_v = is_flags_operator<T>::value;
}  // namespace meta_op_bits

// ===== meta_states_object (MSO): a pure state meta object that owns flags =====
// It is an ordinary member of the meta-object family and follows the same
// invocation protocol:
//   - meta_invoke<MSO, objects...> returns the new MSO state
//   - meta objects may call each other (see the MSO specialization of
//     Meta_Object_Invoke)
//   - an MSO observer receives the whole meta object, including another MSO;
//     read ::flags explicitly when its state is needed
// The MSO itself knows nothing about generators, receivers or loopers; those
// are behaviours meta_looper implements on top of MSO conditions.
//
// The user Fn receives a flags_operator (this_flags) and chains
// set / deact / set_if / deact_if; its result, Fn::apply<this_flags,
// objects...>, becomes the new MSO flags.
//
// Signature: template<class Flags, class Fn>
//   Flags selects the bit storage; the concrete layout is the user's business.
//   The executor is carried along for extraction only and never influences the
//   MSO flags.

using meta_op_bits::flags_operator;
using meta_op_bits::u64;
using meta_op_bits::u128;
using meta_op_bits::segment_bank;
using meta_op_bits::bank_flags_operator;
template<class T>
concept flags_operator_type = meta_op_bits::is_flags_operator_v<T>;

template<template<auto> class Fn>
struct states_executor{
  template<meta_op_bits::u8 Window, meta_op_bits::u64 L, meta_op_bits::u64 U, class Obj>
  static constexpr meta_op_bits::u64 value = Fn<Window>::template value<L, U, Obj>;
};

namespace meta_states_object_details {
template<auto Flags>
consteval auto normalize_flags() {
  if constexpr (meta_op_bits::is_segment_bank_v<decltype(Flags)>) return Flags;
  // An exact u64 seed stays 64-bit; every other integer keeps the u128 carrier
  // so existing spellings (flags_operator<0>, flags_operator<some_u64_const>)
  // do not silently change carrier.
  else if constexpr (std::same_as<std::remove_cv_t<decltype(Flags)>,
                                  meta_op_bits::u64>) return Flags;
  else return u128(Flags);
}
}

template<auto Flags, class Fn>
struct meta_states_object_impl;

template<auto Flags, class Fn>
using meta_states_object = meta_states_object_impl<
    meta_states_object_details::normalize_flags<Flags>(), Fn>;

template<u128 Flags, class Fn>
struct meta_states_object_impl<Flags, Fn> {
  using flags = flags_operator<Flags>;
  using function = Fn;

  // Variadic apply: hands the flags and the incoming objects to the user Fn,
  // Fn::apply<flags, objects...>. As with every other MSO - and with
  // meta_invoke - apply yields the type directly, with no ::type unwrapping.
  // Note: ::value is a static data member, not a type, so a requires-clause
  // must not put typename in front of it.
  template <class... objects> requires requires {
    meta_invoke<Fn, flags, objects...>::value;
    requires std::convertible_to<decltype(meta_invoke<Fn, flags, objects...>::value), u128>;
  }
  using apply = meta_states_object<
      meta_invoke<Fn, flags, objects...>::value, Fn>;

  // Reset directly from a flags_operator type.
  template <flags_operator_type ANOTHER_FLAGS> requires
    std::convertible_to<decltype(ANOTHER_FLAGS::value), u128>
  using meta_set = meta_states_object<ANOTHER_FLAGS::value, Fn>;

  // An MSO produces its flags.
  using type = flags;
  static constexpr auto value = flags::value;
};

template<u64 Flags, class Fn>
struct meta_states_object_impl<Flags, Fn> {
  using flags = flags_operator<Flags>;
  using function = Fn;

  template <class... objects> requires requires {
    meta_invoke<Fn, flags, objects...>::value;
    requires std::same_as<std::remove_cv_t<
        decltype(meta_invoke<Fn, flags, objects...>::value)>, u64>;
  }
  using apply = meta_states_object<meta_invoke<Fn, flags, objects...>::value, Fn>;

  // Only a u64 carrier may set a u64 MSO: no implicit widening / narrowing
  // between carriers.
  template <flags_operator_type ANOTHER_FLAGS> requires
    std::same_as<std::remove_cv_t<decltype(ANOTHER_FLAGS::value)>, u64>
  using meta_set = meta_states_object<ANOTHER_FLAGS::value, Fn>;

  using type = flags;
  static constexpr auto value = flags::value;
};
template<auto Flags, class Fn> requires meta_op_bits::is_segment_bank_v<decltype(Flags)>
struct meta_states_object_impl<Flags, Fn> {
  using flags = bank_flags_operator<Flags>;
  using function = Fn;

  template<class... Objects> requires requires {
    meta_invoke<Fn, flags, Objects...>::value;
    requires std::same_as<
        std::remove_cv_t<decltype(meta_invoke<Fn, flags, Objects...>::value)>,
        typename flags::bank_type>;
  }
  using apply = meta_states_object<meta_invoke<Fn, flags, Objects...>::value, Fn>;

  template<flags_operator_type Other> requires
    std::same_as<std::remove_cv_t<decltype(Other::value)>, typename flags::bank_type>
  using meta_set = meta_states_object<Other::value, Fn>;

  using type = flags;
  static constexpr auto value = flags::value;
};
// Detects meta_states_object instantiations (declared before meta_timer_object).
// The Flags carrier of an MSO is meta_op_bits::flags_operator.
template<class T>
concept meta_states_object_t = meta_object_t<T> && requires { typename T::flags; };
template<class T>
constexpr bool is_meta_states_object_v = meta_states_object_t<T>;


template<class T>
concept pure_meta_object_t = meta_object_t<T> 
  && !meta_states_object_t<T> 
  && !meta_ret_object_t<T> 
  && !meta_timer_object_t<T>;
namespace meta_objects_invoke_details {
template <class From_T, class To_T>
struct meta_transfer_object_impl;

template<pure_meta_object_t From_T, meta_object_t To_T> requires (exp_utilities::has_type<From_T>)
struct meta_transfer_object_impl<From_T, To_T>
{
  using type = typename To_T::template meta_set<typename From_T::type>;
};

template<pure_meta_object_t From_T, meta_object_t To_T> requires (!exp_utilities::has_type<From_T>)
struct meta_transfer_object_impl<From_T, To_T>
{
  using type = typename To_T::template meta_set<meta_uninitialized>;
};

// Transfer the timer's computed status to the destination object's payload.
template<meta_timer_object_t From_T, meta_object_t To_T>
struct meta_transfer_object_impl<From_T, To_T>
{
  using type = typename To_T::template meta_set<typename From_T::timer>;
};

// Transfer the projected result when the source is a meta_ret_object.
template<meta_ret_object_t From_T, meta_object_t To_T>
struct meta_transfer_object_impl<From_T, To_T>
{
  using type = typename To_T::template meta_set<typename From_T::ret>;
};

template<meta_states_object_t From_T, meta_object_t To_T>
struct meta_transfer_object_impl<From_T, To_T>
{
  using type = typename To_T::template meta_set<typename From_T::flags>;
};
}  // namespace meta_objects_invoke_details

template <class From_T, class To_T>
using meta_transfer_object =
    typename meta_objects_invoke_details::meta_transfer_object_impl<From_T,
                                                                    To_T>::type;




/*meta object invocation protocols*/
namespace meta_objects_invoke_details {
// Invoke the first meta-object as a meta-function: ordinary receivers consume
// projected output; MSO observers receive the entire second object.
using initialize_details::no_arguments_initializable;
using exp_utilities::has_type;

template<class ...Args>
struct meta_object_invoke_impl;

// OBJ2 is uninitialized but can be initialized without arguments: initialize it
template<meta_object_t OBJ1, uninitialized_meta_object_t OBJ2, class ...Args>
  requires(!meta_states_object_t<OBJ1> &&
           no_arguments_initializable<typename OBJ2::function>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_object_invoke_impl<OBJ1, meta_invoke<OBJ2>, Args...>::type;
};
// Only when OBJ2 cannot be initialized without arguments: use the non-empty
// Args to initialize it, then pass its projection to OBJ1.
template<meta_object_t OBJ1, uninitialized_meta_object_t OBJ2, class ...Args>
  requires(!meta_states_object_t<OBJ1> &&
           !no_arguments_initializable<typename OBJ2::function> &&
           sizeof...(Args) > 0)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_object_invoke_impl<OBJ1, meta_invoke<OBJ2, Args...>>::type;
};
// No initialization arguments: return the uninitialized meta object and do not
template<meta_object_t OBJ1, uninitialized_meta_object_t OBJ2>
  requires(!meta_states_object_t<OBJ1> &&
           !no_arguments_initializable<typename OBJ2::function>)
struct meta_object_invoke_impl<OBJ1, OBJ2> {
  using type = meta_object_initialize<OBJ2>;
};
// has_type checks for a ::type member, i.e. that the meta object is
// initialized; a caller that is uninitialized itself needs a valid initializer.

// A pure meta object projects ::type.
template<meta_object_t OBJ1, pure_meta_object_t OBJ2, class ...Args> 
  requires(has_type<OBJ2> && ! meta_states_object_t<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, typename OBJ2::type, Args...>;
};

// A meta_ret_object projects ::ret.
template<meta_object_t OBJ1, meta_ret_object_t OBJ2, class ...Args>
  requires(has_type<OBJ2> && ! meta_states_object_t<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, typename OBJ2::ret, Args...>;
};

// A meta_timer_object projects ::type.
template<meta_object_t OBJ1, meta_timer_object_t OBJ2, class ...Args>
  requires(has_type<OBJ2> && ! meta_states_object_t<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, typename OBJ2::type, Args...>;
};
// A meta_states_object projects ::flags.
template<meta_object_t OBJ1, meta_states_object_t OBJ2, class ...Args>
  requires(has_type<OBJ2> && ! meta_states_object_t<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, typename OBJ2::flags, Args...>;
};
// An MSO observer receives the whole meta object, including another MSO, and
template<meta_states_object_t OBJ1, meta_object_t OBJ2, class ...Args>
  requires(has_type<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, OBJ2, Args...>;
};

}  // namespace meta_objects_invoke_details
// Non-MSO callers: when OBJ2 is uninitialized and cannot be initialized without
// arguments, Args only initialize OBJ2; with no Args this yields
// meta_object_initialize<OBJ2>, which still satisfies uninitialized_meta_object_t.
template <meta_object_t OBJ1, meta_object_t OBJ2, class ...Args> 
using meta_object_invoke =
    typename meta_objects_invoke_details::meta_object_invoke_impl<OBJ1, OBJ2, Args...>::type;

namespace meta_objects_invoke_details {
// Unwraps a meta object projection without invoking it:
//   meta_object      -> ::type
//   meta_ret_object  -> ::ret
//   meta_states_object -> ::flags
// This is where cache comes from: a value produced by the generator but not
template <meta_object_t MO>
struct meta_produce;

template <uninitialized_meta_object_t MO> 
struct meta_produce<MO>{
  using type = meta_uninitialized;
};
template <pure_meta_object_t MO> requires (exp_utilities::has_type<MO>)
struct meta_produce<MO>{
  using type = typename MO::type;
};
template <meta_timer_object_t MO> requires (exp_utilities::has_type<MO>)
struct meta_produce<MO>{
  using type = typename MO::type;
};
template <meta_ret_object_t MO> requires (exp_utilities::has_type<MO>)
struct meta_produce<MO> {
  using type = typename MO::ret;   // meta_ret_object projects ::ret
};
template <meta_states_object_t MO> requires (exp_utilities::has_type<MO>)
struct meta_produce<MO> {
  using type = typename MO::flags;   // meta_states_object projects ::flags
};
}  // namespace meta_objects_invoke_details

template <meta_object_t MO>
using meta_produce_t = typename meta_objects_invoke_details::meta_produce<MO>::type;

namespace invoke_object_if_details {
template <bool, class MO1, class MO2>
struct meta_o_branch {
  using type = MO1;
};
template <class MO1, class MO2>
struct meta_o_branch<true, MO1, MO2> {
  using type = meta_object_invoke<MO1, MO2>;
};
}  // namespace invoke_object_if_details

template <bool con>
struct invoke_object_if {
  template <class MO1, class MO2>
  using apply =
      typename invoke_object_if_details::meta_o_branch<con, MO1, MO2>::type;
};

}  // namespace meta_objects

// exp_size for meta_object_t passes straight through to its payload (::type).
// Measuring the length of a meta_object_t itself is meaningless, so exp_size is
// specialized here (a payload that is a list or carries ::length is measured by
// the original path). The specialization lives in this upper layer so that
// exp_utilities stays unaware of meta_object_t. Uninitialized meta objects have
// no ::type to pass through and keep the original exp_utilities path.
namespace exp_utilities {
template<meta_objects::meta_object_t MO>
  requires (has_type<MO>)
constexpr std::size_t exp_size<MO> = exp_size<typename MO::type>;
}  // namespace exp_utilities

namespace meta_loop {
using namespace meta_objects;

//the meta_looper use meta object transfer protocol to invoke the condition object
//incase there will be exceptions during the transformation, by default this condition
//allow uninitialized the pass the examination
template<class Fn, bool on_uninitialized_default>
struct meta_looper_condition_impl{
  template<this_ref pred_t, class ...Args>
  struct apply{
    static constexpr bool value = meta_invoke<Fn, pred_t>::value;
  };
  template<this_ref uninit, class...Args> requires (std::is_same_v<uninit, meta_uninitialized>)
  struct apply<uninit, Args...> : std::bool_constant<on_uninitialized_default>{};
};

template<class Fn, bool on_uninitialized = true>
using make_looper_condition = 
  meta_object<meta_uninitialized, meta_looper_condition_impl<Fn, on_uninitialized>>;


// The looper condition, state, and generator are meta objects.
namespace meta_looper_detail {
template <bool, class Condition, class OBJ, class Generator = meta_empty_o>
struct meta_looper_impl {
  template <class... Args>
  struct apply {
    // Adapt the current state to the condition's expected input, then evaluate
    // whether another iteration should run.
    using _continue_t =
        typename meta_invoke<meta_transfer_object<OBJ, Condition>>::type;
    static constexpr bool _continue_ = _continue_t::value;

    // Advance the generator only when the condition permits another iteration.
    using generator_stage_o =
        meta_invoke<invoke_if<_continue_>, Generator, Args...>;

    // Apply the generated input to the current state when continuing; otherwise
    // retain the current state as the final result.
    using result_stage_o =
        meta_invoke<invoke_object_if<_continue_>, OBJ, generator_stage_o>;

    // Instantiate the next iteration only when the condition remains true.
    using track_apply_t =
        meta_invoke<invoke_if<_continue_>,
                    meta_looper_impl<_continue_, Condition, result_stage_o,
                     generator_stage_o>,
                    Args...>;
    using type = typename track_apply_t::type;

    template <class... arg_types>
    static constexpr decltype(auto) for_each(auto&& f, arg_types&&... args) {
      using stage_t = typename result_stage_o::type;

      using return_type =
          std::invoke_result_t<decltype(f), stage_t, arg_types...>;

      if constexpr (std::is_void_v<return_type>) {
        if constexpr (track_apply_t::_continue_) {
          std::invoke(f, stage_t{}, std::forward<arg_types>(args)...);
          return track_apply_t::for_each(f, std::forward<arg_types>(args)...);
        } else {
          return std::invoke(f, stage_t{}, std::forward<arg_types>(args)...);
        }
      } else {
        if constexpr (track_apply_t::_continue_) {
          (void)std::invoke(f, stage_t{}, std::forward<arg_types>(args)...);
          return track_apply_t::for_each(f, std::forward<arg_types>(args)...);
        } else {
          return std::invoke(f, stage_t{},
                             std::forward<arg_types>(args)...);
        }
      }
    }

    // Applies each unary metafunction in Ps to the stage type before invoking
    // f. The folded type is used only for the callback; loop state is unchanged.
    template <template <class> class... Ps, class... arg_types>
      requires(sizeof...(Ps) > 0)
    static constexpr decltype(auto) for_each(auto&& f, arg_types&&... args) {
      using raw_stage = typename result_stage_o::type;
      using stage_t = meta_fold<raw_stage, Ps...>;

      using return_type =
          std::invoke_result_t<decltype(f), stage_t, arg_types...>;
      if constexpr (std::is_void_v<return_type>) {
        if constexpr (track_apply_t::_continue_) {
          std::invoke(f, stage_t{}, std::forward<arg_types>(args)...);
          return track_apply_t::template for_each<Ps...>(
              f, std::forward<arg_types>(args)...);
        } else {
          return std::invoke(f, stage_t{},
                             std::forward<arg_types>(args)...);
        }
      } else {
        if constexpr (track_apply_t::_continue_) {
          (void)std::invoke(f, stage_t{}, std::forward<arg_types>(args)...);
          return track_apply_t::template for_each<Ps...>(
              f, std::forward<arg_types>(args)...);
        } else {
          return std::invoke(f, stage_t{},
                             std::forward<arg_types>(args)...);
        }
      }
    }

    template <class first_arg_type, class... arg_types>
    static constexpr decltype(auto) for_each_forward(auto&& f,
                                                     first_arg_type&& first,
                                                     arg_types&&... args) {
      using stage_t = typename result_stage_o::type;

      using return_type =
          std::invoke_result_t<decltype(f), stage_t, first_arg_type>;

      if constexpr (std::is_void_v<return_type>) {
        if constexpr (track_apply_t::_continue_ && sizeof...(arg_types)) {
          std::invoke(f, stage_t{}, std::forward<first_arg_type>(first));
          return track_apply_t::for_each_forward(
              f, std::forward<arg_types>(args)...);
        } else {
          return std::invoke(f, stage_t{},
                             std::forward<first_arg_type>(first));
        }
      } else {
        if constexpr (track_apply_t::_continue_ && sizeof...(arg_types)) {
          (void)std::invoke(f, stage_t{},
                            std::forward<first_arg_type>(first));
          return track_apply_t::for_each_forward(
              f, std::forward<arg_types>(args)...);
        } else {
          return std::invoke(f, stage_t{},
                             std::forward<first_arg_type>(first));
        }
      }
    }

    // Forward one input argument to each callback, applying Ps to the stage
    // type first. Additional arguments are forwarded to subsequent iterations.
    template <template <class> class... Ps, class first_arg_type,
              class... arg_types>
      requires(sizeof...(Ps) > 0)
    static constexpr decltype(auto) for_each_forward(
        auto&& f, first_arg_type&& first, arg_types&&... args) {
      using raw_stage = typename result_stage_o::type;
      using stage_t = meta_fold<raw_stage, Ps...>;

      using return_type =
          std::invoke_result_t<decltype(f), stage_t, first_arg_type>;
      if constexpr (std::is_void_v<return_type>) {
        if constexpr (track_apply_t::_continue_ && sizeof...(arg_types)) {
          std::invoke(f, stage_t{}, std::forward<first_arg_type>(first));
          return track_apply_t::template for_each_forward<Ps...>(
              f, std::forward<arg_types>(args)...);
        } else {
          return std::invoke(f, stage_t{},
                             std::forward<first_arg_type>(first));
        }
      } else {
        if constexpr (track_apply_t::_continue_ && sizeof...(arg_types)) {
          (void)std::invoke(f, stage_t{},
                            std::forward<first_arg_type>(first));
          return track_apply_t::template for_each_forward<Ps...>(
              f, std::forward<arg_types>(args)...);
        } else {
          return std::invoke(f, stage_t{},
                             std::forward<first_arg_type>(first));
        }
      }
    }
  };
};

template <class Cond, class Receiver, class Generator>
struct meta_looper_impl<false, Cond, Receiver, Generator> {
  static constexpr bool _continue_ = false;
  using type = typename Receiver::type;
  template <class... Args>
  struct apply {
    using type = typename Receiver::type;
    template <class... arg_types>
    static constexpr decltype(auto) for_each(auto&&, arg_types&&...) { return; }
    template <class first_arg_type, class... arg_types>
    static constexpr decltype(auto) for_each_forward(auto&&, first_arg_type&&, arg_types&&...) { return; }
  };
};

}  // namespace meta_looper_detail


template <class C, class O, class G, class... ARG_Tys>
using meta_looper_t = typename meta_invoke<
  meta_looper_detail::meta_looper_impl<true, C, O, G>,
    ARG_Tys...>::type;

template <class C, class O, class G = meta_empty_o>
using meta_looper =
  meta_looper_detail::meta_looper_impl<true, C, O, G>;

// An existing condition passes through identity_fn unchanged, so
// make_looper_condition instantiates nothing.
namespace meta_for_detail {
struct make_condition_fn {
  template <class Con>
  using apply = make_looper_condition<Con>;
};
}  // namespace meta_for_detail

template<meta_function_t Con, meta_object_t To, meta_object_t From, class ...Args>
using meta_for = 
  meta_invoke<
    meta_looper<
      select_if_t<!meta_object_t<Con>, meta_for_detail::make_condition_fn,
                  identity_fn, Con>,
      To, From>,
    Args...
  >;

}  // namespace meta_loop
