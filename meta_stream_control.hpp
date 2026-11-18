#pragma once

// ============================================================================
// meta_stream_control: the public stream-control condition.
//
// In the two-word model the stream rules live in meta_stream64::stream_cond64
// (see meta_stream64.hpp): it wraps a user Fn and fires the end-of-stream /
// first-round generator delay / exhausted timer / receiver init-failure rules
// per window. The old u128 MSO spelling `stream_states_co<Fn>` is re-pointed at
// it, so the public name survives while the condition is now a plain Fn:
//
//   template <u8 Window, u64 L, u64 U, class Obj>
//   static constexpr u64 value;   // returns a user word (an instruction)
//
// The old reserved byte (bits 120..127 of the u128) is gone: a timer is
// detected per window from the object's own type (meta_timer_object_t), not
// through a marker written in window 1 and read in window 3.
// ============================================================================

#include "meta_states.hpp"

namespace meta_ios {
template <class Fn = meta_stream64::unchanged>
using stream_states_co = meta_stream64::stream_cond64<Fn>;

// The initial entry: `run<stream_states_co<Fn>, Receiver, Generator>` but the
// 8-bit stream state block is computed ONCE here, from the generator/receiver
// types (uninitialized? no-arg initializable? no generator?), and injected as
// the looper's SeedUser. stream_states_co then re-owns that block every window
// (the user Fn cannot write it) and reads it in its drop mask.
template <class Fn, class Receiver, class Generator, class... Args>
using stream_run = typename meta_states_looper_details::meta_states_looper<
    stream_states_co<Fn>, Receiver, Generator,
    meta_states_looper_details::default_driver_policy<
        meta_states_looper_details::meta_states_looper_budget>,
    meta_stream64::details::stream_seed_word<Generator, Receiver>()>
    ::template apply<Args...>;
}  // namespace meta_ios
