#pragma once

#include "meta_states_looper.hpp"
#include "meta_stream64.hpp"
#include "meta_iostream.hpp"   // index_sequence_istream (args_at)

#include <bit>       // std::countr_zero (set_suppression_timer)
#include <utility>   // std::forward (args_at)

// ============================================================================
// meta_states: the public face of the two-word looper (meta_states_looper).
//
// One round is four monitor windows around the generator and receiver calls.
// A condition is a plain Fn:
//
//   template <u8 Window, u64 L, u64 U, class Obj>
//   static constexpr u64 value;
//
// where L is the looper word (GATES/WINDOW/T0C/T1C/POS) and U is the user word
// (MAIL/USER). The engine drives windows 0..3 through meta_core64::communicate;
// a user condition returns the next user word (an instruction).
//
// A window executor keeps the old "specialize on the window" spelling:
//
//   template <u8 Window> struct my_exec : ret_origin<Window> {};
//   template <> struct my_exec<0> {              // pre_gen_exec
//     template <u64 L, u64 U, class Obj>
//     static constexpr u64 value = ...;          // return a user word
//   };
//   using cond = make_condition<states_executor<my_exec>>;
// ============================================================================

namespace meta_states {

// ===== 0) Common aliases =====
namespace bits = meta_loop::meta_states_looper_bits;
using bits::u8;
using bits::u16;
using bits::u32;
using bits::u64;
using meta_objects::flags_operator;
using meta_objects::flags_operator_type;
using meta_objects::meta_empty_o;
using meta_objects::meta_object;
using meta_objects::meta_states_object;

// ===== 1) Gates (positive, bits 0..7 of the looper word) =====
using bits::G_CONTINUE;
using bits::G_NOT_SKIP;        // observe (renamed from NOT_SKIP)
using bits::G_CALL_GENERATOR;  // advance the generator
using bits::G_CALL_RECEIVER;   // receive into the receiver
using bits::G_FINISH;          // end this round
using bits::G_FORWARD;         // direction: Gen -> Rec
using bits::G_INITIALIZE;      // run the initializer
using bits::G_CONSUME;         // for_each_forward consumes an argument

// ===== 2) The four windows =====
using meta_states_looper_details::pre_gen_exec;   // 0
using meta_states_looper_details::post_gen_exec;  // 1
using meta_states_looper_details::pre_rec_exec;   // 2
using meta_states_looper_details::post_rec_exec;  // 3

// ===== 3) Window executors and conditions =====

// Pass-through window: want the default gates, leave USER alone.
template <u8 Window>
struct ret_origin {
  template <u64 L, u64 U, class Obj>
  static constexpr u64 value = meta_stream64::ops::yield(U);
};

// Dispatch a template executor on the window number (defined in meta_objects).
using meta_objects::states_executor;

// Wrap an executor (a window dispatcher) into a condition Fn.
template <class Executor>
struct make_condition {
  template <u8 Window, u64 L, u64 U, class Obj>
  static constexpr u64 value = Executor::template value<Window, L, U, Obj>;
};

// ===== 4) Loop entries and views =====

// Decision markers a value-plane callback may return (also usable as `stop()` /
// `yield()` / `skip()`). The engine normalizes them on top of the current word.
using meta_states_control::skip_t;
using meta_states_control::yield_t;
using meta_states_control::stop_t;
using meta_states_control::skip;
using meta_states_control::yield;
using meta_states_control::stop;

template <class Condition, class Receiver, class Generator = meta_empty_o>
using looper = meta_states_looper_details::meta_states_looper<
    Condition, Receiver, Generator>;

template <class Condition, class Receiver, class Generator = meta_empty_o,
          class... Args>
using run = meta_states_looper_details::meta_states_for<
    Condition, Receiver, Generator, Args...>;

// The stage view a callback receives (to_t / from_t / states / index()).
template <class Receiver, class Generator, class States>
using pack_stream =
    meta_states_looper_details::meta_pack_stream<Receiver, Generator, States>;

// ===== 5) user_fn helpers =====
namespace user_fn {

// Position of the round that produced the given state.
template <class States>
consteval std::size_t index_of(States = {}) {
  return static_cast<std::size_t>(meta_core64::lw::pos_v(States::L));
}

}  // namespace user_fn

// ===== 6) Suppression timers (D7: count in the looper word, target as a type) =====
//
// A suppression timer suppresses one positive gate bit for a number of rounds.
// The remaining count is the looper word's T0C / T1C bytes; the target is NOT
// stored in the word (D7: it would cost a second field, and it is only decided
// at arm time). It is carried as a type instead: a condition that arms a timer
// keeps the target as `timer_target<Bit>` and asks `effective` for the gates
// byte with a running target cleared. Arming is the TIMER instruction
// (meta_stream64::ops::timer / gates_arm); apply() stores the count into the
// first free slot (slot 0, then slot 1) and refuses when both are busy.

// Sentinel target for an empty slot: it clears nothing.
struct no_timer_target {
  static constexpr u8 bit = 0;
};

// A suppression target carried as a type (D7). Bit is one positive gate bit.
template <u8 Bit>
  requires bits::looper_controlled_bit<Bit>
struct timer_target {
  static constexpr u8 bit = Bit;
};

// Remaining count of one timer slot (0 or 1), read from the looper word.
template <u8 Slot>
constexpr u8 timer_count(u64 looper) noexcept {
  static_assert(Slot <= 1, "timer_count: slot is 0 or 1");
  if constexpr (Slot == 0) return meta_core64::lw::t0_v(looper);
  else return meta_core64::lw::t1_v(looper);
}

// Tick both slots in place (saturating decrement). This is the same tick the
// engine applies at the round tail (meta_loop64::tick); it is re-exposed so a
// condition or a test can see the post-tick counts without reaching into the
// loop layer.
constexpr u64 tick_timers(u64 looper) noexcept {
  return meta_loop64::tick(looper);
}

// Effective gates: each running slot (count != 0) suppresses its stored target
// bit. The target now lives in the word (target0/target1 bit index), so this is
// a plain function of the looper word — no type argument needed, and the engine
// applies it inside `apply` already; this helper is for conditions/tests.
// FINISH is never suppressed (meta_states_looper.md §11).
constexpr u8 effective(u64 looper) noexcept {
  const u8 g = meta_core64::lw::gates_v(looper);
  const u8 drop0 = meta_core64::select8(
      meta_core64::lw::t0_v(looper) != 0,
      static_cast<u8>((u8{1} << meta_core64::lw::target0_v(looper)) &
                      static_cast<u8>(~meta_core64::gates::finish)),
      u8{0});
  const u8 drop1 = meta_core64::select8(
      meta_core64::lw::t1_v(looper) != 0,
      static_cast<u8>((u8{1} << meta_core64::lw::target1_v(looper)) &
                      static_cast<u8>(~meta_core64::gates::finish)),
      u8{0});
  return static_cast<u8>(g & static_cast<u8>(~drop0) & static_cast<u8>(~drop1));
}

// Arm a suppression timer: returns a TIMER instruction word carrying `count`.
// The target is a type (D7); only its bit index rides the instruction, which
// apply() keeps for request identity while storing `count` into the first free
// slot. `count >= 1` is a value argument here (the new ops::timer shape), so it
// is validated by isa::valid when the instruction is decoded, not by a
// static_assert (an empty slot is a count of 0, reached by tick).
template <u8 TargetBit>
  requires bits::looper_controlled_bit<TargetBit>
constexpr u64 set_suppression_timer(u64 word, u8 count) noexcept {
  return meta_stream64::ops::timer(
      word, count, static_cast<u8>(std::countr_zero(TargetBit)));
}

// ===== 7) Observe-exactly / args_at =====

// Observe exactly one round (POS == I) and stop right after. The condition is a
// meta states object (its `flags` is the seed, here zero; `function` is the Fn).
template <u64 I>
using exactly_at_co =
    meta_objects::meta_states_object<u64{0}, meta_stream64::stream_cond64<
                                               meta_stream64::exactly_at<I>>>;

// args_at<I>(args...) -> the I-th argument, preserving value category. The
// index_sequence_istream drives exactly sizeof...(Tys) rounds; exactly_at_co<I>
// observes only round I, and for_each_forward hands that round its argument.
template <u64 I, class... Tys>
constexpr decltype(auto) args_at(Tys&&... args) {
  return run<exactly_at_co<I>, meta_empty_o,
             meta_iostream::index_sequence_istream<sizeof...(Tys)>>
      ::for_each_forward(
          [](auto, auto&& value) -> decltype(auto) {
            return std::forward<decltype(value)>(value);
          },
          std::forward<Tys>(args)...);
}

}  // namespace meta_states
