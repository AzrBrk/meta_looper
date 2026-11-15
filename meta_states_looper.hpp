#pragma once

#include "meta_objects.hpp"

// Rounds per chunk. Past this budget the loop stops on a resumable state and the
// outer driver opens the next chunk, which keeps one recursion chain from
// exhausting the template instantiation depth. Override it with
// -DMETA_STATES_LOOPER_BUDGET=... before including this header.
#ifndef META_STATES_LOOPER_BUDGET
#define META_STATES_LOOPER_BUDGET 64
#endif

// Decision markers a user function may return. The engine normalizes them into
// real flags on top of the current flags, never treating the marker itself as
// flags:
//   skip  : skip this round's receive/send, keep looping (leaves G_CONTINUE alone)
//   yield : do this round's receive/send (sets G_CALL_RECEIVER / PIPE_EMIT), keep looping
//   stop  : end the loop (clears G_CONTINUE)
// The type forms (yield_t, ...) are for TMP `using apply = ...`; the function
// forms (yield(), ...) are convenient in constexpr code and decltype(...).
namespace meta_states_control {
struct skip_t {};
struct yield_t {};
struct stop_t {};

constexpr skip_t skip() noexcept { return {}; }
constexpr yield_t yield() noexcept { return {}; }
constexpr stop_t stop() noexcept { return {}; }
}  // namespace meta_states_control

namespace meta_loop {
namespace meta_states_looper_bits {
using meta_objects::flags_operator;
using meta_objects::flags_operator_type;
using meta_objects::meta_op_bits::u8;
using meta_objects::meta_op_bits::u16;
using meta_objects::meta_op_bits::u64;
using meta_objects::meta_op_bits::u128;

//these bits are recognized by looper
inline constexpr u64 G_CONTINUE = u64{1} << 63;//master gate: continue the loop (prerequisite of all gates)
inline constexpr u64 G_NOT_SKIP = u64{1} << 62;//control if the user callable will be invoke
inline constexpr u64 G_INITIALIZE = u64{1} << 61;
inline constexpr u64 G_CONSUME = u64{1} << 60;//for_each_forward consumes this round's argument
inline constexpr u64 G_FORWARD = u64{1} << 59;//control if the loop flow forward(Gen->Rec), if off(Rec->Gen)
inline constexpr u64 G_FINISH = u64{1} << 58;//this round is accounted: bump the flags index (per-round only, consumed at settle), decide if index inc
inline constexpr u64 G_CALL_RECEIVER = u64{1} << 57;//control if the Receiver receives from the Generator
inline constexpr u64 G_CALL_GENERATOR = u64{1} << 56;//control if the Generator advances 

//for timer control, the following field are handled only by the flags_operator
//and not recognized by the looper
inline constexpr std::size_t position_shift = 0;
inline constexpr std::size_t user_shift = 16;
inline constexpr std::size_t timer0_shift = 24;
inline constexpr std::size_t timer1_shift = 40;
inline constexpr std::size_t gates_shift = 56;
inline constexpr std::size_t timer_count_shift = 6;
inline constexpr u16 timer_target_mask = 63;
inline constexpr std::size_t timer_max_count = 1023;
inline constexpr u64 position_mask = static_cast<u64>(flags_operator<0>::mask<u16, position_shift>);
inline constexpr u64 user_mask = static_cast<u64>(flags_operator<0>::mask<u8, user_shift>);
inline constexpr u64 timer0_mask = static_cast<u64>(flags_operator<0>::mask<u16, timer0_shift>);
inline constexpr u64 timer1_mask = static_cast<u64>(flags_operator<0>::mask<u16, timer1_shift>);
inline constexpr u64 gates_mask = static_cast<u64>(flags_operator<0>::mask<u8, gates_shift>);

// The set of bits the looper itself reacts to. By construction it is exactly
// the gates byte (bits 56..63); everything below is a position/user/timer
// field that only the flags_operator understands.
inline constexpr u64 looper_controlled_mask =
    G_CONTINUE | G_NOT_SKIP | G_INITIALIZE | G_CONSUME | G_FORWARD | G_FINISH |
    G_CALL_RECEIVER | G_CALL_GENERATOR;

// A timer may target exactly one looper-controlled bit; G_FINISH is not suppressed.
// Single bit is required because the target is stored as a bit *index*:
// (timer >> timer_count_shift) = count, (timer & timer_target_mask) = index.
// That 6-bit field covers indices 0..63, so even G_CONTINUE (bit 63) fits.
template<u64 Target>
concept looper_controlled_bit =
    std::has_single_bit(Target) &&
    (Target & looper_controlled_mask) == Target;

namespace details {
template<std::size_t Slot> requires (Slot < 2)
inline constexpr std::size_t timer_shift = Slot == 0 ? timer0_shift : timer1_shift;

constexpr u64 suppression(u16 timer) noexcept {
  // An empty slot (count == 0) yields 0; otherwise 1 << target index.
  // (0 - (count != 0)) is an all-ones / all-zeros mask, so a single AND does the
  // job with no branch.
  return (u64{1} << (timer & timer_target_mask)) &
         (0 - u64{(timer >> timer_count_shift) != 0});
}

constexpr u16 tick(u16 timer) noexcept {
  // The whole field is cleared when the count drops to 1 or less:
  //   - a count of 1 becomes 0, the slot is free, and the target index must not
  //     linger (a leftover value cannot tell "empty" from "used");
  //   - a field that already has count 0 is cleared too, so timer_value == 0
  //     really means "empty slot".
  // Decrement unconditionally, then mask the result: with count == 0 the
  // subtraction borrows, but the mask wipes it out, so there is no branch and no
  // side effect.
  const u64 keep = 0 - u64{(timer >> timer_count_shift) > 1};
  return static_cast<u16>((u64{timer} - (u64{1} << timer_count_shift)) & keep);
}

constexpr u128 effective_value(u128 flags) noexcept {
  // The two 16-bit timer slots are adjacent in low (bits 24..55): one shift
  // yields both halves, so the second shift on the raw value disappears.
  const std::uint32_t pair = static_cast<std::uint32_t>(flags.low >> timer0_shift);
  // FINISH must remain visible even when a timer targets it.
  return flags & ~u128{(suppression(static_cast<u16>(pair)) |
                       suppression(static_cast<u16>(pair >> 16))) & ~G_FINISH};
}

// All eight gate bits sit in the top byte of low: one shift extracts the whole
// byte, and every gate test then runs on an 8-bit integer instead of masking the
// 128-bit value repeatedly.
constexpr u8 gates_of(u128 flags) noexcept {
  return static_cast<u8>(flags.low >> gates_shift);
}

// Gate bits relative to bit 0 of the extracted gates byte.
inline constexpr u8 gate_continue = static_cast<u8>(G_CONTINUE >> gates_shift);
inline constexpr u8 gate_not_skip = static_cast<u8>(G_NOT_SKIP >> gates_shift);
inline constexpr u8 gate_initialize = static_cast<u8>(G_INITIALIZE >> gates_shift);
inline constexpr u8 gate_consume = static_cast<u8>(G_CONSUME >> gates_shift);
inline constexpr u8 gate_forward  = static_cast<u8>(G_FORWARD >> gates_shift);
inline constexpr u8 gate_finish   = static_cast<u8>(G_FINISH >> gates_shift);
inline constexpr u8 gate_receive  = static_cast<u8>(G_CALL_RECEIVER >> gates_shift);
inline constexpr u8 gate_advance  = static_cast<u8>(G_CALL_GENERATOR >> gates_shift);

// One AND-and-compare: all set_bits set and all clear_bits clear (bits are in
// the gates byte).
constexpr bool gates_have(u8 gates, u8 set_bits, u8 clear_bits = 0) noexcept {
  return (gates & (set_bits | clear_bits)) == set_bits;
}

constexpr u64 tick_low(u64 low) noexcept {
  // Both 16-bit slots are adjacent: extract 32 bits once, tick each half, write
  // the pair back in one go.
  constexpr u64 slots = timer0_mask | timer1_mask;
    const std::uint32_t pair = static_cast<std::uint32_t>(low >> timer0_shift);
    return (low & ~slots) |
      (u64{tick(static_cast<u16>(pair))} << timer0_shift) |
      (u64{tick(static_cast<u16>(pair >> 16))} << timer1_shift);
}

constexpr u128 tick_value(u128 flags) noexcept {
    flags.low = tick_low(flags.low);
  return flags;
}

constexpr u128 count_round(u128 flags) noexcept {
  static_assert(position_shift == 0);
  flags.low = (flags.low & ~position_mask) |
      ((flags.low + 1) & position_mask);
  return tick_value(flags);
}

constexpr u128 arm_timer_value(u128 flags, std::size_t shift,
                               u16 timer, bool overwrite) noexcept {
  // Write only when overwriting is allowed or the slot is empty (count == 0).
  // The write enable is an all-ones / all-zeros mask, so the read-modify-write is
  // a single branch-free chain.
  const u64 write =
      (0 - u64{overwrite}) |
      (0 - u64{((static_cast<u16>(flags.low >> shift)) >> timer_count_shift) == 0});
  const u64 m = (u64{0xFFFF} << shift) & write;
  flags.low = (flags.low & ~m) | ((u64{timer} << shift) & m);
  return flags;
}
}  // namespace details

template<flags_operator_type Flags, std::size_t Slot> requires (Slot < 2)
inline constexpr u16 timer_value =
    Flags::template get<u16, details::timer_shift<Slot>>;

template<flags_operator_type Flags, std::size_t Slot> requires (Slot < 2)
inline constexpr std::size_t timer_count = timer_value<Flags, Slot> >> timer_count_shift;

template<flags_operator_type Flags, std::size_t Slot> requires (Slot < 2)
inline constexpr u64 timer_target =
    u64{1} << (timer_value<Flags, Slot> & timer_target_mask);

// Timers suppress looper-controlled bits except G_FINISH; requested flags stay intact.
template<flags_operator_type Flags>
using effective = flags_operator<details::effective_value(Flags::value)>;

// Arms a table: a slot that is still counting (count != 0) is left alone and
// Flags is returned unchanged; use override_suppression_timer to force it.
// Count must be >= 1 - "counting" is the only valid state, and writing count = 0
// would leave a meaningless residue (use clear_timer to empty a slot).
template<flags_operator_type Flags, std::size_t Slot, u64 Target,
         std::size_t Count>
  requires (Slot < 2 && Count > 0 && Count <= timer_max_count &&
            looper_controlled_bit<Target>)
using set_suppression_timer = flags_operator<details::arm_timer_value(
    Flags::value, details::timer_shift<Slot>,
    static_cast<u16>((Count << timer_count_shift) | std::countr_zero(Target)), false)>;

// Force-overwrites the timer running in the same slot, ignoring that it is still
// counting. Use it to change the target bit, change the count, or restart a
// running table from scratch.
template<flags_operator_type Flags, std::size_t Slot, u64 Target,
         std::size_t Count>
  requires (Slot < 2 && Count > 0 && Count <= timer_max_count &&
            looper_controlled_bit<Target>)
using override_suppression_timer = flags_operator<details::arm_timer_value(
    Flags::value, details::timer_shift<Slot>,
    static_cast<u16>((Count << timer_count_shift) | std::countr_zero(Target)), true)>;

template<flags_operator_type Flags, std::size_t Slot> requires (Slot < 2)
using clear_timer = typename Flags::template deact<u16, details::timer_shift<Slot>>;

template<flags_operator_type Flags>
using tick_timers = flags_operator<details::tick_value(Flags::value)>;
}  // namespace meta_states_looper_bits

inline constexpr meta_objects::u64 meta_states_looper_default =
    meta_states_looper_bits::G_CONTINUE | meta_states_looper_bits::G_NOT_SKIP |
    meta_states_looper_bits::G_CONSUME | meta_states_looper_bits::G_FORWARD |
    meta_states_looper_bits::G_CALL_RECEIVER | meta_states_looper_bits::G_CALL_GENERATOR |
    (meta_objects::u64{0b1000} << meta_states_looper_bits::user_shift);

using meta_states_looper_flags = flags_operator<meta_states_looper_default>;
}  // namespace meta_loop
#include<typeinfo>
namespace meta_states_looper_details{
    //for convinience of developing
    //let's later delete the directly use of namespace
    using namespace meta_loop;
    using namespace meta_objects;
    using namespace exp_utilities;
    using namespace literal_types;
    using namespace meta_quote;
    using namespace meta_states_looper_bits;
    using namespace meta_invoke_protocols;


    template<
        meta_object_t Receiver, 
        meta_object_t Generator,
        flags_operator_type Control,
        class Driver = void>
    struct meta_pack_stream{
        using from   = Generator;
        using to     = Receiver;
        using to_t   = get_type<to>;
        using from_t = get_type<from>;
        using cache  = meta_produce_t<Generator>;
        using states = Control;
        using driver = Driver;

        consteval std::size_t index(){
            // position is bumped only at the round tail (window (4) / G_FINISH),
            // and this view is built after the tail, so minus one is "this
            // round's index" - exactly the position the four windows saw.
            return states::template get<u16, position_shift> - 1;
        }
        consteval const std::type_info& target_type(){
            return typeid(meta_produce_t<to>);
        }
        consteval auto value(){
            return value_of<meta_produce_t<to>>;
        }
        consteval std::size_t left(){
            return exp_size<from>;
        }
        constexpr to_t object() const{
            static_assert(requires{to_t{};}, "can not construct object!");
            return to_t{};
        }
        consteval u128 flags(){
            return Control::value;
        }
    
    };

    template <class States, class Receiver, class Generator, class Driver = void>
    using meta_states_feed = meta_pack_stream<Receiver, Generator, States, Driver>;

    // Round accounting already happens in the looper control (user_flags_control)
    // at the start of a round, so this only consumes the one-shot G_FINISH marker
    // and keeps the next round clean; a round that ended early would otherwise
    // pollute its window advancement.
    template <class Flags>
    struct meta_states_settle_impl {
        using type = typename Flags::template opDeact<G_FINISH>;
    };

    template <class Flags>
    using meta_states_settle = typename meta_states_settle_impl<Flags>::type;

    // One bit operation expresses "all SetMask bits set, all ClearMask bits
    // clear": (flags & (SetMask | ClearMask)) == SetMask. Negative bits such as
    // G_FINISH (set means suppress) therefore share one comparison with positive
    // gates instead of expanding into a && b && !c. With ClearMask == 0 it
    // degenerates to a plain has<SetMask>.
    template <u64 SetMask, u64 ClearMask = 0>
    constexpr bool gated_bits(u128 flags) noexcept {
        // Gate bits live in the gates byte: extract the byte, then one 8-bit
        // AND-and-compare instead of two 128-bit ANDs.
        return meta_states_looper_bits::details::gates_have(
            meta_states_looper_bits::details::gates_of(flags),
            static_cast<u8>(SetMask >> gates_shift),
            static_cast<u8>(ClearMask >> gates_shift));
    }

    // Keep only the user-writable bits; scheduler-owned bits come from the
    // pre-call snapshot.
    constexpr u128 restore_owned_value(u128 mask, u128 before,
                                       u128 after) noexcept {
        return (after & ~mask) | (before & mask);
    }

    template<u128 Mask, class Before, class After>
    using restore_owned_flags = flags_operator<
        restore_owned_value(Mask, Before::value, After::value)>;

    // Decision marker -> flags relative to the current flags.
    // SkipMask: bits cleared by skip; YieldMask: bits set by yield.
    // Any other return value passes through unchanged, so the old flags-style
    // functions keep working.
    template<class Raw, class Flags, u128 SkipMask, u128 YieldMask>
    struct normalize_decision_impl {
        static consteval auto result_type() {
            if constexpr (std::is_same_v<Raw, meta_states_control::skip_t> ||
                          std::is_same_v<Raw, meta_states_control::yield_t> ||
                          std::is_same_v<Raw, meta_states_control::stop_t>) {
                constexpr u128 clear =
                    std::is_same_v<Raw, meta_states_control::skip_t> ? SkipMask :
                    std::is_same_v<Raw, meta_states_control::stop_t> ?
                        u128{G_CONTINUE} : u128{0};
                constexpr u128 set =
                    std::is_same_v<Raw, meta_states_control::yield_t> ?
                        YieldMask : u128{0};
                return std::type_identity<flags_operator<
                    (Flags::value & ~clear) | set>>{};
            } else {
                return std::type_identity<Raw>{};
            }
        }
        using type = typename decltype(result_type())::type;
    };
    template<class Raw, class Flags, u128 SkipMask, u128 YieldMask>
    using normalize_decision = typename normalize_decision_impl<
        Raw, Flags, SkipMask, YieldMask>::type;

    template<class Raw>
    constexpr u128 decision_value(u128 flags, u128 skip_mask,
                                   u128 yield_mask) noexcept {
        if constexpr (std::is_same_v<Raw, meta_states_control::skip_t>) {
            return flags & ~skip_mask;
        } else if constexpr (std::is_same_v<Raw, meta_states_control::yield_t>) {
            return flags | yield_mask;
        } else if constexpr (std::is_same_v<Raw, meta_states_control::stop_t>) {
            return flags & ~u128{G_CONTINUE};
        } else {
            return Raw::value;
        }
    }

    namespace meta_states_condition_funtion_detail{
        // Owns the user flags: stage advancement, round accounting and timer
        // ticks, so that user conditions no longer have to push the stage
        // themselves. One round is four monitor windows around the generator and
        // receiver calls:
        //   (1) pre_gen_exec   sees the Generator:   continue, advance
        //   (2) post_gen_exec  sees the advanced Generator: what was produced
        //   (3) pre_rec_exec   sees the Receiver:    receive now, callback payload
        //   (4) post_rec_exec  sees the updated Receiver: accounting, next round
        constexpr u8 pre_gen_exec   = u8(0b00010000);
        constexpr u8 post_gen_exec  = u8(0b00100000);
        constexpr u8 pre_rec_exec   = u8(0b01000000);
        constexpr u8 post_rec_exec  = u8(0b10000000);
        constexpr u8 stage_index_mask  = u8(0b11110000);
        // Offset of the stage field inside the looper's own flags (the high
        // nibble, matching the mask above).
        constexpr std::size_t stage_shift = 4;

        // The stage mirror inside the user flags: the user field (u8) stores the
        // window constant itself, so a condition dispatches with
        // get<u8, user_stage_shift>. The looper control maintains it; user
        // functions must not push a stage themselves.
        constexpr std::size_t user_stage_shift = user_shift;
        inline constexpr u128 stage_mask =
            u128{stage_index_mask} << user_stage_shift;
        inline constexpr u128 scheduler_owned_mask =
            u128{position_mask} | stage_mask;
        inline constexpr u8 receiver_init_failed_state = 0b0000'0100;
        inline constexpr u128 receiver_init_failed_mask =
            u128{receiver_init_failed_state} << user_shift;
        inline constexpr u8 initialization_pending_state = 0b0000'1000;
        inline constexpr u128 initialization_pending_mask =
            u128{initialization_pending_state} << user_shift;

        constexpr bool initialization_failed(u128 flags, bool uninitialized) noexcept {
            return uninitialized && !(flags.low & initialization_pending_mask.low);
        }

        constexpr u128 after_execution(u128 flags, bool initializing, bool received) noexcept {
            if (initializing) return (flags | initialization_pending_mask) & ~u128{G_INITIALIZE};
            if (received) return flags & ~initialization_pending_mask;
            return flags;
        }

        // Window order: pre_gen -> post_gen -> pre_rec -> post_rec -> back to the
        // start of the round. The four stage values are one-hot bits in one
        // nibble, so advancing is a left shift; the 0x80 overflow folds back to
        // 0x10 through >> 3. Advancing is shift + or + mask.
        static_assert(pre_gen_exec == 0x10 && post_gen_exec == 0x20 &&
                          pre_rec_exec == 0x40 && post_rec_exec == 0x80,
                      "stage one-hot layout changed: update next_stage");
        constexpr u8 next_stage(u8 stage) noexcept {
            return static_cast<u8>(((stage << 1) | (stage >> 3)) &
                                   stage_index_mask);
        }

        // Writes Stage into the stage mirror (high nibble of the user field).
        // The low nibble carries diagnostics and upper-layer protocol state and
        // is preserved verbatim.
        // The user byte is read once: keep the low nibble, write the stage into the high
// nibble.
        constexpr u128 with_stage_value(u128 flags, u8 stage) noexcept {
            const u64 byte = (flags.low >> user_stage_shift) & 0xFF;
            flags.low = (flags.low & ~(u64{0xFF} << user_stage_shift)) |
                (((byte & 0x0F) | stage) << user_stage_shift);
            return flags;
        }

        template<u128 Flags, u8 Stage>
        using with_stage = flags_operator<with_stage_value(Flags, Stage)>;

        constexpr u128 settle_window_value(u128 flags, u8 stage) noexcept {
            // G_FINISH is read from the gates byte: one byte extraction plus an
            // 8-bit AND, no 128-bit arithmetic.
            const bool finished =
                (meta_states_looper_bits::details::gates_of(flags) &
                 meta_states_looper_bits::details::gate_finish) ||
                stage == post_rec_exec;
            return with_stage_value(
                finished ? meta_states_looper_bits::details::count_round(flags) : flags,
                finished ? pre_gen_exec : next_stage(stage));
        }

        template<u8 Stage, class Cond_o, class Obj>
        struct invoke_condition_impl {
            using staged_flags = with_stage<Cond_o::flags::value, Stage>;
            using staged = typename Cond_o::template meta_set<staged_flags>;
            using called = meta_object_invoke<staged, Obj>;
            using type = typename called::template meta_set<
                restore_owned_flags<scheduler_owned_mask, staged_flags,
                                    typename called::flags>>;
        };

        template<u8 Stage, class Cond_o, class Obj>
        using invoke_condition =
            typename invoke_condition_impl<Stage, Cond_o, Obj>::type;

        // The looper's own flags object: it knows which window is running (the
        // stage number inside this_flags) and holds the complete user condition,
        // so it can read ::flags explicitly and decide what to write back.
        // User flags no longer push the stage or do accounting themselves:
        //   windows (1)-(3)       : advance the stage only, leave the index alone
        //   end of a round ((4))  : index + 1, tick timers, reset the stage
        //   any window with G_FINISH: account here and now, tick, reset the stage
        //     immediately (the step already happened, so it must be counted; the
        //     reset keeps the next round from starting at the wrong window)
        //
        // Accounting happens at the round tail, not the head: head accounting
        // makes window (1) see "this step's index" while (2)-(4) already see the
        // bumped value, so the same round reports two different positions and any
        // user function keyed on position misbehaves. At the tail all four
        // windows see exactly the same position, which means "this is how many
        // rounds have completed" (= this step's index); the window (4) callback
        // also still sees the old value, so condition and callback agree.
        //
        // Ticks are at the tail for the same reason: every window in a round sees
        // a stable count, and count == 1 means "suppress until this round ends"
        // (one full round). At the head, the count armed by a window would read 0
        // in the other windows of the same round, so an earlier arming would
        // suppress fewer windows.
        //
        // Nothing here changes user gates except the initialization failure of an
        // actual receive, which clears G_CONTINUE. Suppression belongs to the
        // looper's judgement (see the *_judge values of a step): a timer only
        // hides a bit from the looper while the user flags keep it intact.
        // Otherwise bits such as G_NOT_SKIP / G_CONSUME / G_FORWARD, which are
        // written once in the seed or in a single step and never restated, would
        // be gone for good once suppressed.
        struct user_flags_control{
            static constexpr u64 receiver_low(u64 low, bool failed) noexcept {
                const u64 enabled = 0 - u64{failed};
                return (low & ~(G_CONTINUE & enabled)) |
                    ((u64{receiver_init_failed_state} << user_shift) & enabled);
            }

            static constexpr u64 settle_low(u64 low, u8 window,
                                             bool receiver_failed = false) noexcept {
                low = receiver_low(low, window == post_rec_exec && receiver_failed);
                const bool finished = (low & G_FINISH) || window == post_rec_exec;
                if (finished) {
                    low = (low & ~position_mask) | ((low + 1) & position_mask);
                    low = meta_states_looper_bits::details::tick_low(low);
                }
                const u64 stage = finished ? pre_gen_exec : next_stage(window);
                constexpr u64 mask = u64{stage_index_mask} << user_stage_shift;
                return (low & ~mask) | (stage << user_stage_shift);
            }

            static constexpr u128 receiver_value(u128 flags, bool failed) noexcept {
                flags.low = receiver_low(flags.low, failed);
                return flags;
            }

            static constexpr u128 settle_value(u128 flags, u8 window,
                                               bool receiver_failed = false) noexcept {
                flags.low = settle_low(flags.low, window, receiver_failed);
                return flags;
            }

            template<class this_flags, class Condition, class Receiver = void>
            struct apply{
                using user_flags = typename Condition::flags;
                // Window number in the looper's own flags, set on the spot by enter_window.
                static constexpr u8 window =
                    [] {
                        if constexpr (meta_op_bits::is_segment_bank_v<decltype(this_flags::value)>)
                            return static_cast<u8>(this_flags::value.segments[0] >> stage_shift);
                        else return this_flags::template get<u8, stage_shift>;
                    }();
                // Stage number mirrored in the user flags; user functions dispatch on it.
                static constexpr u8 user_stage =
                    user_flags::template get<u8, user_stage_shift>;
                static constexpr bool finished =
                    user_flags::template test<G_FINISH>;

                static constexpr auto value =
                    [] {
                        if constexpr (meta_op_bits::is_segment_bank_v<decltype(this_flags::value)>) {
                            auto bank = this_flags::value;
                            bank.segments[0] = settle_low(user_flags::value.low, window,
                                initialization_failed(user_flags::value,
                                    uninitialized_meta_object_t<Receiver>));
                            bank.segments[1] = user_flags::value.high;
                            return bank;
                        } else return settle_value(user_flags::value, window,
                            initialization_failed(user_flags::value,
                                uninitialized_meta_object_t<Receiver>));
                    }();
            };
        };

        using user_flags_control_o = meta_states_object<0ull, user_flags_control>;

        template<std::size_t Count> requires (Count >= 2)
        using bank_user_flags_control_o = meta_states_object<segment_bank<Count>{}, user_flags_control>;

        template<u8 Stage, class Cond_o, class Obj>
        struct invoke_window_impl {
            static constexpr u128 staged_value =
                with_stage_value(Cond_o::flags::value, Stage);
            static consteval auto result_type() {
                if constexpr (Stage == post_rec_exec &&
                              initialization_failed(staged_value, uninitialized_meta_object_t<Obj>)) {
                    return std::type_identity<typename Cond_o::template meta_set<
                        flags_operator<user_flags_control::settle_value(
                            staged_value, Stage, true)>>>{};
                } else {
                    using staged = typename Cond_o::template meta_set<flags_operator<staged_value>>;
                    using called = meta_object_invoke<staged, Obj>;
                    constexpr u128 value = user_flags_control::settle_value(
                        restore_owned_value(scheduler_owned_mask, staged_value,
                                            called::flags::value), Stage);
                    return std::type_identity<
                        typename called::template meta_set<flags_operator<value>>>{};
                }
            }
            using type = typename decltype(result_type())::type;
        };

        template<u8 Stage, class Cond_o, class Obj>
        using invoke_window = typename invoke_window_impl<Stage, Cond_o, Obj>::type;

        // The default window relay computes flags directly; an explicit custom
        // control keeps the MSO invocation protocol.
        //   (1) write Stage into the looper's own flags so the control knows which
        //       window it is in
        //   (2) call the user condition through that control - an MSO observer
        //       receives the whole object, so control::apply gets
        //       (looper_flags, condition) and reads condition::flags on demand
        //   (3) write the flags produced by the control back into the user
        //       condition for the following windows
        // Every window starts from a fresh user_flags_control_o with only the
        // stage set on the spot. If the looper's own flags ever need to
        // accumulate across windows, thread the control object like the condition.
        template<u8 Stage, class Cond_o, class Control_o = user_flags_control_o>
        struct enter_window_impl{
            static consteval auto result_type() {
                if constexpr (std::is_same_v<Control_o, user_flags_control_o>) {
                    return std::type_identity<typename Cond_o::template meta_set<
                        flags_operator<user_flags_control::settle_value(
                            Cond_o::flags::value, Stage)>>>{};
                } else {
                    using staged_flags = typename decltype([] {
                        if constexpr (meta_op_bits::is_segment_bank_v<decltype(Control_o::value)>) {
                            return std::type_identity<typename Control_o::flags::template update_segment<
                                0, u64{0xFF} << stage_shift, u64{Stage} << stage_shift>>{};
                        } else {
                            return std::type_identity<typename Control_o::flags::template set<u8, stage_shift, Stage>>{};
                        }
                    }())::type;
                    using staged_control_o = typename Control_o::template meta_set<staged_flags>;
                    using control_result_o = meta_object_invoke<staged_control_o, Cond_o>;
                    if constexpr (meta_op_bits::is_segment_bank_v<decltype(control_result_o::value)>) {
                        using projection = meta_object<bank_u128_flags<control_result_o::value>, identity_fn>;
                        return std::type_identity<meta_transfer_object<projection, Cond_o>>{};
                    } else {
                        return std::type_identity<meta_transfer_object<control_result_o, Cond_o>>{};
                    }
                }
            }
            using type = typename decltype(result_type())::type;
        };

        template<u8 Stage, class Cond_o, class Control_o = user_flags_control_o>
        using enter_window =
            typename enter_window_impl<Stage, Cond_o, Control_o>::type;

        // A complete window step: call the user condition once with Obj (gated by
        // Gate), then run the relay above. With Gate false the condition is not
        // called and no settlement is repeated; only an executed receive keeps the
        // low-level failure check. G_FINISH trims the remaining windows one by one
        // but must not mask a receive failure.
        template<bool Gate, u8 Stage, class Cond_o, class Obj>
        struct enter_window_step_impl{
            static consteval auto result_type() {
                if constexpr (!Gate) {
                    if constexpr (Stage == post_rec_exec && uninitialized_meta_object_t<Obj>) {
                        if constexpr (initialization_failed(Cond_o::flags::value, true)) {
                            // An earlier FINISH has already accounted for this round.
                            return std::type_identity<typename Cond_o::template meta_set<
                                flags_operator<user_flags_control::receiver_value(
                                    Cond_o::flags::value, true)>>>{};
                        } else return std::type_identity<Cond_o>{};
                    } else return std::type_identity<Cond_o>{};
                } else {
                    return std::type_identity<invoke_window<Stage, Cond_o, Obj>>{};
                }
            }
            using type = typename decltype(result_type())::type;
        };
        template<bool Gate, u8 Stage, class Cond_o, class Obj>
        using enter_window_step =
            typename enter_window_step_impl<Gate, Stage, Cond_o, Obj>::type;
    }

    using meta_states_condition_funtion_detail::pre_gen_exec;
    using meta_states_condition_funtion_detail::post_gen_exec;
    using meta_states_condition_funtion_detail::pre_rec_exec;
    using meta_states_condition_funtion_detail::post_rec_exec;
    using meta_states_condition_funtion_detail::enter_window;
    using meta_states_condition_funtion_detail::enter_window_step;
    using meta_states_condition_funtion_detail::invoke_condition;
    using meta_states_condition_funtion_detail::invoke_window;
    using meta_states_condition_funtion_detail::user_flags_control_o;
    // ========================================================================
    // One step: the four-window logic of a round, extracted from the old apply and
// no longer recursive.
    // ========================================================================
    template<class Condition, class Receiver, class Generator,
             class Driver = void, class... Args>
    struct meta_states_looper_step {
        // ---- window (1) pre_gen_exec
        using pre_gen_exec_stage_o = invoke_window<pre_gen_exec, Condition, Generator>;
        static constexpr u128 pre_gen_judge =
            meta_states_looper_bits::details::effective_value(pre_gen_exec_stage_o::flags::value);
        static constexpr u8 pre_gen_gates = details::gates_of(pre_gen_judge);
        static constexpr bool _entered_ = pre_gen_gates & details::gate_continue;
        static constexpr bool _advance_ = details::gates_have(
            pre_gen_gates, details::gate_continue | details::gate_advance,
            details::gate_finish);
        static constexpr bool _initialize_gen_ = details::gates_have(
            pre_gen_gates, details::gate_continue | details::gate_initialize,
            details::gate_finish);
        static consteval auto generator_type() {
            if constexpr (_initialize_gen_) return std::type_identity<meta_object_initialize<Generator>>{};
            else return std::type_identity<meta_invoke<invoke_if<_advance_>, Generator, Args...>>{};
        }
        using generator_stage_o = typename decltype(generator_type())::type;
        using after_gen_condition = typename pre_gen_exec_stage_o::template meta_set<
            flags_operator<meta_states_condition_funtion_detail::after_execution(
                pre_gen_exec_stage_o::flags::value, _initialize_gen_, false)>>;

        // ---- window (2) post_gen_exec
        static constexpr bool _pre_gen_finished_ =
            pre_gen_gates & details::gate_finish;
        using post_gen_exec_stage_o = enter_window_step<
            !_pre_gen_finished_, post_gen_exec, after_gen_condition,
            generator_stage_o>;
        static constexpr u128 post_gen_judge =
            meta_states_looper_bits::details::effective_value(post_gen_exec_stage_o::flags::value);

        // ---- window (3) pre_rec_exec
        static constexpr u8 post_gen_gates = details::gates_of(post_gen_judge);
        static constexpr bool _post_gen_finished_ =
            post_gen_gates & details::gate_finish;
        using pre_rec_exec_stage_o = enter_window_step<
            !_post_gen_finished_, pre_rec_exec, post_gen_exec_stage_o, Receiver>;
        static constexpr u128 pre_rec_judge =
            meta_states_looper_bits::details::effective_value(pre_rec_exec_stage_o::flags::value);
        static constexpr u8 pre_rec_gates = details::gates_of(pre_rec_judge);

        // The third condition is window (2)'s G_FINISH, i.e. !_post_gen_finished_,
// reused directly.
        static constexpr bool _receive_ =
            _entered_ & !_post_gen_finished_ &
            details::gates_have(pre_rec_gates,
                                details::gate_continue | details::gate_receive);
        static constexpr bool _initialize_rec_ =
            _entered_ && !_post_gen_finished_ &&
            details::gates_have(pre_rec_gates,
                details::gate_continue | details::gate_initialize);

        static constexpr bool _forward_ = pre_rec_gates & details::gate_forward;
        static constexpr bool _pre_rec_observe_ = details::gates_have(
            pre_rec_gates, details::gate_continue | details::gate_not_skip);

        static consteval auto receiver_type() {
            if constexpr (_initialize_rec_) return std::type_identity<meta_object_initialize<Receiver>>{};
            else return std::type_identity<meta_invoke<invoke_object_if<_receive_>,
                                                       Receiver, generator_stage_o>>{};
        }
        using result_stage_o = typename decltype(receiver_type())::type;
        using after_rec_condition = typename pre_rec_exec_stage_o::template meta_set<
            flags_operator<meta_states_condition_funtion_detail::after_execution(
                pre_rec_exec_stage_o::flags::value, _initialize_rec_, _receive_)>>;

        // ---- window (4) post_rec_exec
        static constexpr bool _pre_rec_finished_ =
            pre_rec_gates & details::gate_finish;
        using post_rec_exec_stage_o = enter_window_step<
            !_pre_rec_finished_, post_rec_exec, after_rec_condition,
            result_stage_o>;
        static constexpr u128 post_rec_judge =
            meta_states_looper_bits::details::effective_value(post_rec_exec_stage_o::flags::value);
        static constexpr u8 post_rec_gates = details::gates_of(post_rec_judge);

        static constexpr bool _continue_ =
            _entered_ && details::gates_have(post_rec_gates, details::gate_continue);
        static constexpr bool _observe_ = _pre_rec_observe_ && _continue_;

        using final_condition_flags_t =
            meta_states_settle<typename post_rec_exec_stage_o::flags>;
        using final_condition_o =
            typename post_rec_exec_stage_o::template meta_set<
                final_condition_flags_t>;

        using stage_t = meta_states_feed<typename final_condition_o::flags,
                                         result_stage_o, generator_stage_o,
                                         Driver>;

        static constexpr bool _swap_ = !_forward_ && _continue_;
        using next_receiver_o =
            std::conditional_t<_swap_, generator_stage_o, result_stage_o>;
        using next_generator_o =
            std::conditional_t<_swap_, result_stage_o, generator_stage_o>;

        // Exposed step results; the driver and the chunk depend only on these.
        using condition = final_condition_o;
        using receiver = next_receiver_o;
        using generator = next_generator_o;
        using stage = stage_t;
        static constexpr bool continue_ = _continue_;
        static constexpr bool observe_ = _observe_;
        static constexpr bool consume_ = pre_rec_gates & details::gate_consume;
    };

    // ========================================================================
    // Chunked continuation: state and per-chunk result.
    // ========================================================================
    template<class Condition, class Receiver, class Generator>
    struct meta_states_looper_state {};

    // Lazy "any future callback": with Current true, Rest::observes is never
    // instantiated. Writing Current || Rest::observes directly still instantiates
    // Rest because the compiler must name the member even though || short-circuits
    // during constant evaluation.
    template<bool Current, class Rest>
    struct any_observes : std::bool_constant<[] {
        if constexpr (Current) return true;
        else if constexpr (requires { Rest::observes_fn(); })
            return Rest::observes_fn();
        else return Rest::observes;
    }()> {};

    // Same lazy pattern for "any callback within the next N rounds".
    template<bool Current, std::size_t N, class Rest>
    struct any_within : std::bool_constant<[] {
        if constexpr (Current) return true;
        else return Rest::template observes_within_fn<N>();
    }()> {};

    // A chunk runs at most budget rounds. The chunk Remaining counts down from R
    // to 0 (R + 1 rounds) and the driver starts a chunk at budget - 1, so a full
    // chunk is exactly budget rounds.
    inline constexpr std::size_t meta_states_looper_budget =
        META_STATES_LOOPER_BUDGET;
    static_assert(meta_states_looper_budget >= 1,
                  "META_STATES_LOOPER_BUDGET must be at least 1");

    // for_each tells an observed value from a control decision by the callback's
// return type.
    template<class T>
    struct is_for_each_skip
        : std::bool_constant<std::is_same_v<T, meta_states_control::skip_t>> {};

    template<class T>
    struct is_for_each_yield
        : std::bool_constant<std::is_same_v<T, meta_states_control::yield_t>> {};

    template<class T>
    struct is_for_each_stop
        : std::bool_constant<std::is_same_v<T, meta_states_control::stop_t>> {};

    template<class T>
    inline constexpr bool is_for_each_decision_v =
        is_for_each_skip<std::remove_cvref_t<T>>::value ||
        is_for_each_yield<std::remove_cvref_t<T>>::value ||
        is_for_each_stop<std::remove_cvref_t<T>>::value;

    template<class T>
    inline constexpr bool is_for_each_stop_v =
        is_for_each_stop<std::remove_cvref_t<T>>::value;

    // ========================================================================
    // Flags-driven driver / chunk (the current control plane).
    //
    // The driver is an MSO whose 128-bit flags are its registers: the low 64 hold
    // data and counters, the high 64 hold control and status. The layout is
    // defined by driver_control itself.
    // ========================================================================
    namespace driver_control {
        // Low 64: data and counters.
        inline constexpr std::size_t pc_shift = 0;            // u16
        inline constexpr std::size_t remaining_shift = 16;    // u8
        inline constexpr std::size_t chunk_index_shift = 24;  // u16
        inline constexpr std::size_t chunk_round_shift = 40;  // u8
        inline constexpr std::size_t timer0_shift = 48;       // u8
        inline constexpr std::size_t timer1_shift = 56;       // u8

        // High 64: control and status.
        inline constexpr std::size_t phase_shift = 64;   // u8
        inline constexpr std::size_t policy_shift = 80;  // u16
        inline constexpr std::size_t user_shift = 96;    // u32

        inline constexpr u128 observes_bit = u128{1} << 72;
        inline constexpr u128 has_more_bit = u128{1} << 73;
        inline constexpr u128 boundary_bit = u128{1} << 74;
        inline constexpr u128 stopped_bit = u128{1} << 75;
        inline constexpr u128 paused_bit = u128{1} << 76;
        inline constexpr u128 yielded_bit = u128{1} << 77;
        inline constexpr u128 skip_bit = u128{1} << 78;
        inline constexpr u128 error_bit = u128{1} << 79;

        inline constexpr u8 phase_run = 0;
        inline constexpr u8 phase_more = 1;
        inline constexpr u8 phase_done = 2;

        inline constexpr u8 window_round = 0;
        inline constexpr u8 window_more = 1;
        inline constexpr u8 window_decide = 3;

        template<class Step, u8 Window, class Decision = void>
        struct input {
            using step = Step;
            using decision = Decision;
            static constexpr u8 window = Window;
        };

        // ---- Byte-level SWAR ----
        // Bit 0x80 of a byte is set exactly when that byte is zero (per-byte
        // exact; a borrow never leaks into a neighbour).
        constexpr std::uint32_t zero_byte_flags(std::uint32_t w) noexcept {
            return ~(((w & 0x7F7F7F7Fu) + 0x7F7F7F7Fu) | w | 0x7F7F7F7Fu) &
                   0x80808080u;
        }
        // Two adjacent u8 counters decremented together when non-zero (zero stays
        // zero) in one SWAR operation. No borrow crosses a byte boundary because
        // every subtracted byte is at least 1.
        constexpr u16 dec_pair_if_nonzero(u16 v) noexcept {
            const std::uint32_t w = v;
            return static_cast<u16>(w - ((0x8080u & ~zero_byte_flags(w)) >> 7));
        }

        constexpr u128 round_value(u128 flags, bool observe,
                                    bool cont) noexcept {
            static_assert(pc_shift == 0);
            const u8 remaining = static_cast<u8>(flags.low >> remaining_shift);
            // timer0/timer1 are adjacent u8 blocks (bits 48..63): one SWAR ticks both
// saturating counters.
            const u16 timers = dec_pair_if_nonzero(
                static_cast<u16>(flags.low >> timer0_shift));
            constexpr u64 counters_mask =
                (u64{0xFFFF} << pc_shift) |
                (u64{0xFF} << remaining_shift) |
                (u64{0xFF} << chunk_round_shift) |
                (u64{0xFF} << timer0_shift) |
                (u64{0xFF} << timer1_shift);
            flags.low = (flags.low & ~counters_mask) |
                (u64{static_cast<u16>(flags.low + 1)} << pc_shift) |
                ((u64{remaining} - (remaining != 0)) << remaining_shift) |
                (u64{static_cast<u8>((flags.low >> chunk_round_shift) + 1)}
                 << chunk_round_shift) |
                (u64{timers} << timer0_shift);
            // Phases are blended with bit operations: done/more are 0/1 and
            // 0 - x is an all-ones / all-zeros mask. Status bits use the same
            // & (0 - bit) trick, so no *0 / *1 multiply remains.
            const u8 done = static_cast<u8>(!cont);
            const u8 more = static_cast<u8>(cont & (remaining == 0));
            const u8 phase = static_cast<u8>((phase_done & (0 - done)) |
                                             (phase_more & (0 - more)));
            flags.high = (flags.high & ~u64{0xFF}) | phase |
                (observes_bit.high & (0 - u64{observe})) |
                ((has_more_bit.high | boundary_bit.high) &
                 (0 - u64{phase == phase_more})) |
                (stopped_bit.high & (0 - u64{!cont}));
            return flags;
        }

        constexpr u128 more_value(u128 flags, u8 seed_remaining) noexcept {
            constexpr u64 reset_mask =
                (u64{0xFF} << remaining_shift) |
                (u64{0xFFFF} << chunk_index_shift) |
                (u64{0xFF} << chunk_round_shift);
            flags.low = (flags.low & ~reset_mask) |
                (u64{seed_remaining} << remaining_shift) |
                (u64{static_cast<u16>((flags.low >> chunk_index_shift) + 1)}
                 << chunk_index_shift);
            flags.high &= ~(u64{0xFF} | has_more_bit.high | boundary_bit.high);
            return flags;
        }

        constexpr u128 decide_value(u128 flags, bool skip, bool yield,
                                    bool stop) noexcept {
            const u64 phase = flags.high & u64{0xFF};
            // On stop, phase_done is inserted directly and otherwise preserved: one XOR
// blend, no multiply.
            const u64 next_phase = phase ^ ((phase ^ phase_done) & (0 - u64{stop}));
            flags.high = (flags.high & ~u64{0xFF}) | next_phase |
                (skip_bit.high & (0 - u64{skip})) |
                (yielded_bit.high & (0 - u64{yield})) |
                (stopped_bit.high & (0 - u64{stop}));
            return flags;
        }
    }  // namespace driver_control

    template<std::size_t Budget>
    struct default_driver_policy {
        static_assert(Budget >= 1, "driver budget must be at least 1");
        static_assert(Budget <= 256,
                      "driver remaining is u8; budget must be <= 256");
        static constexpr u8 seed_remaining =
            static_cast<u8>(Budget - 1);

        template<class Flags, class Input>
        struct round {
            static constexpr bool observe = Input::step::observe_;
            static constexpr bool cont = Input::step::continue_;

            static constexpr u128 value =
                driver_control::round_value(Flags::value, observe, cont);
            using type = flags_operator<value>;
        };

        template<class Flags, class Input>
        struct more {
            static constexpr u128 value =
                driver_control::more_value(Flags::value, seed_remaining);
            using type = flags_operator<value>;
        };

        template<class Flags, class Input>
        struct decide {
            using decision = typename Input::decision;
            static constexpr bool skip =
                std::is_same_v<decision, meta_states_control::skip_t>;
            static constexpr bool yield =
                std::is_same_v<decision, meta_states_control::yield_t>;
            static constexpr bool stop =
                std::is_same_v<decision, meta_states_control::stop_t>;
            static constexpr u128 value = driver_control::decide_value(
                Flags::value, skip, yield, stop);
            using type = flags_operator<value>;
        };
    };

    template<class Policy>
    struct driver_fn {
        template<class Flags, class Input>
        struct apply {
            static constexpr u128 value = [] {
                if constexpr (Input::window == driver_control::window_round) {
                    return Policy::template round<Flags, Input>::value;
                } else if constexpr (Input::window ==
                                     driver_control::window_more) {
                    return Policy::template more<Flags, Input>::value;
                } else {
                    return Policy::template decide<Flags, Input>::value;
                }
            }();
        };
    };

    template<std::size_t Budget,
             class Policy = default_driver_policy<Budget>>
    using flags_driver_seed_flags =
        flags_operator<u128{static_cast<u8>(Policy::seed_remaining)}
                       << driver_control::remaining_shift>;

    template<std::size_t Budget, class Policy = default_driver_policy<Budget>>
    using flags_driver_mso = meta_states_object<
        flags_driver_seed_flags<Budget, Policy>::value, driver_fn<Policy>>;

    template<class Driver, class State, class... Args>
    struct flags_chunk;
    template<class Driver, class State, class... Args>
    struct flags_driver;

    template<class Driver, class Condition, class Receiver, class Generator,
             class... Args>
    struct flags_chunk<
        Driver, meta_states_looper_state<Condition, Receiver, Generator>,
        Args...> {
        using step =
            meta_states_looper_step<Condition, Receiver, Generator, Driver,
                                    Args...>;
        using round_driver = meta_invoke<Driver,
            driver_control::input<step, driver_control::window_round>>;

        static consteval u8 phase_fn() {
            return round_driver::flags::template get<
                u8, driver_control::phase_shift>;
        }

        using step_next_state =
            meta_states_looper_state<typename step::condition,
                                     typename step::receiver,
                                     typename step::generator>;
        using rest = flags_chunk<round_driver, step_next_state, Args...>;

        static consteval bool has_more_fn() {
            if constexpr (phase_fn() == driver_control::phase_more) {
                return true;
            } else if constexpr (phase_fn() == driver_control::phase_run) {
                return rest::has_more_fn();
            } else {
                return false;
            }
        }

        // Keep segment-end types behind accessors: eager aliases would expand
        // the whole segment even when a probe stops at this round.
        static consteval auto next_driver_fn() {
            if constexpr (phase_fn() == driver_control::phase_run) {
                return rest::next_driver_fn();
            } else {
                using step_next_driver = meta_invoke<round_driver,
                    driver_control::input<step, driver_control::window_more>>;
                return std::type_identity<step_next_driver>{};
            }
        }

        static consteval auto next_state_fn() {
            if constexpr (phase_fn() == driver_control::phase_run) {
                return rest::next_state_fn();
            } else {
                return std::type_identity<step_next_state>{};
            }
        }

        static consteval auto feed_fn() {
            if constexpr (phase_fn() == driver_control::phase_done) {
                return std::type_identity<typename step::stage>{};
            } else if constexpr (phase_fn() == driver_control::phase_more) {
                return std::type_identity<void>{};
            } else {
                return rest::feed_fn();
            }
        }

        template<bool AcrossChunks = false>
        static consteval bool observes_fn() {
            if constexpr (step::observe_) {
                return true;
            } else if constexpr (phase_fn() != driver_control::phase_done &&
                                 phase_fn() != driver_control::phase_more) {
                return rest::template observes_fn<AcrossChunks>();
            } else if constexpr (AcrossChunks &&
                                 phase_fn() == driver_control::phase_more) {
                using next = flags_chunk<
                    typename decltype(next_driver_fn())::type,
                    step_next_state, Args...>;
                return next::template observes_fn<true>();
            } else {
                return false;
            }
        }

        template<std::size_t N, bool AcrossChunks = false>
        static consteval bool observes_within_fn() {
            if constexpr (N == 0) {
                return false;
            } else if constexpr (step::observe_) {
                return true;
            } else if constexpr (N == 1 && step::consume_) {
                return false;
            } else if constexpr (phase_fn() == driver_control::phase_run) {
                return rest::template observes_within_fn<
                    N - step::consume_, AcrossChunks>();
            } else if constexpr (AcrossChunks &&
                                 phase_fn() == driver_control::phase_more) {
                using next = flags_chunk<
                    typename decltype(next_driver_fn())::type,
                    step_next_state, Args...>;
                return next::template observes_within_fn<
                    N - step::consume_, true>();
            } else {
                return false;
            }
        }

        template<class F, class... A>
        static constexpr decltype(auto) for_each(F&& f, A&&... a) {
            if constexpr (phase_fn() != driver_control::phase_run) {
                if constexpr (step::observe_) {
                    return std::invoke(f, typename step::stage{},
                                       std::forward<A>(a)...);
                } else {
                    return;
                }
            } else {
                if constexpr (step::observe_) {
                    using return_type = std::invoke_result_t<
                        F&, typename step::stage, A...>;
                    if constexpr (is_for_each_decision_v<return_type>) {
                        if constexpr (is_for_each_stop_v<return_type>) {
                            return std::invoke(
                                f, typename step::stage{},
                                std::forward<A>(a)...);
                        } else if constexpr (rest::observes_fn()) {
                            (void)std::invoke(
                                f, typename step::stage{},
                                std::forward<A>(a)...);
                            return rest::for_each(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        } else {
                            return std::invoke(
                                f, typename step::stage{},
                                std::forward<A>(a)...);
                        }
                    } else if constexpr (rest::observes_fn()) {
                        if constexpr (std::is_void_v<return_type>) {
                            std::invoke(f, typename step::stage{},
                                        std::forward<A>(a)...);
                        } else {
                            (void)std::invoke(
                                f, typename step::stage{},
                                std::forward<A>(a)...);
                        }
                        return rest::for_each(std::forward<F>(f),
                                              std::forward<A>(a)...);
                    } else {
                        return std::invoke(f, typename step::stage{},
                                           std::forward<A>(a)...);
                    }
                } else {
                    if constexpr (rest::observes_fn()) {
                        return rest::for_each(std::forward<F>(f),
                                              std::forward<A>(a)...);
                    } else {
                        return;
                    }
                }
            }
        }

        static consteval auto forward_next_type() {
            if constexpr (phase_fn() == driver_control::phase_more)
                return std::type_identity<flags_driver<
                    typename decltype(next_driver_fn())::type,
                    step_next_state, Args...>>{};
            else return std::type_identity<rest>{};
        }

        template<std::size_t N>
        static consteval bool forward_observes() {
            return observes_within_fn<N, true>();
        }

        // Probe the next chunk directly; materializing its execution driver
        // would also request the eager final result type.
        template<std::size_t N>
        static consteval bool future_within_args() {
            if constexpr (N == 0) return false;
            else if constexpr (phase_fn() == driver_control::phase_more) {
                using next = flags_chunk<
                    typename decltype(next_driver_fn())::type,
                    step_next_state, Args...>;
                return next::template forward_observes<N>();
            } else {
                return rest::template forward_observes<N>();
            }
        }

        template<class F, class First, class... A>
        static constexpr decltype(auto) forward_tail(F&& f, First&& first, A&&... a) {
            using forward_next = typename decltype(forward_next_type())::type;
            if constexpr (step::consume_)
                return forward_next::for_each_forward(std::forward<F>(f), std::forward<A>(a)...);
            else return forward_next::for_each_forward(
                std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
        }

        template<class F, class First, class... A>
        static constexpr decltype(auto) for_each_forward(F&& f, First&& first, A&&... a) {
            constexpr bool future = [] {
                if constexpr (phase_fn() == driver_control::phase_done) return false;
                else if constexpr (step::observe_) {
                    if constexpr (is_for_each_stop_v<
                        std::invoke_result_t<F&, typename step::stage, First>>) return false;
                    else return future_within_args<
                        sizeof...(A) + !step::consume_>();
                }
                else return future_within_args<
                    sizeof...(A) + !step::consume_>();
            }();
            if constexpr (step::observe_) {
                using result = std::invoke_result_t<F&, typename step::stage, First>;
                if constexpr (is_for_each_stop_v<result> || !future) {
                    return std::invoke(f, typename step::stage{}, std::forward<First>(first));
                } else {
                    (void)std::invoke(f, typename step::stage{}, std::forward<First>(first));
                    return forward_tail(std::forward<F>(f), std::forward<First>(first),
                                        std::forward<A>(a)...);
                }
            } else if constexpr (future) {
                return forward_tail(std::forward<F>(f), std::forward<First>(first),
                                    std::forward<A>(a)...);
            }
        }
    };

    template<class Driver, class State, class... Args>
    struct flags_driver {
        using chunk = flags_chunk<Driver, State, Args...>;

        template<std::size_t N>
        static consteval bool forward_observes() {
            return chunk::template forward_observes<N>();
        }

        template<class Decision>
        using decide = meta_invoke<Driver,
            driver_control::input<typename chunk::step,
                                  driver_control::window_decide,
                                  Decision>>;

        static consteval auto result_type() {
            if constexpr (chunk::has_more_fn()) {
                return std::type_identity<typename flags_driver<
                    typename decltype(chunk::next_driver_fn())::type,
                    typename decltype(chunk::next_state_fn())::type,
                    Args...>::type>{};
            } else {
                return chunk::feed_fn();
            }
        }
        using type = typename decltype(result_type())::type;

        static consteval bool observes_fn() {
            return chunk::template observes_fn<true>();
        }

        template<class F, class... A>
        static constexpr decltype(auto) for_each(F&& f, A&&... a) {
            if constexpr (chunk::has_more_fn()) {
                using chunk_return = decltype(chunk::for_each(
                    std::forward<F>(f), std::forward<A>(a)...));
                if constexpr (is_for_each_stop_v<chunk_return>) {
                    return chunk::for_each(std::forward<F>(f),
                                           std::forward<A>(a)...);
                } else {
                    using next_chunk = flags_chunk<
                        typename decltype(chunk::next_driver_fn())::type,
                        typename decltype(chunk::next_state_fn())::type,
                        Args...>;
                    if constexpr (next_chunk::template observes_fn<true>()) {
                        using next = flags_driver<
                            typename decltype(chunk::next_driver_fn())::type,
                            typename decltype(chunk::next_state_fn())::type,
                            Args...>;
                        if constexpr (chunk::observes_fn()) {
                            (void)chunk::for_each(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        }
                        return next::for_each(std::forward<F>(f),
                                              std::forward<A>(a)...);
                    } else {
                        if constexpr (chunk::observes_fn()) {
                            return chunk::for_each(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        } else {
                            return;
                        }
                    }
                }
            } else {
                if constexpr (chunk::observes_fn()) {
                    return chunk::for_each(std::forward<F>(f),
                                           std::forward<A>(a)...);
                } else {
                    return;
                }
            }
        }

        template<class F, class First, class... A>
        static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                         A&&... a) {
            return chunk::for_each_forward(std::forward<F>(f),
                                           std::forward<First>(first),
                                           std::forward<A>(a)...);
        }
    };


    // ========================================================================
    // Public entries; the shapes are unchanged.
    // ========================================================================
    template<meta_states_object_t Condition, meta_object_t Receiver,
             meta_object_t Generator = meta_empty_o,
             class DriverPolicy =
                 default_driver_policy<meta_states_looper_budget>>
    struct meta_states_looper {
        using main_condition = Condition;

        template<class... Args>
        struct apply {
            using driver = flags_driver<
                flags_driver_mso<meta_states_looper_budget, DriverPolicy>,
                meta_states_looper_state<Condition, Receiver, Generator>,
                Args...>;
            using type = typename driver::type;

            template<class Decision>
            using decide = typename driver::template decide<Decision>;

            template<class F, class... A>
            static constexpr decltype(auto) for_each(F&& f, A&&... a) {
                return driver::for_each(std::forward<F>(f),
                                        std::forward<A>(a)...);
            }

            template<class F, class First, class... A>
            static constexpr decltype(auto) for_each_forward(
                F&& f, First&& first, A&&... a) {
                return driver::for_each_forward(
                    std::forward<F>(f), std::forward<First>(first),
                    std::forward<A>(a)...);
            }
        };

        template<class Decision, class... Args>
        using decide = typename meta_invoke<meta_states_looper, Args...>
            ::template decide<Decision>;
    };

    template <class Condition, class Receiver, class Generator, class... ARG_Tys>
    using meta_states_looper_t = typename meta_invoke<
        meta_states_looper<Condition, Receiver, Generator>,
        ARG_Tys...>::type;

    template <class Condition, class Receiver, class Generator,
              class DriverPolicy, class... ARG_Tys>
    using meta_states_looper_with_policy_t = typename meta_invoke<
        meta_states_looper<Condition, Receiver, Generator, DriverPolicy>,
        ARG_Tys...>::type;

    // The meta_for counterpart of meta_looper: the condition must be a
    // meta_states_object and the Generator defaults to meta_empty_o (the "no
    // generator" spelling).
    template <meta_states_object_t Con, meta_object_t To,
              meta_object_t From = meta_empty_o, class... Args>
    using meta_states_for =
        meta_invoke<meta_states_looper<Con, To, From>, Args...>;

    template <meta_states_object_t Con, meta_object_t To,
              meta_object_t From, class DriverPolicy, class... Args>
    using meta_states_for_with_policy =
        meta_invoke<meta_states_looper<Con, To, From, DriverPolicy>, Args...>;
}
