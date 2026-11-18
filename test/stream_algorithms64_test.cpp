#include "meta_stream_algorithms.hpp"

#include <cstdio>
#include <type_traits>

// §6.2: filter / take / transform / fold / collect on the two-word model.
// Mirrors the filter/take scenarios of examples/composition_smoke.cpp.

using namespace meta_ios;
using namespace meta_iostream;
using namespace meta_objects;
using exp_utilities::exp_list;
// NOTE: no `using namespace exp_utilities` -- its `take` (type-list slice)
// collides with meta_ios::take.

struct twice {
  template <class T>
  using apply = std::integral_constant<std::size_t, T::value * 2>;
};

template <std::size_t Stride>
struct divisible {
  template <class T>
  using apply = std::bool_constant<(T::value % Stride == 0)>;
};

struct sum {
  template <class State, class Input>
  using apply = std::integral_constant<std::size_t, State::value + Input::value>;
};

using ic0 = std::integral_constant<std::size_t, 0>;
using ic2 = std::integral_constant<std::size_t, 2>;
using ic4 = std::integral_constant<std::size_t, 4>;
using ic6 = std::integral_constant<std::size_t, 6>;

// --- 1) filter(divisible<2>) -> transform(twice) -> take(3) -> collect ------
// source emits 0..6; keep even (0,2,4,6) -> double (0,4,8,12) -> take 3.
using f_pipe = meta_pipe<index_sequence_istream<7>>
    ::all_to<filter<divisible<2>>, con<filter_con<true>>>
    ::all_to<transform<twice>>
    ::all_to<take<3>, con<take_con<>>>
    ::run<collect<>>;
// correct expected list: 0, 4, 8
using f_expect = exp_list<ic0,
                          std::integral_constant<std::size_t, 4>,
                          std::integral_constant<std::size_t, 8>>;
static_assert(std::is_same_v<f_pipe::type::to_t, f_expect>,
              "filter->transform->take->collect should yield 0,4,8");

// --- 2) observe count for a filtered pipeline --------------------------------
constexpr std::size_t f_calls = [] {
  std::size_t calls = 0;
  f_pipe::for_each([&](auto) { ++calls; });
  return calls;
}();
static_assert(f_calls == 3, "filtered pipeline must observe exactly 3 rounds");

// --- 3) take + collect, and take as the output node --------------------------
using take_collect = meta_pipe<index_sequence_istream<10>>
    ::all_to<take<2>, con<take_con<true>>>
    ::run<collect<>>;
static_assert(std::is_same_v<take_collect::type::to_t,
                             exp_list<ic0, std::integral_constant<std::size_t, 1>>>,
              "take<2> + collect should collect 0,1");

using final_take = meta_pipe<index_sequence_istream<10>>
    ::run<take<2>, con<take_con<true>>>;
constexpr std::size_t final_take_calls = [] {
  std::size_t calls = 0;
  final_take::for_each([&](auto) { ++calls; });
  return calls;
}();
static_assert(final_take_calls == 2, "take<2> output must observe exactly 2");

// --- 4) fold with a filter upstream ------------------------------------------
using folded = meta_pipe<index_sequence_istream<7>>
    ::all_to<filter<divisible<2>>, con<filter_con<true>>>
    ::all_to<transform<twice>>
    ::run<fold<std::integral_constant<std::size_t, 0>, sum>>;
// even of 0..6: 0,2,4,6 -> doubled: 0,4,8,12 -> sum 24
static_assert(folded::type::to_t::value == 24, "fold after filter should sum 24");

// --- 5) sparse filter (no matches until late) --------------------------------
using sparse = meta_pipe<index_sequence_istream<7>>
    ::all_to<filter<divisible<100>>, con<filter_con<true>>>
    ::all_to<take<3>, con<take_con<>>>
    ::run<collect<>>;
static_assert(exp_utilities::exp_size<sparse::type::to_t> == 1,
              "divisible<100> over 0..6 keeps only 0");

int main() {
  std::printf("stream_algorithms64_test: %s (f_calls=%zu final_take_calls=%zu)\n",
              "PASS", (std::size_t)f_calls, (std::size_t)final_take_calls);
  return 0;
}
