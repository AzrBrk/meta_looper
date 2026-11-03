#include "../bench/legacy_control_reference.hpp"

#include <cstdio>

namespace settle_tests {
using namespace meta_states;
namespace windows = meta_states_looper_details::meta_states_condition_funtion_detail;

constexpr bool compare(u128 value, u8 stage, bool failed) {
  const auto expected = legacy_control_reference::settle(value, stage, failed);
  const auto actual = windows::user_flags_control::settle_low(value.low, stage, failed);
  const auto wrapped = windows::user_flags_control::settle_value(value, stage, failed);
  return actual == expected.low && wrapped == expected &&
      windows::with_stage_value(value, stage) == legacy_control_reference::with_stage(value, stage);
}

constexpr bool boundaries() {
  for (u64 position : {u64{0}, u64{1}, u64{65535}}) {
    for (u64 timer : {u64{0}, u64{63}, u64{64}, u64{127}, u64{128}, u64{65535}}) {
      for (u8 stage : {pre_gen_exec, post_gen_exec, pre_rec_exec, post_rec_exec}) {
        for (bool finish : {false, true}) {
          for (bool failed : {false, true}) {
            const u128 value{position | (timer << timer0_shift) |
                (timer << timer1_shift) | (u64{0xFF} << user_shift) |
                G_CONTINUE | (finish ? G_FINISH : 0), ~u64{0}};
            if (!compare(value, stage, failed)) return false;
          }
        }
      }
    }
  }
  return true;
}
static_assert(boundaries());

constexpr bool random_cases(unsigned count) {
  u64 random = 0x123456789ABCDEF0ull;
  for (unsigned index = 0; index < count; ++index) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    const auto stage = static_cast<u8>(u64{0x10} << (index & 3));
    if (!compare(u128{random, ~random}, stage, (index & 4) != 0)) return false;
  }
  return true;
}
static_assert(random_cases(512));
}

int main() {
  const bool passed = settle_tests::boundaries() && settle_tests::random_cases(100000);
  std::puts(passed ? "PASS: low settlement boundaries and 100000 differential cases" : "FAIL");
  return passed ? 0 : 1;
}