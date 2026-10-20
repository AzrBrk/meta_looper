#include "meta_ios.hpp"

#include <type_traits>
#include <utility>

using namespace meta_ios;
using namespace meta_iostream;
using namespace meta_states;

namespace pipe_boundary_tests {

struct counting_main_fn {
  template <class Flags, class Object>
  struct apply {
    using counted = typename Flags::template inc<u8, user_shift>;
    using low_only = meta_objects::flags_operator<counted::value.low>;
    using chosen = std::conditional_t<
        (Flags::template get<u16, position_shift> >= 3),
        typename low_only::template opDeact<G_CONTINUE>, low_only>;
    static constexpr auto value = chosen::value;
  };
};

using main_seed = meta_objects::flags_operator<condition_seed>
  ::set<u8, user_shift, 0>;
using counting_main = meta_objects::meta_states_object<main_seed::value,
                             counting_main_fn>;

struct counter_fn {
  template <class Current>
  using apply = std::integral_constant<unsigned, Current::value + 1>;
};

using infinite_source = meta_objects::meta_object<
    counter_fn, std::integral_constant<unsigned, 0>>;
using bounded = meta_pipe<infinite_source>::run<
    meta_ostream<>, con<node_con<>>, main_con<counting_main>>;
static_assert(exp_utilities::exp_size<typename bounded::type::to_t> == 3);
static_assert(bounded::type::states::template get<u8, user_shift> == 3);
static_assert(bounded::type::states::template get<u16, position_shift> == 3);
static_assert(bounded::type::states::template get<meta_objects::u64, 64> == 1);

struct kill_output_fn {
  template <class Window, class Flags, class Object>
  struct apply {
    static_assert(user_fn::index_of<Flags>() < 2);
    using chosen = std::conditional_t<
        Window::value == post_rec_exec && user_fn::index_of<Flags>() == 1,
        typename Flags::template opDeact<G_CONTINUE>, Flags>;
    static constexpr auto value = chosen::value;
  };
};

using tail_stopped = meta_pipe<meta_istream_list<int, double, char, long>>
    ::all_to<meta_iterator>
    ::run<meta_ostream<>, con<node_con<kill_output_fn>>>;
static_assert(std::is_same_v<typename tail_stopped::type::to_t,
                             exp_list<int, double>>);
static_assert(tail_stopped::type::states::template get<meta_objects::u64, 64> == 0);
static_assert(tail_stopped::type::states::template get<u16, position_shift> == 5);

template <std::size_t Index>
using boundary_node = meta_stream_details::pipe_node<
    meta_stream_details::default_node_con<Index == 0>, meta_iterator>;

template <std::size_t... Index>
auto boundary_runner(std::index_sequence<Index...>)
    -> meta_stream_details::meta_pipe_runner<
        meta_stream_details::pipe_main_co,
        meta_stream_details::pipe_nodes<boundary_node<sizeof...(Index) - Index - 1>...>,
        meta_istream_list<int>>;

#ifdef META_PIPE_TEST_TOO_MANY_NODES
using boundary = decltype(boundary_runner(std::make_index_sequence<65>{}));
#else
using boundary = decltype(boundary_runner(std::make_index_sequence<64>{}));
#endif
static_assert(std::is_same_v<typename boundary::type::to_t, int>);
static_assert(boundary::type::states::template get<meta_objects::u64, 64> == 0);

constexpr bool check_callbacks() {
  unsigned bounded_calls = 0;
  bounded::for_each([&](auto) { ++bounded_calls; });
  unsigned tail_calls = 0;
  tail_stopped::for_each([&](auto) { ++tail_calls; });
  unsigned boundary_calls = 0;
  boundary::for_each([&](auto) { ++boundary_calls; });
  return bounded_calls == 3 && tail_calls == 2 && boundary_calls == 1;
}
static_assert(check_callbacks());

}

int main() {
  return pipe_boundary_tests::check_callbacks() ? 0 : 1;
}