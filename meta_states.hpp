#pragma once

// ============================================================================
// meta_states: the public face of meta_states_looper.
//
// This layer only arranges what should be exported and ships the built-in
// components; every mechanism lives in meta_states_looper.hpp (implementation
// namespace) and users never need to touch it.
//
// One round is four monitor windows around the generator and receiver calls:
//
//   (1) pre_gen_exec   sees the Generator   -> continue (G_CONTINUE), advance (G_CALL_GENERATOR)
//   (2) post_gen_exec  condition sees the advanced Generator -> what the produced value means
//   (3) pre_rec_exec   sees the Receiver    -> receive now (G_CALL_RECEIVER), callback payload
//   (4) post_rec_exec  sees the Receiver    -> accounting (G_FINISH), how the next round runs
//
// A user writes only a window function (a template dispatched on the window
// number) plus a Receiver (and optionally a Generator). Stage advancement,
// index accounting, timer ticks, suppression, and "G_FINISH trims the remaining
// windows of this round" are all done by the looper's own flags control
// (user_flags_control).
//
// ---------------------------------------------------------------------------
// Minimal shape:
//
//   template <u8 stage>                       // the window number is the specialization argument
//   struct my_exec : meta_states::ret_origin<stage> {};   // unwritten windows pass through
//
//   template <>                               // window (1): sees the generator
//   struct my_exec<meta_states::pre_gen_exec> {
//     template <class flags, class gen>
//     struct apply { static constexpr auto value = []{ ... }(); };
//   };
//
//   using cond = meta_states::make_condition<
//       meta_states::states_executor<my_exec>>;
//   meta_states::run<cond, meta_states::recv_t, gen_t>::for_each([](auto s){ ... });
//
// Alternatively write one template and pick windows through partial
// specializations of apply (see the exactly_at pattern). Specializing the same
// window twice is a redefinition error, and a misspelled window constant is an
// ordinary compile error.
// ---------------------------------------------------------------------------

#include "meta_states_looper.hpp"

namespace meta_states {

// ===== 0) Common aliases, so users do not have to hunt for a namespace =====
namespace bits = meta_loop::meta_states_looper_bits;
using bits::u8;
using bits::u16;
using bits::u64;
using bits::u128;
using meta_objects::flags_operator;
using meta_objects::meta_empty_o;
using meta_objects::meta_object;
using meta_objects::meta_states_object;

// ===== 1) Control bits (the eight bits the looper reads, in the top byte of the low 64) =====
//
// All of them read as "set means do it"; only G_FINISH is negative (set means
// the round ends here):
//   G_CONTINUE         master gate: whether the loop continues (gate prerequisite)
//   G_NOT_SKIP         whether this step calls back into the user
//   G_CALL_GENERATOR   whether the generator advances
//   G_CALL_RECEIVER    whether the receiver receives
//   G_FINISH           end this round: trim later windows, reset the stage (one-shot)
//   G_FORWARD          direction: set = Gen->Rec; cleared = the next stage swaps the two roles
//   G_INITIALIZE       run the initializer instead of a normal advance/receive, then consume it
//   G_CONSUME          whether for_each_forward consumes an argument this round (on by default)
using bits::G_CONTINUE;
using bits::G_NOT_SKIP;
using bits::G_CALL_GENERATOR;
using bits::G_CALL_RECEIVER;
using bits::G_FINISH;
using bits::G_FORWARD;
using bits::G_INITIALIZE;
using bits::G_CONSUME;

// Default gates: everything open (continue / callback / advance / receive /
// keep direction / consume) with G_FINISH clear.
inline constexpr u64 default_gates = meta_loop::meta_states_looper_default & bits::gates_mask;

// ===== 2) The four stages (windows) and where the stage mirror lives =====
using meta_states_looper_details::meta_states_condition_funtion_detail::pre_gen_exec;
using meta_states_looper_details::meta_states_condition_funtion_detail::post_gen_exec;
using meta_states_looper_details::meta_states_condition_funtion_detail::pre_rec_exec;
using meta_states_looper_details::meta_states_condition_funtion_detail::post_rec_exec;
// The stage mirror occupies the high nibble of the user field (stage_index_mask
// is its mask). In the low nibble, 0b0100 is reserved for the receiver
// initialization failure diagnostic and the remaining bits are available to
// upper-layer protocols.
using meta_states_looper_details::meta_states_condition_funtion_detail::stage_index_mask;
using meta_states_looper_details::meta_states_condition_funtion_detail::user_stage_shift;
using meta_states_looper_details::meta_states_condition_funtion_detail::receiver_init_failed_state;
using meta_states_looper_details::meta_states_condition_funtion_detail::receiver_init_failed_mask;
using meta_states_looper_details::meta_states_condition_funtion_detail::initialization_pending_state;
using meta_states_looper_details::meta_states_condition_funtion_detail::initialization_pending_mask;

// ===== 3) Flags field layout, for advanced users who build flags by hand =====
using bits::position_shift;   // u16: completed rounds (bumped at the round tail, identical in all four windows)
using bits::user_shift;       // u8 : stage mirror in the high nibble, failure diagnostics in the low nibble
using bits::timer0_shift;     // u16: timer slot 0
using bits::timer1_shift;     // u16: timer slot 1
using bits::gates_shift;      // u8 : the eight control bits
using bits::position_mask;
using bits::user_mask;
using bits::timer0_mask;
using bits::timer1_mask;
using bits::gates_mask;
// The set of bits the looper reacts to (the gates byte) plus the "a timer
// targets exactly one looper bit" constraint.
using bits::looper_controlled_mask;
using bits::looper_controlled_bit;

// ===== 4) Timers =====
//
// The unit is one round: the looper decrements each timer at the end of a round
// (user G_FINISH or window (4)), and the whole field is cleared when the count
// reaches zero - so timer_value == 0 really means "this slot is empty".
// Suppression: while the count is greater than zero the target bit is cleared
// on every window write-back, even if the user function sets it again.
using bits::set_suppression_timer;       // does not overwrite a running timer in the same slot
using bits::override_suppression_timer;  // force a new table
using bits::clear_timer;
using bits::tick_timers;
using bits::timer_value;     // whole field = (count << 6) | target bit index
using bits::timer_count;
using bits::timer_target;
using bits::timer_max_count;
using bits::effective;       // flags with the suppressed target bits masked out

// ===== 5) Starting flags of a condition object =====
// Default gates with the stage mirror pointing at window (1), where a round starts.
inline constexpr u64 condition_seed =
    meta_loop::meta_states_looper_default | (u64(pre_gen_exec) << user_shift);

// ===== 6) Built-in components: window functions and condition functions =====
//
// A user "window function" is just something dispatched on the window number.
// Both spellings below work and rely on ordinary C++ rules:
//
//   A) A template specialized on the window number (unwritten windows inherit
//      the pass-through behaviour of ret_origin):
//      template <u8 stage>
//      struct my_exec : ret_origin<stage> {};
//      template <>
//      struct my_exec<pre_gen_exec> {
//        template <this_ref flags, class gen>
//        struct apply { static constexpr auto value = ...; };
//      };
//      using cond = make_condition<states_executor<my_exec>>;   // picks the entry by stage
//
//   B) One template plus partial specializations of apply (only the windows you
//      care about):
//      template <std::size_t I>
//      struct exactly_at {
//        template <class stage_t, class flags, class obj>
//        struct apply { ... };                                                        // generic
//        template <class flags, class obj>
//        struct apply<std::integral_constant<u8, pre_rec_exec>, flags, obj> { ... };   // window (3) only
//      };
//      using cond = make_condition<exactly_at<2>>;
//
// The library does not check how many windows were written or whether they are
// duplicated: specializing one window twice is a C++ redefinition error, and a
// misspelled window constant is an ordinary compile error.

using meta_objects::this_ref;
template<u8 any_flags>
struct ret_origin{
    template<this_ref flags, class obj>
    using apply = flags;
};

// 6.2) Built-in condition function: reads the current window number from the
//      stage mirror the looper maintains (high nibble of the user field) and
//      passes it as a key together with the flags and the object to the
//      executor. The executor is either states_executor<Fn> (specialized on the
//      stage) or any template that provides apply. Only the high nibble is
//      read; the failure diagnostics in the low nibble stay under the
//      scheduler, and the rest is available to upper-layer protocols.
template <class Executor>
struct state_fn {
    template <class this_flags, class obj>
    struct apply {
        static constexpr u8 stage_index = static_cast<u8>(
            this_flags::template get<u8, user_shift> & stage_index_mask);
        static constexpr auto value =
            meta_invoke<Executor, std::integral_constant<u8, stage_index>,
                        this_flags, obj>::value;
    };
};

// 6.3) Wraps an executor into the condition meta object the looper consumes
//      (meta_states_object). Flags may replace the initial gates and position
//      (condition_seed by default); the scheduler writes the stage mirror
//      before every window call, so users cannot use it to jump between
//      windows.
template <class Executor, u128 Flags = condition_seed>
using make_condition = meta_states_object<Flags, state_fn<Executor>>;

// ===== 7) Loop entries and views =====

// Engine: ::apply (one step with arguments) / ::type / ::for_each /
// ::for_each_forward.
template <class Condition, class Receiver, class Generator = meta_empty_o>
using looper = meta_states_looper_details::meta_states_looper<Condition, Receiver, Generator>;

// Entry with a custom driver policy; the policy provides the round / more /
// done / decide windows.
template <class Condition, class Receiver, class Generator = meta_empty_o,
          class DriverPolicy = meta_states_looper_details::
              default_driver_policy<meta_states_looper_details::meta_states_looper_budget>>
using looper_with_policy = meta_states_looper_details::meta_states_looper<
    Condition, Receiver, Generator, DriverPolicy>;

// Final type after the loop (run<...>::type).
template <class Condition, class Receiver, class Generator = meta_empty_o,
          class... Args>
using looper_t = meta_states_looper_details::meta_states_looper_t<
    Condition, Receiver, Generator, Args...>;

// Recommended entry: run<cond, recv, gen>::for_each(f) /
// ::for_each_forward(f, args...). The Generator defaults to meta_empty_o, which
// yields an empty tag every round (see the meta_empty_o semantics).
template <class Condition, class Receiver, class Generator = meta_empty_o,
          class... Args>
using run = meta_states_looper_details::meta_states_for<
    Condition, Receiver, Generator, Args...>;

template <class Condition, class Receiver, class Generator,
          class DriverPolicy, class... Args>
using run_with_policy = meta_states_looper_details::meta_states_for_with_policy<
    Condition, Receiver, Generator, DriverPolicy, Args...>;

// Callbacks and the final state always expose a packed view: projections
// computed on demand, holding no state.
//   ::to_t / ::from_t / ::cache / ::states (a flags type)
//   .index() / .value() / .left() / .object()
template <class Receiver, class Generator, class States>
using pack_stream =
    meta_states_looper_details::meta_pack_stream<Receiver, Generator, States>;

namespace user_fn{
    using argument_stack = flags_operator<0>;
using meta_objects::flags_operator_type;
template<flags_operator_type Flags>
consteval std::size_t index_of(Flags = {}){
    return static_cast<std::size_t>(Flags::template get<u16, position_shift>);
}

// ===== Automatic slot allocation for suppression timers =====
// A slot (0 or 1) is free exactly when its count is 0 (see bits::tick: the
// whole slot is cleared at zero, so timer_value == 0 really means "empty").
// A value of 2 means both slots are counting, so nothing can be allocated.
template<flags_operator_type Flags>
inline constexpr std::size_t first_free_timer_slot =
    bits::timer_count<Flags, 0> == 0 ? 0 :
    bits::timer_count<Flags, 1> == 0 ? 1 : 2;

// Installs a suppression table: Bit is suppressed for Times rounds, with the
// slot chosen automatically from what Flags currently occupies, so users no
// longer pick a Slot.
//   - Times == 0 installs nothing (handy for conditional suppression);
//   - a slot already suppressing the same Bit returns Flags unchanged (the
//     "never overwrite a running table" semantics of set_suppression_timer), so
//     calling it every round is safe;
//   - when both slots target something else it fires a static_assert; wait for
//     one to expire or pick a slot explicitly with override_suppression_timer.
// Bit and Times are template arguments rather than function arguments because a
// function argument is not a constant expression and static_assert could not
// read it.
template<flags_operator_type Flags, u64 Bit, std::size_t Times>
consteval u128 set_suppression() {
    static_assert(std::has_single_bit(Bit) &&
                  (Bit & bits::looper_controlled_mask) == Bit,
                  "set_suppression: Bit must be a single looper-controlled bit");
    static_assert(Times <= bits::timer_max_count,
                  "set_suppression: Times must fit in the 10-bit timer count");
    constexpr bool same_target =
        (bits::timer_count<Flags, 0> != 0 &&
         bits::timer_target<Flags, 0> == Bit) ||
        (bits::timer_count<Flags, 1> != 0 &&
         bits::timer_target<Flags, 1> == Bit);
    constexpr std::size_t slot = first_free_timer_slot<Flags>;
    if constexpr (Times == 0 || same_target) {
        return Flags::value;
    } else if constexpr (slot > 1) {
        static_assert(slot < 2,
                      "set_suppression: both suppression timers are running; "
                      "wait for one to expire or use override_suppression_timer");
        return Flags::value;
    } else {
        constexpr u16 timer = static_cast<u16>(
            (Times << bits::timer_count_shift) | std::countr_zero(Bit));
        return bits::details::arm_timer_value(
            Flags::value, bits::details::timer_shift<slot>, timer, true);
    }
}
}

// Decision markers a window or condition function may return:
//   skip_t  : skip this round's receive/send, keep looping
//   yield_t : do this round's receive/send, keep looping
//   stop_t  : end the loop
// The constexpr function forms skip() / yield() / stop() work as well.
using meta_states_control::skip_t;
using meta_states_control::yield_t;
using meta_states_control::stop_t;
using meta_states_control::skip;
using meta_states_control::yield;
using meta_states_control::stop;

}  // namespace meta_states
