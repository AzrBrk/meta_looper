#include "../meta_stream_control.hpp"
#include "../meta_iostream.hpp"

#include <cstdio>

#ifndef BENCH_ROUNDS
#define BENCH_ROUNDS 256
#endif
#ifndef BENCH_KIND
#define BENCH_KIND 0
#endif

namespace medium_smoke {
using namespace meta_iostream;
using namespace meta_ios;

template<class... Types>
struct type_list {};

#if BENCH_KIND == 1
using receiver = meta_ostream<exp_utilities::exp_list<>>;
#elif BENCH_KIND == 2
using receiver = meta_ostream<type_list<>>;
#elif BENCH_KIND == 3
using receiver = meta_aligned_iterator<>;
#else
using receiver = meta_iterator;
#endif

using execution = meta_states::run<stream_states_co<>, receiver,
    index_sequence_istream<BENCH_ROUNDS>>;
using result = meta_objects::meta_produce_t<typename execution::type::to>;

#if BENCH_KIND == 1 || BENCH_KIND == 2
static_assert([]<std::size_t... Index>(std::index_sequence<Index...>) {
#if BENCH_KIND == 1
  using expected = exp_utilities::exp_list<std::integral_constant<std::size_t, Index>...>;
#else
  using expected = type_list<std::integral_constant<std::size_t, Index>...>;
#endif
  return std::same_as<result, expected>;
}(std::make_index_sequence<BENCH_ROUNDS>{}));
#elif BENCH_KIND == 3
static_assert(result::type::value == BENCH_ROUNDS - 1);
#else
static_assert(result::value == BENCH_ROUNDS - 1);
#endif

constexpr bool check_callbacks() {
  std::size_t calls = 0;
  bool ordered = true;
  execution::for_each([&](auto view) {
    ordered = ordered && view.index() == calls;
    ++calls;
  });
  return ordered && calls == BENCH_ROUNDS;
}
static_assert(check_callbacks());
}

int main() {
  const bool passed = medium_smoke::check_callbacks();
  std::printf("%s: %d integer-stream rounds, receiver mode %d\n",
      passed ? "PASS" : "FAIL", BENCH_ROUNDS, BENCH_KIND);
  return passed ? 0 : 1;
}