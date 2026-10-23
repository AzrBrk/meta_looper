#pragma once

#include <bit>

#include "meta_invoke_protocols.hpp"
#include "meta_uint128.hpp"

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

  // An object with a ::type payload is already initialized; preserve it unchanged.
  template<initialized_meta_object_t OBJ>
  struct meta_object_initialize_impl<OBJ>
  {
    using type = OBJ;
  };
  // If the initializer accepts the no_arguments marker, invoke OBJ without
  // explicit arguments. The initialization protocol supplies the marker.
  template<uninitialized_meta_object_t OBJ> 
    requires (no_arguments_initializable<typename OBJ::function>)
  struct meta_object_initialize_impl<OBJ>
  {
    using type = meta_invoke<OBJ>;
  };
  // Otherwise, defer initialization by returning a meta-function whose apply
  // forwards the caller's initialization arguments to OBJ.
  template<uninitialized_meta_object_t OBJ> 
    requires (!no_arguments_initializable<typename OBJ::function>)
  struct meta_object_initialize_impl<OBJ>
  {
    struct type{
      template<class ...init_args>
      using apply = meta_invoke<OBJ, init_args...>;
    };
  };
}

// Preserve an initialized object, initialize it without explicit arguments
// when supported, or return a deferred initializer meta-function.
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
template <template <std::size_t, class, class, class> class meta_timer_template,
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

// meta_empty_o 的识别位（它现在就是一个普通元对象，不再有特殊调用规则；
// 这个 concept 留给上层做"空对象/没接东西"的判断用）。
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

// ===== flags 变换步骤：一元元函数，输入/输出都是带 ::value 的 u128 类型 =====
// 无条件置位/清位
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
// 条件置位/清位：active 为 false 时原样返回 flags_t
template<u128 new_bits, bool active>
struct set_if_f{
  template<class flags_t>
  using apply = std::conditional_t<active,
      u128_int_t<u128{flags_t::value} | new_bits>, flags_t>;
};
template<u128 new_bits, bool active>
struct deact_if_f{
  template<class flags_t>
  using apply = std::conditional_t<active,
      u128_int_t<u128{flags_t::value} & ~new_bits>, flags_t>;
};

// ===== 字段宽度/掩码的命名空间级 trait（支持 bool 作为 1bit）=====
namespace flags_field_details {
  // 字段位宽：bool 为 1bit，其余为 sizeof(T)*8
  template <class T>
  struct field_bits : std::integral_constant<std::size_t, sizeof(T) * 8> {};
  template <>
  struct field_bits<bool> : std::integral_constant<std::size_t, 1> {};

  template <class T>
  constexpr std::size_t field_bits_v = field_bits<T>::value;

  // 字段的全 1 掩码值（未移位）：bool -> 1，其余 -> T(-1)
  template <class T>
  constexpr u128 field_ones =
      field_bits_v<T> == 128 ? ~u128{0} : ((u128{1} << field_bits_v<T>) - 1);

  // 字段掩码（移位到 shift）
  template <class T, std::size_t shift>
    requires (field_bits_v<T> <= 128 && shift <= 128 - field_bits_v<T>)
  constexpr u128 field_mask = field_ones<T> << shift;
}  // namespace flags_field_details

template<u128 flags>
struct flags_operator : u128_int_t<flags>
{
  constexpr operator u128() const noexcept { return flags; }
  // 单 bit 测试
  template<u128 bit>
  static constexpr bool has      = (flags & bit) == bit;   // 全部置位
  template<u128 bit>
  static constexpr bool has_any  = (flags & bit) != 0;     // 任一置位
  template<u128 bit>
  static constexpr bool test     = has_any<bit>;                // 常用别名

  // 整体测试
  static constexpr bool is_empty = (flags == 0);
  static constexpr bool any      = (flags != 0);
  static constexpr int  count    = std::popcount(flags.low) + std::popcount(flags.high);

  
  // ===== 原始按位操作（u128 掩码）=====
  template<u128 bit>
  using opSet = flags_operator<flags | bit>;
  template<u128 bit>
  using opDeact = flags_operator<flags & ~bit>;
  template<u128 bit>
  using opToggle = flags_operator<flags ^ bit>;

  template<u128 bit, bool con = true>
  using opToggle_if = std::conditional_t<con, opToggle<bit>, flags_operator<flags>>;

  // ===== 字段访问：指定类型 + 偏移 =====
  // 生成指定位置的字段掩码（bool 为 1bit）
  template<class uintegral_type, std::size_t shift>
  static constexpr u128 mask = flags_field_details::field_mask<uintegral_type, shift>;

  // 获取指定位置的字段值，右移到最低位后按字段类型返回
  template<class uintegral_type, std::size_t shift>
  static constexpr uintegral_type get =
      static_cast<uintegral_type>((flags & mask<uintegral_type, shift>) >> shift);
  // 字段级测试
  template<class T, std::size_t shift>
  static constexpr bool field_any = (get<T, shift> != 0);
  template<class T, std::size_t shift, T v>
  static constexpr bool field_eq  = (get<T, shift> == v);
  template<class T, std::size_t shift>
  static constexpr bool field_none = (get<T, shift> == 0);

  // ===== 字段写入：set<T, shift, value> =====
  // 把字段类型的值 value 写入从 shift 开始的字段（掩码内），其他位清零后写入。
  template<class uintegral_type, std::size_t shift, uintegral_type value>
  using set = flags_operator<
      (flags & ~mask<uintegral_type, shift>) |
      ((static_cast<u128>(value) << shift) & mask<uintegral_type, shift>)>;

  // ===== 字段清空：deact<T, shift> =====
  // 把从 shift 开始的字段清零，其余位不变。
  template<class uintegral_type, std::size_t shift>
  using deact = flags_operator<flags & ~mask<uintegral_type, shift>>;

  // ===== 条件字段写入/清空 =====
  template<bool con, class uintegral_type, std::size_t shift, uintegral_type value>
  using set_if = std::conditional_t<con,
      set<uintegral_type, shift, value>, 
      flags_operator<flags>
    >;
  template<bool con, class T, std::size_t shift, T v_true, T v_false>
  using set_if_else = set<T, shift, con ? v_true : v_false>;

  // 条件位选择器：根据布尔条件选择性地设置或清除特定位
  template<bool con, u128 bit_true, u128 bit_false>
  using select = flags_operator<con ? (flags | bit_true) & ~bit_false
                                    : (flags | bit_false) & ~bit_true>;
  template<bool con, class uintegral_type, std::size_t shift>
  using deact_if = std::conditional_t<con,
      deact<uintegral_type, shift>, flags_operator<flags>>;
  // 字段按位
  template<class T, std::size_t shift, T v>
  using field_or  = set<T, shift, static_cast<T>(get<T, shift> | v)>;
  template<class T, std::size_t shift, T v>
  using field_and = set<T, shift, static_cast<T>(get<T, shift> & v)>;
  template<class T, std::size_t shift, T v>
  using field_xor = set<T, shift, static_cast<T>(get<T, shift> ^ v)>;

  // 字段自增/自减（比如计数器字段）
  template<class T, std::size_t shift, T delta = 1>
  using inc = set<T, shift, static_cast<T>(get<T, shift> + delta)>;
  template<class T, std::size_t shift, T delta = 1>
  using dec = set<T, shift, static_cast<T>(get<T, shift> - delta)>;

  // 纯位运算的加减：把 flags 当一个大整数，直接在 shift 处加上/减去 delta。
  //   add_at / sub_at：flags ± (delta << shift)
  // 和 inc / dec 的区别是"不掩码、不检查边界"：
  //   inc / dec 走 set<...>，结果被截断在字段掩码内，溢出只在字段里打转，绝不碰邻居；
  //   add_at / sub_at 什么都不挡，进位/借位会直接漏到相邻字段。
  //   不需要边界保护、或者只想省掉那一串掩码时用它（也方便把 flags 当普通整数累加）。
  // delta 是模板参数；运行期的量直接用 flags + (u128(delta) << shift)。
  template<std::size_t shift, u128 delta = 1>
  using add_at = flags_operator<flags + (delta << shift)>;
  template<std::size_t shift, u128 delta = 1>
  using sub_at = flags_operator<flags - (delta << shift)>;
  // F 是一个 constexpr 函数 T(T)
  template<class T, std::size_t shift, auto F>
  using update = set<T, shift,
      static_cast<T>(F(get<T, shift>))>;
  // 用法：update<u8, 4, [](u8 v){ return v + 1; }>
  constexpr bool operator==(const flags_operator& other) const noexcept {
    return flags == other.value;
  }
  constexpr bool operator!=(const flags_operator& other) const noexcept {
    return !(*this == other);
  }
  // 或
  template<u128 other>
  static constexpr bool eq = (flags == other);
  template<u128 other>
  static constexpr bool ne = (flags != other);
  template<u128 other> using opOr  = flags_operator<flags | other>;
  template<u128 other> using opAnd = flags_operator<flags & other>;
  template<u128 other> using opXor = flags_operator<flags ^ other>;
  using opNot = flags_operator<~flags>;

  // 与另一个 flags_operator 实例合并
  template<class Other>
  using merge     = flags_operator<flags | Other::value>;
  template<class Other>
  using intersect = flags_operator<flags & Other::value>;
  template<class Other>
  using diff      = flags_operator<flags & ~u128{Other::value}>;   // A 去掉 B

  using reset = flags_operator<0>;
  template<u128 keep>
  using keep_only = flags_operator<flags & keep>;
};
template<class T>
struct is_flags_operator : std::false_type {};
template<u128 flags>
struct is_flags_operator<flags_operator<flags>> : std::true_type {};
template <class T>
constexpr bool is_flags_operator_v = is_flags_operator<T>::value;
}  // namespace meta_op_bits

// ===== meta_states_object（MSO）：持有 flags 的纯状态元对象 =====
// 它是 MO 的普通一员，符合 MO 调用规范：
//   - meta_invoke<MSO, objects...> 返回 MSO 的新状态
//   - MO 之间可相互调用（见 Meta_Object_Invoke 的 MSO 协议特化）
//   - MSO 作为观察者接收完整元对象，包括另一个 MSO；需要其状态时显式读取 ::flags。
// MSO 自身不知道"生成器/接收器/looper"，那些是 meta_looper 用 MSO 实现条件判断的行为。
//
// 用户的 Fn 接收 flags_operator（this_flags），用 set/deact/set_if/deact_if 链式变换，
// Fn::apply<this_flags, objects...> 的结果成为新 MSO 的 flags。
//
// 签名：template<class Flags, class Fn>
// meta_states_object返回指定类型生成的特定bits
// meta_states_object 的产出是 ::flags
// meta_states_object 不规定bits的具体定义，请使用者自己实现自定义的bits布局
// meta_states_object 可以携带特定的执行器（executor）以实现自定义的行为
// 执行器仅供提取，不会直接影响 meta_states_object 的 flags。

using meta_op_bits::flags_operator;
using meta_op_bits::u64;
using meta_op_bits::u128;
template<class T>
concept flags_operator_type = meta_op_bits::is_flags_operator_v<T>;

template<template<auto> class Fn>
struct states_executor{
  template<class flags, class ...Args>
  using apply = meta_invoke<Fn<flags::value>, Args...>;
};

template <u128 Flags, class Fn>
struct meta_states_object {
  using flags = flags_operator<Flags>;
  using function = Fn;

  // 变参 apply：把 flags 和传入的 objects 一起交给用户的 Fn。
  // Fn::apply<flags, objects...>,正如其他MSO，以及meta_invoke的定义，apply直接就是指定类型，不需要::type解压
  // 注意：::value 是静态数据成员而非类型，requires 中不能加 typename。
  template <class... objects> requires requires { meta_invoke<Fn, flags, objects...>::value; }
  using apply = meta_states_object<
      meta_invoke<Fn, flags, objects...>::value, Fn>;

  // 直接使用 flags_operator 类型进行重置
  template <flags_operator_type ANOTHER_FLAGS>
  using meta_set = meta_states_object<ANOTHER_FLAGS::value, Fn>;

  // MSO 的产物是它的 flags。
  using type = flags;
  static constexpr auto value = flags::value;
};
// 检测是否为 meta_states_object 的实例（定义见 meta_timer_object 之后）。
// MSO 的 Flags 载体约定为 meta_op_bits::flags_operator。
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

//当第二个元对象未初始化且可以无参初始化时，尝试无参初始化后再调用
template<meta_object_t OBJ1, uninitialized_meta_object_t OBJ2, class ...Args>
  requires(!meta_states_object_t<OBJ1> &&
           no_arguments_initializable<typename OBJ2::function>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_object_invoke_impl<OBJ1, meta_invoke<OBJ2>, Args...>::type;
};
//当第二个元对象未初始化且无法无参初始化时，提供一个带初始参数的初始化结构
template<meta_object_t OBJ1, uninitialized_meta_object_t OBJ2, class ...Args>
  requires(!meta_states_object_t<OBJ1> &&
           !no_arguments_initializable<typename OBJ2::function>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  struct type
  {
    template<class ...initial_args>
    using apply = meta_object_invoke_impl<OBJ1, meta_invoke<OBJ2, initial_args...>, Args...>::type;
  };
};
//使用has_type检查元对象是否具有type成员，确保元对象已经初始化
//如果调用方也未初始化，必须具有合法的初始化函数

//pure meta object 返回::type
template<meta_object_t OBJ1, pure_meta_object_t OBJ2, class ...Args> 
  requires(has_type<OBJ2> && ! meta_states_object_t<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, typename OBJ2::type, Args...>;
};

//meta_ret_object 返回::ret
template<meta_object_t OBJ1, meta_ret_object_t OBJ2, class ...Args>
  requires(has_type<OBJ2> && ! meta_states_object_t<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, typename OBJ2::ret, Args...>;
};

//meta_timer_object 返回::type
template<meta_object_t OBJ1, meta_timer_object_t OBJ2, class ...Args>
  requires(has_type<OBJ2> && ! meta_states_object_t<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, typename OBJ2::type, Args...>;
};
//meta_states_object 返回::flags
template<meta_object_t OBJ1, meta_states_object_t OBJ2, class ...Args>
  requires(has_type<OBJ2> && ! meta_states_object_t<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, typename OBJ2::flags, Args...>;
};
//meta_states_object 作为观察者获得整个元对象本身（包括 MSO，不自动初始化）
template<meta_states_object_t OBJ1, meta_object_t OBJ2, class ...Args>
  requires(has_type<OBJ1>)
struct meta_object_invoke_impl<OBJ1, OBJ2, Args...> {
  using type = meta_invoke<OBJ1, OBJ2, Args...>;
};

}  // namespace meta_objects_invoke_details
//如果OBJ2 未初始化，且无法无参数初始化，将返回一个元函数，等待初始化参数再调用。
//可以使用meta_invoke_protocols::is_meta_function_v检测返回的是否为元函数
template <meta_object_t OBJ1, meta_object_t OBJ2, class ...Args> 
using meta_object_invoke =
    typename meta_objects_invoke_details::meta_object_invoke_impl<OBJ1, OBJ2, Args...>::type;

namespace meta_objects_invoke_details {
// 按协议解包元对象的【产出】（不调用）：
//   meta_object      -> ::type
//   meta_ret_object  -> ::ret
//   meta_states_object -> ::flags
// 这是 cache 的来源：生成器产出但未被接收器消费的值。
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
  using type = typename MO::ret;   // meta_ret_object 的产出是 ::ret
};
template <meta_states_object_t MO> requires (exp_utilities::has_type<MO>)
struct meta_produce<MO> {
  using type = typename MO::flags;   // meta_states_object 的产出是 ::flags
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

// meta_object_t 的 exp_size：直接穿透到它持有的 payload（::type）。
// 对着 meta_object_t 本身量长度没有意义，所以这里特化 exp_size 这个变量模板
// （payload 若是列表/其它带 length 的东西，自然由原来的路径算）。
// 特化只写在上层（meta_objects 这边），exp_utilities 不预知 meta_object_t 的存在。
// 未初始化的元对象没有 ::type 可穿透，仍走 exp_utilities 原本的路径。
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
    static const bool _continue_ = _continue_t::value;

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

template<meta_function_t Con, meta_object_t To, meta_object_t From, class ...Args>
using meta_for = 
  meta_invoke<
    meta_looper<std::conditional_t<meta_object_t<Con>, Con, make_looper_condition<Con>>, To, From>,
    Args...
  >;

}  // namespace meta_loop
