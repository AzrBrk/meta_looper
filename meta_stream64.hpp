#pragma once

// ============================================================================
// meta_stream64: the stream rules on top of meta_core64, plus the convenience
// constructors a user Fn uses to build its instruction word.
//
// A user Fn returns the user word itself (choice A):
//   template <u8 Window, u64 L, u64 U, class Obj> static constexpr u64 value;
// It may return any instruction; the stream wrapper only overrides it when a
// stream rule fires (end of stream, first-round generator delay, exhausted
// timer, receiver initialization failure).
//
// AGENTS.md 2/3 again: the wrapper never uses `?:` or if/else to pick a value.
// Each window yields one drop mask; the gates are blended with & and ~, and the
// final word is chosen with select64.
// ============================================================================

#include "meta_core64.hpp"
#include "meta_objects.hpp"

namespace meta_stream64 {

using meta_core64::u8;
using meta_core64::u16;
using meta_core64::u64;
using meta_core64::mask8;
using meta_core64::mask64;
using meta_core64::select8;
using meta_core64::select64;

namespace ops {

// Build a GATES instruction, keeping the 48 user bits.
constexpr u64 gates(u64 word, u8 g) noexcept {
  return meta_core64::uw::with_mail(
      word, meta_core64::isa::encode(meta_core64::isa::f_gates, g));
}
constexpr u64 nop(u64 word) noexcept {
  return meta_core64::uw::with_mail(
      word, meta_core64::isa::encode(meta_core64::isa::f_nop, 0));
}
constexpr u64 timer(u64 word, u8 count, u8 target) noexcept {
  return meta_core64::uw::with_mail(
      word, meta_core64::isa::encode(
                meta_core64::isa::f_timer,
                static_cast<u16>(u16{count} | (u16{target} << 8))));
}
constexpr u64 gates_arm(u64 word, u8 g, u8 count, u8 target) noexcept {
  return meta_core64::uw::with_mail(
      word, meta_core64::isa::encode(
                meta_core64::isa::f_gates_arm,
                static_cast<u16>(u16{g} | (u16{target} << 8) |
                                 (u16{count} << 11))));
}
constexpr u64 user(u64 word, u64 bits) noexcept {
  return meta_core64::uw::with_user(word, bits);
}

// Gates a word is asking for, whatever instruction it carries.
constexpr u8 gates_of(u64 word) noexcept {
  const u16 mail = meta_core64::uw::mail_v(word);
  const u8 family = static_cast<u8>(mail & 0x7);
  const u8 arg = static_cast<u8>((mail >> 3) & 0xFF);
  const bool explicit_gates = (family == meta_core64::isa::f_gates) |
                              (family == meta_core64::isa::f_gates_arm);
  return select8(explicit_gates, arg, meta_core64::gates::defaults);
}

// Adjust the requested gates: `add` is OR-ed in, `drop` is cleared.
constexpr u64 set_gates(u64 word, u8 add, u8 drop) noexcept {
  return gates(word, static_cast<u8>((gates_of(word) | add) & ~drop));
}

// Named single-bit helpers, mask-blended (no `?:`).
constexpr u64 bit_on_off(u64 word, u8 bit, bool on) noexcept {
  const u8 m = mask8(on);
  return set_gates(word, static_cast<u8>(bit & m),
                   static_cast<u8>(bit & static_cast<u8>(~m)));
}
constexpr u64 with_obs(u64 word, bool on) noexcept {
  return bit_on_off(word, meta_core64::gates::obs, on);
}
constexpr u64 with_cont(u64 word, bool on) noexcept {
  return bit_on_off(word, meta_core64::gates::cont, on);
}
constexpr u64 with_rec(u64 word, bool on) noexcept {
  return bit_on_off(word, meta_core64::gates::rec, on);
}
constexpr u64 with_gen(u64 word, bool on) noexcept {
  return bit_on_off(word, meta_core64::gates::gen, on);
}

// The three usual decisions, expressed as gates.
constexpr u64 stop(u64 word) noexcept {
  return gates(word, meta_core64::gates::obs);
}
constexpr u64 skip(u64 word) noexcept {
  return gates(word, meta_core64::gates::cont | meta_core64::gates::gen |
                         meta_core64::gates::consume);
}
constexpr u64 yield(u64 word) noexcept {
  return gates(word, meta_core64::gates::defaults);
}
}  // namespace ops

// The default user Fn: it wants the default gates and leaves USER alone.
struct unchanged {
  template <u8 Window, u64 L, u64 U, class Obj>
  static constexpr u64 value = ops::yield(U);
};

namespace details {

using namespace meta_objects;

inline constexpr u8 stop_mask =
    static_cast<u8>(meta_core64::gates::cont | meta_core64::gates::rec |
                    meta_core64::gates::gen);

// stream_states_co's own 8-bit state block (USER bits 32..39). It is computed
// once at the entry (see stream_seed below), then stream_cond64 owns it: the
// wrapper re-writes it every window, so a user Fn cannot touch it (self-owned),
// and the drop mask reads it (self-read). Bits 46/47 belong to the pipe
// (emit/flush); bits 40..44 stay free for the user Fn's own marks.
//
//   bit 32  no_generator   generator is meta_empty_o (set in pre_gen, kept live)
//   bit 33  gen_uninit     generator is an uninitialized meta object
//   bit 34  rec_uninit     receiver is an uninitialized meta object
//   bit 35  gen_no_arg     generator's initialize cannot run with no args
//   bit 36  rec_no_arg     receiver's initialize cannot run with no args
//   bits 37..39            reserved
inline constexpr std::size_t stream_state_shift = 32;
inline constexpr meta_core64::u64 stream_state_mask = meta_core64::u64{0xFF}
                                                      << stream_state_shift;
inline constexpr meta_core64::u64 no_generator_bit = meta_core64::u64{1} << 32;
inline constexpr meta_core64::u64 gen_uninit_bit   = meta_core64::u64{1} << 33;
inline constexpr meta_core64::u64 rec_uninit_bit   = meta_core64::u64{1} << 34;
inline constexpr meta_core64::u64 gen_no_arg_bit   = meta_core64::u64{1} << 35;
inline constexpr meta_core64::u64 rec_no_arg_bit   = meta_core64::u64{1} << 36;

// The entry seed: one-shot, from the generator/receiver *types*. It turns the
// type traits the drop mask used to re-evaluate per window into plain bits:
// "uninitialized" and "cannot no-arg initialize" are type facts, so they are
// computed here, once, and never again inside the wrapper.
template <class Gen, class Rec>
constexpr meta_core64::u64 stream_seed() {
  constexpr bool gen_has_init = has_initializer<typename Gen::function>;
  constexpr bool gen_can_noarg =
      meta_objects::initialize_details::no_arguments_initializable<
          typename Gen::function>;
  constexpr bool rec_has_init = has_initializer<typename Rec::function>;
  constexpr bool rec_can_noarg =
      meta_objects::initialize_details::no_arguments_initializable<
          typename Rec::function>;
  constexpr bool gen_no_arg = gen_has_init && !gen_can_noarg;
  constexpr bool rec_no_arg = rec_has_init && !rec_can_noarg;
  return select64(meta_empty_object_t<Gen>, no_generator_bit, u64{0}) |
         select64(uninitialized_meta_object_t<Gen>, gen_uninit_bit, u64{0}) |
         select64(uninitialized_meta_object_t<Rec>, rec_uninit_bit, u64{0}) |
         select64(gen_no_arg, gen_no_arg_bit, u64{0}) |
         select64(rec_no_arg, rec_no_arg_bit, u64{0});
}

// The full initial user word: a NOP mail plus the 8-bit state block computed
// once at the entry (stream_seed). The looper threads this USER field through
// every window, and stream_cond64 re-owns the block each window.
template <class Gen, class Rec>
constexpr meta_core64::u64 stream_seed_word() {
  return meta_core64::uw::make(
      meta_core64::isa::encode(meta_core64::isa::f_nop, 0),
      stream_seed<Gen, Rec>());
}

constexpr u8 drop(u8 g, u8 bits) noexcept { return static_cast<u8>(g & ~bits); }

// Per-window drop mask. The `if constexpr` chain dispatches on the object's
// *type* (that cannot be a mask), but every branch only returns a mask; the
// blending happens at the call site. L is the looper word (window 0 reads POS,
// window 3 reads window 2's latched gates byte); U is the user word (the 8-bit
// stream state block lives in its USER field).
template <u8 Window, class Obj>
constexpr u8 drop_mask(meta_core64::u64 L, meta_core64::u64 U) noexcept {
  if constexpr (Window == 0) {
    if constexpr (meta_timer_object_t<Obj>) {
      return select8(!Obj::timer::value, stop_mask, u8{0});
    } else if constexpr (meta_empty_object_t<Obj>) {
      return meta_core64::gates::gen;
    } else if constexpr (initialized_meta_object_t<Obj>) {
      // First round: hold the generator once, as the old delay state did.
      return select8(meta_core64::lw::pos_v(L) == 0, meta_core64::gates::gen,
                     u8{0});
    } else {
      return u8{0};
    }
  } else if constexpr (Window == 1) {
    if constexpr (!meta_empty_object_t<Obj>) {
      return select8(exp_utilities::eol<meta_produce_t<Obj>>, stop_mask, u8{0});
    } else {
      return u8{0};
    }
  } else if constexpr (Window == 2) {
    if constexpr (meta_timer_object_t<Obj>) {
      return select8(!Obj::timer::value, stop_mask, u8{0});
    } else {
      // Rule 1 (bits): no generator + receiver cannot no-arg initialize → stop.
      // The receiver's uninitialized / no-arg facts were computed once at the
      // entry into the stream state block, not re-derived from the type here.
      const meta_core64::u64 st = meta_core64::uw::user_v(U);
      const bool no_gen = (st & no_generator_bit) != 0;
      const bool rec_uninit = (st & rec_uninit_bit) != 0;
      const bool rec_no_arg = (st & rec_no_arg_bit) != 0;
      return select8(no_gen && rec_uninit && rec_no_arg, stop_mask, u8{0});
    }
  } else {
    // Window 3 (post_rec).
    const meta_core64::u64 st = meta_core64::uw::user_v(U);
    // Window 2's latched gates byte (this round's pre_rec decision).
    const u8 g2 = meta_core64::lw::gates_v(L);
    const bool no_gen = (st & no_generator_bit) != 0;
    const bool was_receive =
        (g2 & meta_core64::gates::rec) != 0 &&
        (g2 & meta_core64::gates::init) == 0;
    const bool gen_failed =
        (st & gen_uninit_bit) != 0 && (st & gen_no_arg_bit) != 0;
    bool stop = false;
    // Rule 2 (eol kept): no generator + receiver eol → stop.
    if constexpr (!meta_empty_object_t<Obj>) {
      stop = stop || (no_gen && exp_utilities::eol<meta_produce_t<Obj>>);
    }
    // Rule 3 (bits): a receive left the receiver uninitialized — the generator
    // itself never initialized (a meta object invoking an uninitialized one
    // stays uninitialized). G_INITIALIZE's deferred receiver is a normal
    // re-initialize, not this failure, so it is gated on `was_receive`.
    stop = stop || (was_receive && gen_failed);
    return select8(stop, stop_mask, u8{0});
  }
}

// Rewrite the user's word into a GATES instruction with the given gates.
constexpr u64 override_gates(u64 word, u8 g) noexcept {
  return ops::gates(word, g);
}

}  // namespace details

// stream_cond64<Fn>: the stream wrapper, usable as a user Fn in `communicate`.
template <class Fn = unchanged>
struct stream_cond64 {
  template <u8 Window, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    using namespace meta_core64;
    u64 word = Fn::template value<Window, L, U, Obj>;
    // Self-owned 8-bit state block: the user Fn cannot write it. We re-read the
    // block from U (the entry seed carried it in; the wrapper is the only writer
    // afterwards), so a user Fn's marks in bits 32..39 are discarded.
    const u64 owned = uw::user_v(U) & details::stream_state_mask;
    word = uw::with_user(word,
                         (uw::user_v(word) & ~details::stream_state_mask) |
                             owned);
    // pre_gen: latch "no generator" into the owned block (a pipe node's upstream
    // produce is dynamic, so this one fact is still set per-round).
    if constexpr (Window == 0) {
      if constexpr (meta_objects::meta_empty_object_t<Obj>) {
        word = uw::with_user(word,
                             uw::user_v(word) | details::no_generator_bit);
      }
    }
    const u8 base = ops::gates_of(word);
    const u8 m = details::drop_mask<Window, Obj>(L, word);
    const u8 g = static_cast<u8>(base & ~m);
    // No rule fired: keep the user's instruction verbatim so TIMER /
    // GATES_ARM survive. A fired rule rewrites it into GATES.
    return select64(m == 0, word, details::override_gates(word, g));
  }();
};

// Observe exactly one round: OBS on only when POS == I; the loop ends right
// after (CONT is cleared at POS == I + 1). This is the args_at building block.
template <u64 I>
struct exactly_at {
  template <u8 W, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    u64 w = ops::yield(U);
    if constexpr (W == 2) {
      const meta_core64::u32 pos = meta_core64::lw::pos_v(L);
      w = ops::with_obs(w, pos == I);
      if (pos == I + 1) w = ops::stop(w);   // 清 CONT/REC/GEN，结束
    }
    return w;
  }();
};

}  // namespace meta_stream64
