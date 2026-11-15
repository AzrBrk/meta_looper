#pragma once

#include "meta_states.hpp"

namespace meta_ios_details {

inline constexpr meta_states::u8 stream_state_mask = 0b0000'1111;
inline constexpr meta_states::u8 generator_delay_state = 0b0000'0001;
inline constexpr meta_states::u8 no_generator_state = 0b0000'0010;

// Reserved area: the top byte of the high 64 bits belongs to the stream
// condition, not to the user Fn (writes there are restored away below).
//   bit 0 (timer_stream_state): the object seen by this window is a
//   meta_timer_object; the other seven bits are reserved for extensions.
// Window 3 reads the bit written by window 1 to detect a timer generator.
inline constexpr std::size_t stream_reserved_shift = 56;  // byte offset inside the high 64 bits
inline constexpr meta_states::u64 stream_reserved_mask =
    meta_states::u64{0xFF} << stream_reserved_shift;
inline constexpr meta_states::u8 timer_stream_state = 0b0000'0001;
inline constexpr meta_states::u64 timer_stream_mask =
    meta_states::u64{timer_stream_state} << stream_reserved_shift;

static_assert(
    meta_states_looper_details::meta_states_condition_funtion_detail::scheduler_owned_mask.high == 0);
static_assert(meta_states::user_shift + 4 <= 64);
inline constexpr meta_states::u64 stream_owned_mask =
    meta_states_looper_details::meta_states_condition_funtion_detail::scheduler_owned_mask.low |
    (meta_states::u64{stream_state_mask & ~meta_states::initialization_pending_state}
        << meta_states::user_shift);

// ::timer answers "may this object still advance once":
//   std::integral_constant<bool, times > 0>   count remains
//   std::false_type                           count exhausted
//   meta_break_signal (derives false_type)    break_f matched
// Both terminal forms have value == false, so !timer::value covers them.
//
// An exhausted timer must never be applied: meta_timer_object<0, ...>::apply
// evaluates times - 1, which underflows to SIZE_MAX in size_t and revives the
// countdown. The stream therefore ends in the pre windows, before the object
// is applied. Details: README section "meta_stream_control.hpp".
template<class Object>
inline constexpr bool timer_stream_ended = [] {
    if constexpr (meta_objects::meta_timer_object_t<Object>) {
        return !Object::timer::value;
    } else {
        return false;
    }
}();

// Reserved-byte value for this window: set when the object is a timer.
template<class Object>
inline constexpr meta_states::u64 stream_reserved_bits =
    meta_objects::meta_timer_object_t<Object> ? timer_stream_mask
                                              : meta_states::u64{0};

struct unchanged {
    template<class Window, class Flags, class Object>
    using apply = Flags;
};

template<class Fn,
         meta_objects::meta_op_bits::u128 SkipMask =
             meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP,
         meta_objects::meta_op_bits::u128 YieldMask =
             meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP>
struct stream_fn {
    template<class Window, class Flags, class Object>
    struct apply {
        // At most one of receiver/generator may be a meta_timer_object; ::timer
        // can only end one side. Window 1 writes the marker, window 3 checks it.
        static_assert(
            !(Window::value == meta_states::pre_rec_exec &&
              (Flags::value.high & timer_stream_mask) != 0 &&
              meta_objects::meta_timer_object_t<Object>),
            "stream_states_co: the receiver and the generator cannot both be "
            "meta_timer_object; only one side may use ::timer as the stream end flag");

        static constexpr auto controlled = [] {
            using namespace meta_states;
            using namespace meta_objects;
            constexpr auto index = user_fn::index_of<Flags>();
            constexpr bool no_generator =
                ((Flags::value.low >> user_shift) & no_generator_state) != 0;
            u128 flags = Flags::value;
            // Rebuild the reserved byte from this window's object: window 3 first
            // reads the "generator is a timer" marker of windows 1-2, then stores
            // "receiver is a timer".
            flags.high = (flags.high & ~stream_reserved_mask) |
                stream_reserved_bits<Object>;

            // An MSO observer receives the whole object; a data MSO's flags do
            // not follow the stream layout, so neither the first-round delay nor
            // the stream end is derived for it: the user condition decides.
            if constexpr (meta_states_object_t<Object> ||
                          !meta_object_t<Object>) {
                return flags;
            } else if constexpr (Window::value == pre_gen_exec) {
                if constexpr (timer_stream_ended<Object>) {
                    // Timer generator exhausted: do not advance; the stream ends
                    // once this round settles.
                    flags.low &= ~G_CONTINUE;
                    return flags;
                } else if constexpr (meta_empty_object_t<Object>) {
                    flags.low = (flags.low & ~G_CALL_GENERATOR) |
                        (u64{no_generator_state} << user_shift);
                    return flags;
                } else {
                    constexpr u64 delay = u64{generator_delay_state} << user_shift;
                    if ((flags.low & delay) != 0) {
                        flags.low = (flags.low & ~delay) | G_CALL_GENERATOR;
                        return flags;
                    }
                    if constexpr (initialized_meta_object_t<Object> && index == 0) {
                        // Restore only the advance gate this wrapper closed; never
                        // open gates of pipe middle nodes or branches.
                        if ((flags.low & G_CALL_GENERATOR) != 0) {
                            flags.low = (flags.low & ~G_CALL_GENERATOR) | delay;
                            return flags;
                        }
                    }
                }
            } else if constexpr (Window::value == post_gen_exec) {
                if constexpr (!meta_empty_object_t<Object> &&
                              exp_utilities::eol<meta_produce_t<Object>>) {
                    flags.low &= ~G_CONTINUE;
                }
            } else if constexpr (Window::value == pre_rec_exec) {
                if constexpr (timer_stream_ended<Object>) {
                    // Timer receiver exhausted: do not receive (same times - 1 underflow).
                    flags.low &= ~G_CONTINUE;
                    return flags;
                } else if constexpr (no_generator && uninitialized_meta_object_t<Object> &&
                              !initialize_details::no_arguments_initializable<typename Object::function>) {
                    flags.low = (flags.low & ~G_CONTINUE) | receiver_init_failed_mask.low;
                } else if constexpr (no_generator && initialized_meta_object_t<Object> && index == 0) {
                    return meta_loop::meta_states_looper_bits::details::arm_timer_value(
                        flags, timer0_shift,
                        (u16{1} << bits::timer_count_shift) | std::countr_zero(G_CALL_RECEIVER),
                        false);
                }
            } else if constexpr (Window::value == post_rec_exec) {
                if constexpr (no_generator && exp_utilities::eol<meta_produce_t<Object>>) {
                    flags.low &= ~G_CONTINUE;
                }
            }
            return flags;
        }();

        using controlled_flags = meta_objects::flags_operator<controlled>;
        static constexpr auto value = [] {
            if constexpr ((controlled.low & meta_states::G_CONTINUE) != 0) {
                using raw = meta_invoke<Fn, Window, controlled_flags, Object>;
                // The user Fn may return skip_t / yield_t / stop_t; normalize them
                // to flags, then restore the fields owned by the base control.
                // User gates and timers stay writable, as do the high 64 bits
                // minus the reserved byte.
                auto result = meta_states_looper_details::decision_value<raw>(
                    controlled, SkipMask, YieldMask);
                result.low = (result.low & ~stream_owned_mask) |
                    (controlled.low & stream_owned_mask);
                // The top byte of the high 64 bits is stream-owned: discard user writes.
                result.high = (result.high & ~stream_reserved_mask) |
                    (controlled.high & stream_reserved_mask);
                return result;
            } else {
                return controlled;
            }
        }();
    };
};

}

namespace meta_ios {

template<class Fn = meta_ios_details::unchanged>
using stream_states_co = meta_objects::meta_states_object<
    meta_states::condition_seed, meta_states::state_fn<meta_ios_details::stream_fn<Fn>>>;

}
