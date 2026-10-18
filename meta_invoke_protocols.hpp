#pragma once

#include "meta_utilities.hpp"

namespace meta_invoke_protocols {
template <std::size_t>
struct dummy {};

template <template <class...> class apply_shape>
struct meta_function_template_container {};

template <class F>
concept meta_function_t =
    requires { typename meta_function_template_container<F::template apply>; };

template <template <class...> class F, std::size_t... Is>
consteval bool can_instantiate(std::index_sequence<Is...>) {
  return requires { typename F<dummy<Is>...>; };
}

template <template <class...> class F, std::size_t N = 0,
          std::size_t Limit = 64>
consteval std::size_t meta_alias_argc() {
  static_assert(N <= Limit, "template parameter count exceeds limit");

  if constexpr (can_instantiate<F>(std::make_index_sequence<N>{})) {
    if constexpr (!can_instantiate<F>(std::make_index_sequence<N + 1>{})) {
      return N;
    } else {
      return meta_alias_argc<F, N + 1, Limit>();
    }
  } else {
    return meta_alias_argc<F, N + 1, Limit>();
  }
}

}  // namespace meta_invoke_protocols

using meta_invoke_protocols::meta_function_t;

template <class F>
constexpr bool is_meta_function_v = meta_function_t<F>;

namespace meta_invoke_detail {
template <class F, class... L>
struct impl {
  static_assert(meta_function_t<F>,
                "First parameter of meta_invoke must be a meta function");
};
template <class F, class... L>
  requires meta_function_t<F>
struct impl<F, L...> {
  using type = typename F::template apply<L...>;
};

template <class F, class TL>
struct type_list_invoke;

template <class F, template <class...> class L, class... Ts>
struct type_list_invoke<F, L<Ts...>> {
  using type = typename impl<F, Ts...>::type;
};
}  // namespace meta_invoke_detail

template <class F, class... L>
using meta_invoke = typename meta_invoke_detail::impl<F, L...>::type;

template <class F, class TL>
using meta_list_invoke =
    typename meta_invoke_detail::type_list_invoke<F, TL>::type;

namespace meta_invoke_if_detail {
template <bool con, class F, class... Args>
struct meta_function_branch {
  using type = F;
};
template <class F, class... Args>
struct meta_function_branch<true, F, Args...> {
  using type = meta_invoke<F, Args...>;
};
}  // namespace meta_invoke_if_detail

template <bool con>
struct invoke_if {
  template <class F, class... Args>
  using apply =
      typename meta_invoke_if_detail::meta_function_branch<con, F,
                                                           Args...>::type;
};

namespace meta_quote {

template <template <typename> class Template>
struct unary {
  template <typename Arg>
  using apply = Template<Arg>;
};

template <template <typename, typename> class Template>
struct binary {
  template <typename T, typename U>
  using apply = Template<T, U>;
};

template <template <typename, typename> class Template, typename Arg>
struct bind_binary {
  template <typename U>
  using apply = Template<Arg, U>;
};

}  // namespace meta_quote

namespace meta_fold_detail {

template <class T, template <class> class... Fs>
struct impl {};

// no metafunction: result is T itself
template <class T>
struct impl<T> {
  using type = T;
};

template <class T, template <class> class F>
struct impl<T, F> {
  using type = F<T>;
};

template <class T, template <class> class First, template <class> class... Rest>
struct impl<T, First, Rest...> {
  using type = typename impl<First<T>, Rest...>::type;
};

}  // namespace meta_fold_detail

template <class T, template <class> class... Fs>
using meta_fold = typename meta_fold_detail::impl<T, Fs...>::type;

template <template <class> class... meta_templates>
struct meta_fold_list {
  template <class T>
  using apply = meta_fold<T, meta_templates...>;
};
