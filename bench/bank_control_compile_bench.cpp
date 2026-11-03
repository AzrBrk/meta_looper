#include "../meta_stream.hpp"

#include <type_traits>

#ifndef BENCH_KIND
#define BENCH_KIND 4
#endif
#ifndef BENCH_SEGMENTS
#define BENCH_SEGMENTS 16
#endif
#ifndef BENCH_ROUNDS
#define BENCH_ROUNDS 128
#endif
#ifndef BENCH_VERIFY
#define BENCH_VERIFY 0
#endif
#ifndef BENCH_LEGACY
#define BENCH_LEGACY 0
#endif

#if BENCH_LEGACY || BENCH_VERIFY
#include "legacy_control_reference.hpp"
#endif

namespace bank_control_compile_bench {
using namespace meta_objects;
using meta_states::u16;
namespace windows = meta_states_looper_details::meta_states_condition_funtion_detail;

constexpr auto initial() {
#if BENCH_KIND == 4
  return u128{};
#else
  segment_bank<BENCH_SEGMENTS> bank{};
  for (std::size_t index = 2; index < BENCH_SEGMENTS; ++index)
    bank.write(index, 0x9E3779B97F4A7C15ull ^ index);
  return bank;
#endif
}

template<auto Value>
using state_constant = std::integral_constant<decltype(Value), Value>;

struct update_f {
  template<class Current, class Index>
  struct apply {
    static constexpr auto staged = [] {
      constexpr u64 stage = u64{0x10} << (Index::value & 3);
#if BENCH_KIND == 4
      return u128{stage << windows::stage_shift, Current::value.high};
#else
      auto bank = Current::value;
      bank.write(0, stage << windows::stage_shift);
      return bank;
#endif
    }();
  #if BENCH_LEGACY
    using control = meta_states_object<staged, legacy_control_reference::control>;
  #else
    using control = meta_states_object<staged, windows::user_flags_control>;
  #endif
    using condition = meta_ios::stream_states_co<>;
    using input_flags = condition::flags::set<u16, meta_states::position_shift,
        static_cast<u16>(Index::value)>;
    using input = condition::meta_set<input_flags>;
    using called = meta_object_invoke<control, input>;
    using output = state_constant<called::value>;
  };

  template<class Current, class Index>
  using apply_result = typename apply<Current, Index>::output;
};

struct receiver_f {
  template<class Current, class Index>
  using apply = update_f::apply_result<Current, Index>;
};

using receiver = meta_object<state_constant<initial()>, receiver_f>;
using execution = meta_ios::meta_pipe<meta_iostream::index_sequence_istream<BENCH_ROUNDS>>
    ::run<receiver>;
using result = meta_produce_t<typename execution::type::to>;

#if BENCH_VERIFY
static_assert([] {
  auto expected = initial();
  for (std::size_t index = 0; index < BENCH_ROUNDS; ++index) {
    constexpr auto base = meta_ios::stream_states_co<>::value;
    auto input = base;
    input.low = (input.low & ~meta_states::position_mask) | static_cast<u16>(index);
    const auto stage = static_cast<meta_states::u8>(u64{0x10} << (index & 3));
    auto settled = legacy_control_reference::settle(input, stage);
#if BENCH_KIND == 4
    expected = settled;
#else
    expected.import_u128(settled);
#endif
  }
  return result::value == expected;
}());
#endif
}

std::uint64_t bank_control_compile_result() {
#if BENCH_KIND == 4
  return bank_control_compile_bench::result::value.low;
#else
  return bank_control_compile_bench::result::value.read(0);
#endif
}