#pragma once

// ============================================================================
// meta_looper_step: four windows driven by UFCO, with a condition that only
// ever sees the USER word.
//
//   state<UFCO, Cond, Rec, Gen>
//   step<S>::next   -> state with the advanced UFCO / condition / objects
//
// The looper word lives inside UFCO's flags. The condition is called as
//   meta_invoke<Cond, window_tag<W>, Obj>
// and its Fn sees (UFlags, window_tag, Obj) - never the looper word. It can ask
// for state with a QUERY instruction; the reply is written back into U's
// instruction slot before the loop moves on.
// ============================================================================

#include "meta_ufco.hpp"
#include "meta_loop64.hpp"  // reuse gen_action / rec_action (object protocol)

namespace meta_looper {

using meta_isa32::u8;
using meta_isa32::u32;
using meta_isa32::u64;
namespace g = meta_isa32::gates;
namespace lw = meta_isa32::word;

template <u8 W>
struct window_tag {};

// One window: condition -> UFCO -> reply. The condition re-sees U2 next window.
template <class UFCO, class Cond, class Obj, u8 W>
struct exchange {
  using C1 = meta_invoke<Cond, window_tag<W>, Obj>;
  static constexpr u64 U1 = C1::flags::value;
  static constexpr u32 instr = static_cast<u32>(U1 >> 32);
  using F1 = meta_invoke<UFCO, meta_objects::flags_operator<U1>, Obj>;
  static constexpr u64 L1 = F1::flags::value;
  static constexpr u32 reply = meta_ufco::reply_for(instr, L1);
  static constexpr u64 U2 =
      (U1 & 0xFFFF'FFFFull) | (u64{reply} << 32);
  using cond = typename C1::template meta_set<meta_objects::flags_operator<U2>>;
  using ufco = F1;
};

template <class UFCO, class Cond, class Rec, class Gen>
struct state {
  using ufco = UFCO;
  using cond = Cond;
  using rec = Rec;
  using gen = Gen;
};

template <class S>
struct step {
  using UFCO = typename S::ufco;
  using Cond = typename S::cond;
  using Rec = typename S::rec;
  using Gen = typename S::gen;

  // window 0: pre_gen
  using e0 = exchange<UFCO, Cond, Gen, 0>;
  static constexpr u8 g0 = lw::gates_v(e0::L1);
  static constexpr bool entered = (g0 & g::cont) != 0;
  static constexpr bool fin0 = (g0 & g::finish) != 0;
  static constexpr bool enter1 = entered && !fin0;
  using gen1 = typename meta_loop64::gen_action<(g0 & g::init) != 0 && !fin0,
                                                (g0 & g::gen) != 0 && !fin0,
                                                Gen>::type;

  // window 1: post_gen
  using e1 = exchange<typename e0::ufco, typename e0::cond, gen1, 1>;
  static constexpr u8 g1 = lw::gates_v(e1::L1);
  static constexpr bool fin1 = enter1 && (g1 & g::finish) != 0;
  static constexpr bool enter2 = enter1 && !fin1;
  static constexpr bool halt1 = !enter2 || (g1 & g::cont) == 0;
  static constexpr u8 mask1 =
      static_cast<u8>(~meta_core64::select8(halt1, g::stop_mask, u8{0}));

  // window 2: pre_rec
  using e2 = exchange<typename e1::ufco, typename e1::cond, Rec, 2>;
  static constexpr u8 g2 = static_cast<u8>(lw::gates_v(e2::L1) & mask1);
  using rec1 = typename meta_loop64::rec_action<enter2 && (g2 & g::init) != 0,
                                                enter2 && (g2 & g::rec) != 0,
                                                Rec, gen1>::type;
  static constexpr bool fin2 = enter2 && (g2 & g::finish) != 0;
  static constexpr bool enter3 = enter2 && !fin2;
  static constexpr bool halt2 = !enter3 || (g2 & g::cont) == 0;
  static constexpr u8 mask2 =
      static_cast<u8>(~meta_core64::select8(halt2, g::stop_mask, u8{0}));

  // window 3: post_rec
  using e3 = exchange<typename e2::ufco, typename e2::cond, rec1, 3>;
  static constexpr u8 g3 =
      static_cast<u8>(lw::gates_v(e3::L1) & mask1 & mask2);

  static constexpr u8 final_g =
      meta_core64::select8(fin0, g0,
      meta_core64::select8(fin1, g1,
      meta_core64::select8(fin2 || !enter3, g2, g3)));

  static constexpr bool cont = entered && (final_g & g::cont) != 0;
  static constexpr bool fwd_ = !enter2 || (g2 & g::fwd) != 0;
  static constexpr bool swap_ = !fwd_ && cont;
  static constexpr bool obs = enter2 && (g2 & g::obs) != 0;
  static constexpr bool observed = obs && cont;
  static constexpr bool consume = enter2 && (g2 & g::consume) != 0;

  // One-shot control bits; accounting once per round.
  static constexpr u8 settled_gates =
      static_cast<u8>(final_g & ~(g::finish | g::fwd));
  static constexpr u64 L = lw::with_pos(
      lw::with_stage_nibble(
          lw::with_gates(lw::tick(e3::L1), settled_gates),
          lw::stage_pre_gen),
      lw::pos_v(e3::L1) + 1);

  using next_rec = select_if_t<swap_, always_fn<gen1>, always_fn<rec1>>;
  using next_gen = select_if_t<swap_, always_fn<rec1>, always_fn<gen1>>;
  using next = state<typename e3::ufco, typename e3::cond, next_rec, next_gen>;
};

}  // namespace meta_looper
