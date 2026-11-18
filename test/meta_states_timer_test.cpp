// §6.3 — the redesigned timer/tool helpers in meta_states.hpp (two-word model).
// D7: the timer *count* lives in the looper word's T0C/T1C; the *target* is a
// type, so the word stays a plain u64.
#include "meta_states.hpp"

#include <string>
#include <type_traits>
#include <utility>

using namespace meta_states;
using namespace meta_core64;

// ---- timer_target / no_timer_target: target carried as a type ----
static_assert(timer_target<gates::rec>::bit == gates::rec);
static_assert(timer_target<gates::obs>::bit == gates::obs);
static_assert(no_timer_target::bit == 0);

// ---- timer_count: read a slot's count from the looper word ----
constexpr u64 L0 = lw::make(gates::defaults, 0, 9, 4, 0);
static_assert(timer_count<0>(L0) == 9);
static_assert(timer_count<1>(L0) == 4);

// ---- tick_timers: saturating decrement on both slots ----
static_assert(timer_count<0>(tick_timers(L0)) == 8);
static_assert(timer_count<1>(tick_timers(L0)) == 3);
static_assert(timer_count<0>(tick_timers(lw::make(0, 0, 0, 0, 0))) == 0);  // 0 stays 0
static_assert(timer_count<1>(tick_timers(lw::make(0, 0, 0, 0, 0))) == 0);

// ---- effective: running slot clears its target bit (target in the word) ----
// slot 0 running (count 3) with target rec -> REC cleared
constexpr u64 L1 = lw::with_target0(
    lw::make(gates::defaults, 0, 3, 0, 0),
    static_cast<u8>(std::countr_zero(gates::rec)));
static_assert(effective(L1) == (gates::defaults & ~gates::rec));
// both slots empty -> gates unchanged
constexpr u64 L2 = lw::make(gates::defaults, 0, 0, 0, 0);
static_assert(effective(L2) == gates::defaults);
// both slots running -> both targets cleared
constexpr u64 L2b = lw::with_target1(
    lw::with_target0(lw::make(gates::defaults, 0, 1, 2, 0),
                     static_cast<u8>(std::countr_zero(gates::rec))),
    static_cast<u8>(std::countr_zero(gates::obs)));
static_assert(effective(L2b) == (gates::defaults & ~gates::rec & ~gates::obs));
// FINISH is never suppressed (meta_states_looper.md §11)
constexpr u64 L3 = lw::with_target0(
    lw::make(gates::defaults, 0, 5, 0, 0),
    static_cast<u8>(std::countr_zero(gates::finish)));
static_assert(effective(L3) == gates::defaults);

// ---- set_suppression_timer: build a valid TIMER instruction ----
constexpr u64 W0 = uw::make(isa::encode(isa::f_gates, gates::defaults), 0);
constexpr u64 armed = set_suppression_timer<gates::rec>(W0, 7);
static_assert(isa::valid(uw::mail_v(armed)));
static_assert(isa::d_count_v(isa::decode(uw::mail_v(armed))) == 7);
static_assert(isa::d_target_v(isa::decode(uw::mail_v(armed))) ==
              static_cast<u8>(std::countr_zero(gates::rec)));

// round-trip: arm -> apply stores count + target and suppresses IMMEDIATELY
constexpr u64 L_arm = apply(lw::make(gates::defaults, 0, 0, 0, 0),
                            uw::mail_v(armed));
static_assert(lw::t0_v(L_arm) == 7);
static_assert(lw::target0_v(L_arm) == static_cast<u8>(std::countr_zero(gates::rec)));
static_assert(lw::gates_v(L_arm) == (gates::defaults & ~gates::rec));  // same-window suppression
static_assert(effective(L_arm) == (gates::defaults & ~gates::rec));

// ---- exactly_at_co / args_at: the I-th argument, value category intact ----
static_assert(std::is_same_v<decltype(args_at<1>(1, 2.5, 'k')), double&&>);
static_assert(std::is_same_v<decltype(args_at<2>(1, 2.5, 'k')), char&&>);

// The VALUE is right too, not just the type (the whole walk is constexpr).
consteval bool args_at_values() {
  static_assert(args_at<0>(10, 20, 30) == 10);
  static_assert(args_at<1>(10, 20, 30) == 20);
  static_assert(args_at<2>(10, 20, 30) == 30);
  return true;
}
static_assert(args_at_values());

int main() { return 0; }
