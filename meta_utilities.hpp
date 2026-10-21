#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>
namespace exp_utilities {
namespace literal_types {
struct no_exist_type : std::false_type {};



struct end_of_list {
  using front = no_exist_type;
  using back = no_exist_type;
};

}  // namespace literal_types
template<class T>
constexpr bool no_exist = std::is_same_v<T, literal_types::no_exist_type>;
template<class TL>
constexpr bool eol = std::is_same_v<TL, literal_types::end_of_list>;

namespace exp_select_detail {
// Helper to find the largest power of 2 <= N (N > 0)
template <int N>
struct largest_pow2_le {
  static constexpr int value = N >= 32   ? 32
                               : N >= 16 ? 16
                               : N >= 8  ? 8
                               : N >= 4  ? 4
                               : N >= 2  ? 2
                                         : 1;
};

// Safely drop a power-of-2 number of elements from a type list.
// Returns literal_types::no_exist_type if the list is too short.
template <int N, class T>
struct safe_drop_pow2 {
  using type = literal_types::no_exist_type;
};

// N=0: drop nothing
template <class T>
struct safe_drop_pow2<0, T> {
  using type = T;
};

// Empty list with N>0: failure
template <int N, template <typename...> typename TL>
  requires(N > 0)
struct safe_drop_pow2<N, TL<>> {
  using type = literal_types::no_exist_type;
};

// N=1: drop exactly one element
template <template <typename...> typename TL, class T0, class... Ts>
struct safe_drop_pow2<1, TL<T0, Ts...>> {
  using type = TL<Ts...>;
};

// N>1 with at least two elements: recurse by halving
template <int N, template <typename...> typename TL, class T0, class T1,
          class... Ts>
  requires(N > 1)
struct safe_drop_pow2<N, TL<T0, T1, Ts...>> {
  using half = typename safe_drop_pow2<N / 2, TL<T0, T1, Ts...>>::type;
  using type = typename safe_drop_pow2<N / 2, half>::type;
};

// Forward declaration
template <int N, class T>
struct drop_impl;

// Helper to avoid instantiating drop_impl when the previous drop failed
template <bool Failed, int N, class T>
struct drop_impl_helper;

template <int N, class T>
struct drop_impl_helper<true, N, T> {
  using type = literal_types::no_exist_type;
};

template <int N, class T>
struct drop_impl_helper<false, N, T> {
  using type = typename drop_impl<N, T>::type;
};

// Drop N elements using binary decomposition
template <int N, class T>
struct drop_impl {
  static constexpr int P = largest_pow2_le<N>::value;
  using intermediate = typename safe_drop_pow2<P, T>::type;
  using type = typename drop_impl_helper<
      std::is_same<intermediate, literal_types::no_exist_type>::value, N - P,
      intermediate>::type;
};

template <class T>
struct drop_impl<0, T> {
  using type = T;
};

// Extract the first element of a type list, or no_exist_type if empty
template <class T>
struct first_type {
  using type = literal_types::no_exist_type;
};

template <template <typename...> typename TL, class First, class... Rest>
struct first_type<TL<First, Rest...>> {
  using type = First;
};

// The optimized select_type
template <int I, class T>
struct select_type {
  using dropped = typename drop_impl<I, T>::type;
  using type = typename first_type<dropped>::type;
};
}  // namespace exp_select_detail

template <std::size_t I, class TL>
using exp_select = typename exp_select_detail::select_type<I, TL>::type;

namespace exp_list_select_detail {
template <std::size_t... I>
struct list_select_impl {
  template <template <typename...> typename TL, class... Tys>
  static constexpr auto apply_impl(TL<Tys...>) {
    return TL<exp_select<I, TL<Tys...>>...>{};
  }

  template <typename TL>
  using apply = decltype(apply_impl(std::declval<TL>()));
};
}  // namespace exp_list_select_detail

template <std::size_t... I>
struct exp_list_select {
  template <typename TL>
  using apply = typename exp_list_select_detail::list_select_impl<
      I...>::template apply<TL>;
};

template <class index_sequence_t>
struct to_exp_list_select_t_impl;

template <std::size_t... I>
struct to_exp_list_select_t_impl<std::index_sequence<I...>> {
  using type = exp_list_select<I...>;
};

template <class index_sequence_t>
using to_exp_list_select_t =
    typename to_exp_list_select_t_impl<index_sequence_t>::type;

// take<N, TL>：取 TL 的前 N 个元素；N 大于长度时取全部。
// 用 std::tuple_element_t 一次展开（libstdc++/libc++ 在支持
// __type_pack_element 时是 O(1)/元素），不逐个走 exp_select。
namespace take_detail {
template <std::size_t N, class TL>
struct take_impl;

template <std::size_t N, template <class...> class TL, class... Ts>
struct take_impl<N, TL<Ts...>> {
  static constexpr std::size_t count =
      N < sizeof...(Ts) ? N : sizeof...(Ts);

  template <std::size_t... Is>
  static auto apply(std::index_sequence<Is...>)
      -> TL<std::tuple_element_t<Is, std::tuple<Ts...>>...>;

  using type = decltype(apply(std::make_index_sequence<count>{}));
};
}  // namespace take_detail

template <std::size_t N, class TL>
using take = typename take_detail::take_impl<N, TL>::type;

template <class... Tys>
struct exp_list;

namespace exp_list_details {
template <class TL>
struct to_exp_list {
  using type = literal_types::no_exist_type;
};
template <template <class...> class TL, class... Typs>
struct to_exp_list<TL<Typs...>> {
  using type = exp_list<Typs...>;
};

template <typename... Tys>
struct my_list {
  static constexpr std::size_t length = sizeof...(Tys);
};

template <class TL>
struct is_exp_list_based : std::false_type {};
template <template <class...> class TL, class... TS>
struct is_exp_list_based<TL<TS...>> {
  static constexpr bool value = std::is_base_of_v<exp_list<TS...>, TL<TS...>>;
};

// pop_back 不再逐个累加搬移，而是复用 take 的一次展开：
// 取前 N-1 个元素，每个元素走 tuple_element（O(1)/元素），
// 深度从 O(N) 降到 O(1) 级（不含 tuple_element 自身的实现）。
// exp_list::pop_back 的 API/结果类型保持不变。
}  // namespace exp_list_details

template <class First, class... Tys>
struct exp_list<First, Tys...> {
  static constexpr std::size_t length = sizeof...(Tys) + 1;
  template <template <class...> class TL>
  using to = TL<First, Tys...>;
  template <template <class> class F>
  using for_each = exp_list<F<First>, F<Tys>...>;
  template <class T>
  using push_back = exp_list<First, Tys..., T>;
  template <class T>
  using push_front = exp_list<T, First, Tys...>;
  using pop_front = exp_list<Tys...>;
  using pop_back = take<sizeof...(Tys), exp_list<First, Tys...>>;
  using front = First;
  using back =
      exp_select<sizeof...(Tys), exp_list_details::my_list<First, Tys...>>;
  template <std::size_t I>
    requires(I < length)
  using at = exp_select<I, exp_list<First, Tys...>>;
};

template <>
struct exp_list<> {
  static constexpr std::size_t length = 0;
  template <template <class...> class TL>
  using to = TL<>;
  template <template <class> class F>
  using for_each = exp_list<>;
  template <class T>
  using push_back = exp_list<T>;
  template <class T>
  using push_front = exp_list<T>;
  using pop_front = literal_types::end_of_list;
  using pop_back = literal_types::end_of_list;
  using front = literal_types::end_of_list;
  using back = literal_types::end_of_list;
};

template <class TL, class = void>
struct pop_last_impl {
  using type = TL;
};

template <class TL>
struct pop_last_impl<TL, std::void_t<decltype(TL::length)>> {
  using type =
      std::conditional_t<(TL::length >= 1),
                         typename to_exp_list_select_t<std::make_index_sequence<
                             TL::length - 1>>::template apply<TL>,
                         literal_types::end_of_list>;
};

template <class TL>
using pop_last = typename pop_last_impl<TL>::type;

template <class TL>
using to_exp_list_t = typename exp_list_details::to_exp_list<TL>::type;
namespace exp_function_info_details {
template <class R, class... Args>
using function_t = R(Args...);
template <class R, class C, class... Args>
using member_function_t = R (C::*)(Args...);

template <class F>
concept Functor = requires(F f) { &F::operator(); };

template <class F>
struct function_info;

template <class R, class... Args>
struct function_info<R (*)(Args...)> {
  using return_type = R;
  using argument_types = exp_list<Args...>;
};
template <class R, class C, class... Args>
struct function_info<R (C::*)(Args...)> {
  using return_type = R;
  using argument_types = exp_list<Args...>;
};

template <class R, class C, class... Args>
struct function_info<R (C::*)(Args...) const> {
  using return_type = R;
  using argument_types = exp_list<Args...>;
};

template <Functor F>
struct function_info<F> : function_info<decltype(&F::operator())> {};
}  // namespace exp_function_info_details

template <class F>
using exp_function_info = exp_function_info_details::function_info<F>;
template <class T>
struct any_caster {
  constexpr any_caster() noexcept : _dummy(), _has_value(false) {}
  constexpr any_caster(const T& val) : _has_value(false) {
    std::construct_at(&_value, val);
    _has_value = true;
  }
  constexpr any_caster(T&& val) : _has_value(false) {
    std::construct_at(&_value, std::move(val));
    _has_value = true;
  }
  constexpr any_caster(const any_caster& another) : _has_value(false) {
    if (another._has_value) {
      std::construct_at(&_value, another._value);
      _has_value = true;
    }
  }
  constexpr any_caster(any_caster&& another) noexcept : _has_value(false) {
    if (another._has_value) {
      std::construct_at(&_value, std::move(another._value));
      _has_value = true;
    }
  }
  // if T is constexpr - able, then this function will return a constexpr value
  // of T
  constexpr T constexpr_value() { return T{}; }

  // if T is constexpr - able, then this function will return a constexpr value
  // of T constructed with val
  template <auto val>
  constexpr T constexpr_value() {
    return T{val};
  }
  constexpr T value() { return _value; }
  constexpr any_caster& operator=(const any_caster& another) {
    if (this == &another) return *this;
    if (_has_value && another._has_value) {
      _value = another._value;
    } else if (_has_value && !another._has_value) {
      std::destroy_at(&_value);
      _has_value = false;
    } else if (!_has_value && another._has_value) {
      std::construct_at(&_value, another._value);
      _has_value = true;
    }
    return *this;
  }
  constexpr any_caster& operator=(any_caster&& another) {
    if (this == &another) return *this;
    if (_has_value && another._has_value) {
      _value = std::move(another._value);
    } else if (_has_value && !another._has_value) {
      std::destroy_at(&_value);
      _has_value = false;
    } else if (!_has_value && another._has_value) {
      std::construct_at(&_value, std::move(another._value));
      _has_value = true;
    }
    return *this;
  }
  union {
    char _dummy;
    T _value;
  };
  bool _has_value;
  constexpr ~any_caster() {
    if (_has_value) {
      std::destroy_at(&_value);
    }
  }
  operator T() { return _value; }
  constexpr explicit operator bool() const noexcept { return _has_value; }
  constexpr bool has_value() { return _has_value; }
  constexpr T& operator*() { return _value; }
  constexpr const T& operator*() const { return _value; }
};

template <class TL>
concept exp_list_based = exp_list_details::is_exp_list_based<TL>::value;
namespace exp_size_details {
template <class TL>
concept has_length = requires { TL::length; };
template <class TL>
struct type_list_size {
  static constexpr std::size_t value = 0;
};

template <class TL>
  requires has_length<TL>
struct type_list_size<TL> {
  static constexpr std::size_t value = TL::length;
};

template <template <class...> class TL, class... Tys>
  requires(!has_length<TL<Tys...>>)
struct type_list_size<TL<Tys...>> {
  static constexpr std::size_t value = sizeof...(Tys);
};
}  // namespace exp_size_details
template <class L>
constexpr std::size_t exp_size =
    exp_size_details::type_list_size<std::remove_cvref_t<L>>::value;

namespace max_index_details {
template <class TL>
struct max_index_type {
  static_assert((exp_size<TL> > 0), "Error: empty list");
  static constexpr std::size_t value = exp_size<TL> - 1;
};
}  // namespace max_index_details

template <class L>
constexpr std::size_t max_index = max_index_details::max_index_type<L>::value;

template <class L, std::size_t N>
constexpr bool length_equal = (exp_size<L> == N);

template <std::size_t N>
struct length_is {
  template <class L>
  struct apply : std::bool_constant<length_equal<L, N>> {};
};

template <std::size_t N>
struct length_is_not {
  template <class L>
  struct apply : std::bool_constant<!length_equal<L, N>> {};
};

template <std::size_t N>
struct length_less {
  template <class L>
      struct apply : std::bool_constant < exp_size<L><N> {};
};

template <std::size_t N>
struct length_greater {
  template <class L>
  struct apply : std::bool_constant<(exp_size<L> > N)> {};
};
namespace exp_find_detail {
template <class IDX, class T, class TL>
struct find_impl;

template <class IDX, class T, template <class...> class TL, class First,
          class... Ts>
struct find_impl<IDX, T, TL<First, Ts...>> {
  using type = typename std::conditional_t<
      std::is_same<T, First>::value,
      std::integral_constant<std::size_t, IDX::value>,
      typename find_impl<std::integral_constant<std::size_t, IDX::value + 1>, T,
                         TL<Ts...>>::type>;
};

template <class IDX, class T, template <class...> class TL>
struct find_impl<IDX, T, TL<>> {
  using type = IDX;  // idx overflow
};
}  // namespace exp_find_detail

template <class T, class TL>
using exp_find =
    typename exp_find_detail::find_impl<std::integral_constant<std::size_t, 0>,
                                        T, TL>::type;

template <class T, class TL>
using exp_try_find =
    std::integral_constant<bool, (max_index<TL> >= exp_find<T, TL>::value)>;

namespace get_type_detail {
template <class T, class U = std::void_t<>>
struct get_type_impl : std::false_type {
  using type = literal_types::no_exist_type;
};

template <class T>
struct get_type_impl<T, std::void_t<typename T::type>> : std::true_type {
  using type = typename T::type;
};
}  // namespace get_type_detail
template <class T>
using get_type = typename get_type_detail::get_type_impl<T>::type;

template <class T>
constexpr bool has_type = get_type_detail::get_type_impl<T>::value;

template <class T>
concept has_value = requires { T::value; };
template <class T, auto cmp>
constexpr bool value_equal = false;

template <class T, auto cmp>
  requires has_value<T>
constexpr bool value_equal<T, cmp> = T::value == cmp;

template<class T>
struct value_of_type{
  static constexpr auto value = literal_types::no_exist_type{};
};
template<class T> requires(has_type<T>)
struct value_of_type<T>{
  static constexpr auto value = T::value;
};

template<class T>
constexpr auto value_of = value_of_type<T>::value;

template <auto val>
struct value_is {
  template <class T>
  struct apply : std::bool_constant<value_equal<T, val>> {};
  template <class F2>
  struct OR {
    template <class T>
    struct apply : std::bool_constant<value_equal<T, val> ||
                                      F2::template apply<T>::value> {};
  };
  template <class F2>
  struct AND {
    template <class T>
    struct apply : std::bool_constant<value_equal<T, val> &&
                                      F2::template apply<T>::value> {};
  };
};

template <auto val>
struct value_is_not {
  template <class T>
  struct apply : std::bool_constant<!value_equal<T, val>> {};
  template <class F2>
  struct OR {
    template <class T>
    struct apply : std::bool_constant<!(value_equal<T, val> ||
                                        F2::template apply<T>::value)> {};
  };
  template <class F2>
  struct AND {
    template <class T>
    struct apply : std::bool_constant<!value_equal<T, val> &&
                                      F2::template apply<T>::value> {};
  };
};

template <class T>
struct type_is {
  template <class U>
  struct apply : std::bool_constant<std::is_same_v<get_type<U>, T>> {};
  template <class F2>
  struct OR {
    template <class U>
    struct apply : std::bool_constant<std::is_same_v<get_type<U>, T> ||
                                      F2::template apply<U>::value> {};
  };
  template <class F2>
  struct AND {
    template <class U>
    struct apply : std::bool_constant<std::is_same_v<get_type<U>, T> &&
                                      F2::template apply<U>::value> {};
  };
};

template <class T>
struct type_is_not {
  template <class U>
  struct apply : std::bool_constant<!std::is_same_v<get_type<U>, T>> {};
  template <class F2>
  struct OR {
    template <class U>
    struct apply : std::bool_constant<!(std::is_same_v<get_type<U>, T> ||
                                        F2::template apply<U>::value)> {};
  };
  template <class F2>
  struct AND {
    template <class U>
    struct apply : std::bool_constant<!std::is_same_v<get_type<U>, T> &&
                                      F2::template apply<U>::value> {};
  };
};

struct below_zero {
  static constexpr size_t value = static_cast<std::size_t>(-1);
};

template <std::size_t I>
using Idx = std::integral_constant<std::size_t, I>;

namespace exp_indices_details {
template <class _Idx, size_t _I>
struct Add_Idx {};

template <std::size_t I>
struct Add_Idx<below_zero, I> {
  using type = Idx<0>;
};
template <template <size_t> class idx, std::size_t I_in_Idx, std::size_t I>
struct Add_Idx<idx<I_in_Idx>, I> {
  using type = idx<I_in_Idx + I>;
};
template <std::size_t I, size_t ADD>
struct Add_Idx<Idx<I>, ADD> {
  using type = Idx<I + ADD>;
};
}  // namespace exp_indices_details

template <class idx, size_t I>
using add_idx_t = typename exp_indices_details::Add_Idx<idx, I>::type;

template <class _Idx>
using inc_idx_t = add_idx_t<_Idx, 1>;

template <size_t... _elements>
struct meta_array : exp_list<Idx<_elements>...> {
  using cv_typelist = exp_list<Idx<_elements>...>;
  template <template <size_t... I> class integer_array_type>
  using to = integer_array_type<_elements...>;
  template <size_t I>
  using at = exp_select<I, cv_typelist>;
  template <size_t I>
  static constexpr size_t get() {
    return at<I>::value;
  }

  static constexpr size_t length = cv_typelist::length;
  static constexpr size_t sum = (0 + ... + _elements);
  static consteval std::array<std::size_t, length> array() {
    return {_elements...};
  }
};

// Element has a static constexpr size_t value member
// (e.g. std::integral_constant<size_t, N>, Idx<N>, custom index types)
template <class T>
concept has_size_t_value = requires {
  { T::value } -> std::convertible_to<std::size_t>;
};

template <class TL>
struct to_meta_array {
  static_assert(sizeof(TL) == 0,
                "to_meta_array: not all elements have size_t value");
};

// Matches any type wrapper whose elements all expose size_t value
template <template <class...> class Wrapper, class... Ts>
  requires(has_size_t_value<Ts> && ...)
struct to_meta_array<Wrapper<Ts...>> {
  using type = meta_array<Ts::value...>;
};

template <class TL>
using to_meta_array_t = get_type<to_meta_array<TL>>;

template <char c>
struct exp_char {
  static constexpr char value = c;
  constexpr operator char() const { return value; }
};

namespace exp_str_detail {
template <char... str>
struct exp_str_impl : exp_list<exp_char<str>...> {
  constexpr exp_str_impl() {}
  const char m_str[sizeof...(str)]{str...};
  operator const char*() { return m_str; }
  static constexpr std::size_t length = sizeof...(str);
};
template <class TL>
struct to_exp_char {};
template <char... str>
struct to_exp_char<exp_list<exp_char<str>...>> {
  using type = exp_str_impl<str...>;
};

template <const char* p, class idx_ts>
struct my_sptr_impl_conv;

template <const char* p, std::size_t... idx>
struct my_sptr_impl_conv<p, std::index_sequence<idx...>> {
  using type = exp_str_detail::exp_str_impl<p[idx]...>;
};

template <std::size_t N, const char* p>
struct my_sptr {
  static constexpr auto to_my_ch() {
    using cnt_arr = std::make_index_sequence<N>;
    using ret_t = typename my_sptr_impl_conv<p, cnt_arr>::type;
    return ret_t{};
  }
};
template <std::size_t N, const char* p>
using meta_str = decltype(my_sptr<N, p>::to_my_ch());
}  // namespace exp_str_detail

template <class TL>
using to_exp_char_t = get_type<exp_str_detail::to_exp_char<to_exp_list_t<TL>>>;

template <std::size_t N>
struct static_str {
  constexpr static_str(char const (&s)[N]) { std::ranges::copy(s, str); }
  char str[N];
};

template <static_str ss>
constexpr auto operator""_exp_str() {
  constexpr auto size = sizeof(ss.str);
  return exp_str_detail::meta_str<size, ss.str>{};
}

template <static_str ss>
using str_to_list =
    typename exp_str_detail::meta_str<sizeof(ss.str),
                                      ss.str>::template to<exp_list>;
}  // namespace exp_utilities
