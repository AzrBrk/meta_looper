#include "meta_stream.hpp"
#include "../examples/segmented_flags_prototype.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifndef BENCH_KIND
#define BENCH_KIND 3
#endif
#ifndef BENCH_SEGMENTS
#define BENCH_SEGMENTS 2
#endif
#ifndef BENCH_ROUNDS
#define BENCH_ROUNDS 256
#endif
#ifndef BENCH_WORK
#define BENCH_WORK 64
#endif
#ifndef BENCH_SWITCH
#define BENCH_SWITCH 0
#endif
#ifndef BENCH_VERIFY
#define BENCH_VERIFY 0
#endif

namespace segmented_compile_bench {
using word_type = std::uint64_t;
using meta_objects::meta_op_bits::u128;
using segmented_flags_prototype::segment_bank;
using segmented_flags_prototype::segment_window;

#if BENCH_KIND == 0
using storage = word_type;
#elif BENCH_KIND == 1 || BENCH_KIND == 4
using storage = u128;
#else
using storage = segment_bank<BENCH_SEGMENTS>;
#endif

constexpr word_type seed(std::size_t index) {
  return 0x123456789ABCDEFull ^ (0x9E3779B97F4A7C15ull * (index + 1));
}

constexpr storage initial() {
#if BENCH_KIND == 0
  return seed(0);
#elif BENCH_KIND == 1 || BENCH_KIND == 4
  return {seed(0), seed(1)};
#else
  storage result{};
  for (std::size_t index = 0; index < BENCH_SEGMENTS; ++index)
    result.segments[index] = seed(index);
  return result;
#endif
}

constexpr word_type mix(word_type value, std::size_t iteration) {
  value ^= value << (iteration % 61 + 1);
  value |= value >> (iteration % 37 + 3);
  value &= ~(word_type{iteration} << (iteration % 50));
  return value ^ (word_type{iteration} * 2654435761ull);
}

constexpr word_type wide_mix(word_type value, std::size_t iteration) {
  u128 wide{value};
  wide = wide ^ (wide << (iteration % 61 + 1));
  wide = wide & u128{~word_type{0}};
  wide = wide | (wide >> (iteration % 37 + 3));
  wide = wide & ~(u128{iteration} << (iteration % 50));
  return static_cast<word_type>(wide ^ u128{word_type{iteration} * 2654435761ull});
}

constexpr bool upper_selected([[maybe_unused]] std::size_t operation) {
#if BENCH_SWITCH == 2
  return (operation & 1) != 0;
#elif BENCH_SWITCH == 3
  return ((operation / 16) & 1) != 0;
#else
  return false;
#endif
}

constexpr storage advance(storage state, std::size_t round) {
#if BENCH_KIND == 3
  segment_window window(state);
#endif
  constexpr std::size_t operations = BENCH_WORK == 0 ? 1 : BENCH_WORK;
  for (std::size_t operation = 0; operation < operations; ++operation) {
    [[maybe_unused]] const auto iteration = round * operations + operation;
#if BENCH_KIND == 0
    word_type& current = state;
#elif BENCH_KIND == 1 || BENCH_KIND == 4
    word_type& current = upper_selected(operation) ? state.high : state.low;
#elif BENCH_KIND == 2
    word_type& current = state.segments[upper_selected(operation) ? BENCH_SEGMENTS - 1 : 0];
#else
#if BENCH_SWITCH == 1
    window.select<0>();
#elif BENCH_SWITCH == 2 || BENCH_SWITCH == 3
    if (upper_selected(operation)) window.select<BENCH_SEGMENTS - 1>();
    else window.select<0>();
#endif
    word_type& current = window.value;
#endif
#if BENCH_WORK == 0
  current = seed(0) ^ word_type{round + 1};
#elif BENCH_KIND == 1
    current = wide_mix(current, iteration);
#else
    current = mix(current, iteration);
#endif
  }
#if BENCH_KIND == 3
  window.commit();
#endif
  return state;
}

template<auto Value>
using state_constant = std::integral_constant<decltype(Value), Value>;

struct update_f {
  template<class Current, class Index>
  using apply = state_constant<advance(Current::value, Index::value)>;
};

using receiver = meta_objects::meta_object<state_constant<initial()>, update_f>;
using execution = meta_ios::meta_pipe<meta_iostream::index_sequence_istream<BENCH_ROUNDS>>
    ::run<receiver>;
using result = meta_objects::meta_produce_t<typename execution::type::to>;

#if BENCH_VERIFY
constexpr storage reference() {
  auto state = initial();
  constexpr std::size_t operations = BENCH_WORK == 0 ? 1 : BENCH_WORK;
  for (std::size_t round = 0; round < BENCH_ROUNDS; ++round) {
    for (std::size_t operation = 0; operation < operations; ++operation) {
#if BENCH_KIND == 0
      word_type& current = state;
#elif BENCH_KIND == 1 || BENCH_KIND == 4
      word_type& current = upper_selected(operation) ? state.high : state.low;
#else
      word_type& current = state.segments[upper_selected(operation) ? BENCH_SEGMENTS - 1 : 0];
#endif
#if BENCH_WORK == 0
  current = seed(0) ^ word_type{round + 1};
#else
      current = mix(current, round * operations + operation);
#endif
    }
  }
  return state;
}
static_assert(result::value == reference());
#endif

constexpr word_type digest() {
#if BENCH_KIND == 0
  return result::value;
#elif BENCH_KIND == 1 || BENCH_KIND == 4
  return result::value.low ^ result::value.high;
#else
  word_type value = 0;
  for (auto segment : result::value.segments) value ^= segment;
  return value;
#endif
}
}

std::uint64_t segmented_flags_compile_result() {
  return segmented_compile_bench::digest();
}