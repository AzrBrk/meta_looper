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

  // G_CONTINUE is the master gate; G_FINISH ends the round at the window that
  // sets it (later windows' actions are truncated, the bit is consumed at the
  // round tail). A finishing pre_gen window also suppresses its own advance.
  static constexpr bool entered = (g0 & gates::cont) != 0;
  static constexpr bool fin0 = (g0 & gates::finish) != 0;
  static constexpr bool enter1 = entered && !fin0;
  using gen1 = typename gen_action<(g0 & gates::init) != 0 && !fin0,
                                   (g0 & gates::gen) != 0 && !fin0,
                                   typename S::gen>::type;

  // window 1: post_gen sees the advanced generator
  static constexpr u64 L1 = lw::with_window(L0a, 1);
  static constexpr u64 U1 = uw::with_mail(R0, mail_nop);
  static constexpr u64 R1 =
      meta_core64::communicate<L1, U1, gen1, cond>();
  static constexpr u64 L1a = settle(L1, R1);
  static constexpr u8 g1_raw = lw::gates_v(L1a);
  static constexpr bool fin1 = enter1 && (g1_raw & gates::finish) != 0;
  static constexpr bool enter2 = enter1 && !fin1;
  // A halt (window 0/1 dropped CONT) is latched: later windows must not re-open
  // it, so their stop bits are cleared.
  static constexpr bool halt1 = !enter2 || (g1_raw & gates::cont) == 0;
  static constexpr u8 mask1 =
      static_cast<u8>(~meta_core64::select8(halt1, gates::stop_mask, u8{0}));
  static constexpr u8 g1 = static_cast<u8>(g1_raw & mask1);

  // window 2: pre_rec sees the receiver, and its gates decide the receive
  static constexpr u64 L2 = lw::with_window(L1a, 2);
  static constexpr u64 U2 = uw::with_mail(R1, mail_nop);
  static constexpr u64 R2 =
      meta_core64::communicate<L2, U2, typename S::rec, cond>();
  static constexpr u64 L2a =
      lw::with_gates(settle(L2, R2),
                     static_cast<u8>(lw::gates_v(settle(L2, R2)) & mask1));
  static constexpr u8 g2 = lw::gates_v(L2a);
  // G_INITIALIZE takes precedence over receive; a finish in an earlier window
  // suppresses the receive entirely.
  using rec1 = typename rec_action<enter2 && (g2 & gates::init) != 0,
                                   enter2 && (g2 & gates::rec) != 0,
                                   typename S::rec, gen1>::type;
  static constexpr bool fin2 = enter2 && (g2 & gates::finish) != 0;
  static constexpr bool enter3 = enter2 && !fin2;

  // window 3: post_rec sees the updated receiver; its gates decide the loop
  static constexpr u64 L3 = lw::with_window(L2a, 3);
  static constexpr u64 U3 = uw::with_mail(R2, mail_nop);
  static constexpr u64 R3 =
      meta_core64::communicate<L3, U3, rec1, cond>();
  static constexpr u64 L3a = settle(L3, R3);
  static constexpr bool halt2 = !enter3 || (g2 & gates::cont) == 0;
  static constexpr u8 mask2 =
      static_cast<u8>(~meta_core64::select8(halt2, gates::stop_mask, u8{0}));
  static constexpr u8 g3 =
      static_cast<u8>(lw::gates_v(L3a) & mask1 & mask2);

  // The window that finished the round supplies the final gates.
  static constexpr u8 final_g =
      meta_core64::select8(fin0, g0,
      meta_core64::select8(fin1, g1,
      meta_core64::select8(fin2 || !enter3, g2, g3)));

  static constexpr bool cont = entered && (final_g & gates::cont) != 0;
  // G_FORWARD is read from pre_rec (window 2), matching the old looper. A round
  // whose pre_rec window never ran stays forward.
  static constexpr bool fwd_ = !enter2 || (g2 & gates::fwd) != 0;
  static constexpr bool swap_ = !fwd_ && cont;
  // Raw pre_rec OBS, before the post_rec CONT gate. The pipe uses this so a
  // dying flush round (UpstreamDied clears CONT in post_rec) is still observed.
  static constexpr bool obs = enter2 && (g2 & gates::obs) != 0;
  static constexpr bool observed = obs && cont;
  static constexpr bool consume = enter2 && (g2 & gates::consume) != 0;

  // Accounting happens once per round, from the window that actually ended it.
  // G_FINISH and G_FORWARD are one-shot: both are consumed at the round tail.
  static constexpr u8 settled_gates =
      static_cast<u8>(final_g & ~(gates::finish | gates::fwd));
  static constexpr u64 settled_base =
      enter3 ? L3a : (enter2 ? L2a : (enter1 ? L1a : L0a));
  static constexpr u64 L3h = lw::with_gates(settled_base, settled_gates);
  static constexpr u64 L4 =
      lw::with_pos(lw::with_window(tick(L3h), 0), lw::pos_v(L3h) + 1);

  // G_FORWARD off swaps the direction for the next round: Rec -> Gen.
  using next_rec = select_if_t<swap_, always_fn<gen1>, always_fn<rec1>>;
  using next_gen = select_if_t<swap_, always_fn<rec1>, always_fn<gen1>>;
  using next = state<L4, R3, next_rec, next_gen, Fn>;
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
