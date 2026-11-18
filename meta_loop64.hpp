#pragma once

// ============================================================================
// meta_loop64: one round of the new model, plus a bounded driver for tests.
//
// A round is four windows. Each window calls `communicate` (which returns the
// user's final request word) and then applies that one instruction to the
// looper word. The gates decide whether the generator advances / the receiver
// runs; the post_rec gates decide whether the loop continues.
//
// Layering: this is the LOOP layer. It knows words, gates, and the meta-object
// protocol - it does NOT know about streams. The condition is whatever Fn the
// state carries (a plain Fn, or a stream wrapper such as meta_stream64's
// stream_cond64); the loop never names stream_cond64.
// ============================================================================

#include "meta_core64.hpp"
#include "meta_objects.hpp"

namespace meta_loop64 {

using meta_core64::u8;
using meta_core64::u16;
using meta_core64::u64;
namespace lw = meta_core64::lw;
namespace gates = meta_core64::gates;
namespace isa = meta_core64::isa;
namespace uw = meta_core64::uw;

template <u64 Lv, u64 Uv, class Rec, class Gen, class Fn>
struct state {
  static constexpr u64 L = Lv;
  static constexpr u64 U = Uv;
  using rec = Rec;
  using gen = Gen;
  using fn  = Fn;
};

// Lazy "advance only when asked", so a false gate never instantiates the
// not-taken branch. G_INITIALIZE (the init gate) takes precedence over the
// ordinary advance/receive gates: it re-initializes the object instead of
// advancing or receiving (design: meta_states_looper.md "初始化／重置指令").
template <bool Init, bool Advance, class Gen>
struct gen_action { using type = Gen; };
template <bool Advance, class Gen>
struct gen_action<true, Advance, Gen> {
  using type = meta_objects::meta_object_initialize<Gen>;
};
template <class Gen>
struct gen_action<false, true, Gen> { using type = meta_invoke<Gen>; };

template <bool Init, bool Receive, class Rec, class Gen>
struct rec_action { using type = Rec; };
template <bool Receive, class Rec, class Gen>
struct rec_action<true, Receive, Rec, Gen> {
  using type = meta_objects::meta_object_initialize<Rec>;
};
template <class Rec, class Gen>
struct rec_action<false, true, Rec, Gen> {
  using type = meta_objects::meta_object_invoke<Rec, Gen>;
};

// Saturating decrement: subtract 1 only when non-zero (mask-blended).
constexpr u8 tick8(u8 c) noexcept {
  return static_cast<u8>(c - static_cast<u8>(c != 0));
}
constexpr u64 tick(u64 flags) noexcept {
  return lw::with_t0(lw::with_t1(flags, tick8(lw::t1_v(flags))),
                     tick8(lw::t0_v(flags)));
}

// Apply the user's final request word to the looper word (one u64 in/out).
constexpr u64 settle(u64 looper, u64 user_word) noexcept {
  return meta_core64::apply(looper, uw::mail_v(user_word));
}

template <class S>
struct step {
  using Fn = typename S::fn;
  using cond = Fn;   // the condition is whatever the state carries

  // Each window starts a fresh mail slot: the condition re-issues its policy
  // every window instead of inheriting the previous window's ACK.
  static constexpr u16 mail_nop = isa::encode(isa::f_nop, 0);

  // window 0: pre_gen sees the generator
  static constexpr u64 L0 = lw::with_window(S::L, 0);
  static constexpr u64 U0 = uw::with_mail(S::U, mail_nop);
  static constexpr u64 R0 =
      meta_core64::communicate<L0, U0, typename S::gen, cond>();
  static constexpr u64 L0a = settle(L0, R0);
  static constexpr u8 g0 = lw::gates_v(L0a);
  using gen1 = typename gen_action<(g0 & gates::init) != 0,
                                   (g0 & gates::gen) != 0,
                                   typename S::gen>::type;

  // window 1: post_gen sees the advanced generator
  static constexpr u64 L1 = lw::with_window(L0a, 1);
  static constexpr u64 U1 = uw::with_mail(R0, mail_nop);
  static constexpr u64 R1 =
      meta_core64::communicate<L1, U1, gen1, cond>();
  static constexpr u64 L1a = settle(L1, R1);
  static constexpr u8 g1 = lw::gates_v(L1a);

  // Once a window ends the round, later windows must not re-open it: the halt
  // is latched in the step (a value, so `if constexpr` keeps it lazy). Window 0
  // can also end the round (an exhausted timer drops CONT in pre_gen), so the
  // latch starts from g0, not g1.
  static constexpr bool halted1 = (g0 & gates::cont) == 0 ||
                                  (g1 & gates::cont) == 0;
  static constexpr u8 mask1 =
      static_cast<u8>(~meta_core64::select8(halted1, gates::stop_mask, u8{0}));

  // window 2: pre_rec sees the receiver, and its gates decide the receive
  static constexpr u64 L2 = lw::with_window(L1a, 2);
  static constexpr u64 U2 = uw::with_mail(R1, mail_nop);
  static constexpr u64 R2 =
      meta_core64::communicate<L2, U2, typename S::rec, cond>();
  static constexpr u64 L2a =
      lw::with_gates(settle(L2, R2), static_cast<u8>(lw::gates_v(settle(L2, R2)) & mask1));
  static constexpr u8 g2 = lw::gates_v(L2a);
  // G_INITIALIZE takes precedence over receive: it re-initializes the receiver
  // (force-invokes its initialize) instead of receiving from the generator.
  using rec1 = typename rec_action<(g2 & gates::init) != 0,
                                   (g2 & gates::rec) != 0,
                                   typename S::rec, gen1>::type;

  static constexpr bool halted2 = (g2 & gates::cont) == 0;
  static constexpr u8 mask2 =
      static_cast<u8>(~meta_core64::select8(halted2, gates::stop_mask, u8{0}));

  // window 3: post_rec sees the updated receiver; its gates decide the loop
  static constexpr u64 L3 = lw::with_window(L2a, 3);
  static constexpr u64 U3 = uw::with_mail(R2, mail_nop);
  static constexpr u64 R3 =
      meta_core64::communicate<L3, U3, rec1, cond>();
  static constexpr u64 L3a = settle(L3, R3);
  static constexpr u8 g3 =
      static_cast<u8>(lw::gates_v(L3a) & mask1 & mask2);
  static constexpr u64 L3h = lw::with_gates(L3a, g3);

  static constexpr bool cont = (g3 & gates::cont) != 0;
  // Raw pre_rec OBS, before the post_rec CONT gate. The pipe uses this so a
  // dying flush round (UpstreamDied clears CONT in post_rec) is still observed.
  static constexpr bool obs = (g2 & gates::obs) != 0;
  static constexpr bool observed = obs && cont;
  static constexpr bool consume = (g2 & gates::consume) != 0;

  static constexpr u64 L4 =
      lw::with_pos(lw::with_window(tick(L3h), 0), lw::pos_v(L3h) + 1);
  using next = state<L4, R3, rec1, gen1, Fn>;
};

// Bounded driver: stop when the loop ends or the round budget is spent.
template <class S, std::size_t Rounds, int Calls, bool Cont>
struct run_impl;

template <class S, std::size_t Rounds, int Calls>
struct run_impl<S, Rounds, Calls, false> {
  using state = S;
  static constexpr int calls_total = Calls;
  static constexpr std::size_t rounds_left = Rounds;
};

// Budget exhausted: stop even though the loop wanted to continue.
template <class S, int Calls>
struct run_impl<S, 0, Calls, true> {
  using state = S;
  static constexpr int calls_total = Calls;
  static constexpr std::size_t rounds_left = 0;
};

template <class S, std::size_t Rounds, int Calls>
struct run_impl<S, Rounds, Calls, true> {
  using st = step<S>;
  static constexpr int here = Calls + static_cast<int>(st::observed);
  using tail = run_impl<typename st::next, Rounds - 1, here, st::cont>;
  using state = typename tail::state;
  static constexpr int calls_total = tail::calls_total;
  static constexpr std::size_t rounds_left = tail::rounds_left;
};

template <class S, std::size_t Rounds>
struct run {
  using st = step<S>;
  using impl = run_impl<typename st::next, Rounds - 1,
                        static_cast<int>(st::observed), st::cont>;
  using state = typename impl::state;
  static constexpr int calls = impl::calls_total;
  static constexpr std::size_t rounds_left = impl::rounds_left;
};

}  // namespace meta_loop64
