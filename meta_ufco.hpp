#pragma once

// ============================================================================
// meta_ufco: the looper's own state machine, as a meta states object.
//
// It owns the 64-bit looper word (POS / STATE / STAGE / TIMER0 / TIMER1 /
// GATES) and is the only thing that decodes instructions. The condition never
// sees this word; it only sees the USER word U = [instruction 32][user 32].
//
//   UFCO::function::apply<ThisFlags, UFlags, Obj> -> new 64-bit looper word
//
// Decoding is a consteval function with plain `if constexpr`: no template
// instantiation per instruction kind. Design: design/09_ufco_isa.md
// ============================================================================

#include "meta_isa32.hpp"
#include "meta_objects.hpp"

namespace meta_ufco {

using meta_isa32::u8;
using meta_isa32::u16;
using meta_isa32::u32;
using meta_isa32::u64;
namespace gates = meta_isa32::gates;
namespace word = meta_isa32::word;

// Arm one timer slot: write count+target, then suppress the target gate in the
// SAME window (the `call`-style implicit micro-op).
constexpr u64 arm(u64 st, u8 slot, u8 target, u8 count) noexcept {
  const u16 ns = word::ts_make(count, target);
  const u64 with = slot == 0 ? word::with_timer0(st, ns)
                             : word::with_timer1(st, ns);
  return word::with_gates(
      with, static_cast<u8>(word::gates_v(with) &
                            static_cast<u8>(~static_cast<u8>(u8{1} << target))));
}

// The looper state machine.
struct control {
  template <class ThisFlags, class UFlags, class Obj = void>
  struct apply {
    static constexpr u64 st = ThisFlags::value;
    static constexpr u32 instr = static_cast<u32>(UFlags::value >> 32);
    static constexpr u8 fam = meta_isa32::family_of(instr);

    // Pulse semantics: the gates byte is rebuilt from this instruction.
    // Families that carry gates use them; the others fall back to defaults.
    static constexpr u8 g =
        (fam == meta_isa32::f_gates || fam == meta_isa32::f_gates_arm ||
         fam == meta_isa32::f_gates_arm2)
            ? meta_isa32::gates_of(instr)
            : gates::defaults;
    static constexpr u64 base = word::with_gates(st, g);

    static constexpr u64 value = [] {
      if constexpr (fam == meta_isa32::f_arm) {
        return arm(base, meta_isa32::arm_slot_of(instr),
                   meta_isa32::arm_target_of(instr),
                   meta_isa32::arm_count_of(instr));
      } else if constexpr (fam == meta_isa32::f_arm2) {
        return arm(arm(base, 0, meta_isa32::t0_target_of(instr),
                       meta_isa32::t0_count_of(instr)),
                   1, meta_isa32::t1_target_of(instr),
                   meta_isa32::t1_count_of(instr));
      } else if constexpr (fam == meta_isa32::f_gates_arm) {
        return arm(base, meta_isa32::arm_slot_of(instr),
                   meta_isa32::arm_target_of(instr),
                   meta_isa32::arm_count_of(instr));
      } else if constexpr (fam == meta_isa32::f_gates_arm2) {
        return arm(arm(base, 0, meta_isa32::t0_target_of(instr),
                       meta_isa32::t0_count_of(instr)),
                   1, meta_isa32::t1_target_of(instr),
                   meta_isa32::t1_count_of(instr));
      } else {
        return base;  // NOP / GATES / QUERY / ACK: gates only
      }
    }();
  };
};

// Initial looper word: defaults gates, stage pre_gen, POS 0, no timers.
inline constexpr u64 seed =
    word::make(gates::defaults, word::stage_pre_gen, 0, 0, 0);

using control_o = meta_objects::meta_states_object<seed, control>;

// Reply to the condition. Query families pack the requested state fields;
// action families return a normal ACK.
constexpr u32 reply_for(u32 instr, u64 st) noexcept {
  const u8 fam = meta_isa32::family_of(instr);
  if (fam == meta_isa32::f_query) {
    const u8 bm = meta_isa32::query_bitmap_of(instr);
    u32 p = 0;
    unsigned sh = 0;
    if (bm & meta_isa32::q_pos) {
      p |= static_cast<u32>(word::pos_v(st)) << sh;
      sh += 16;
    }
    if (bm & meta_isa32::q_gates) {
      p |= static_cast<u32>(word::gates_v(st)) << sh;
      sh += 8;
    }
    if (bm & meta_isa32::q_timer0) {
      p |= static_cast<u32>(word::timer0_v(st)) << sh;
      sh += 16;
    }
    if (bm & meta_isa32::q_timer1) {
      p |= static_cast<u32>(word::timer1_v(st)) << sh;
      sh += 16;
    }
    if (bm & meta_isa32::q_stage) {
      p |= static_cast<u32>(word::stage_v(st)) << sh;
      sh += 8;
    }
    return meta_isa32::encode(meta_isa32::f_ack, p);
  }
  // ACK: mirror the requested gates, reason ok, timer slots running.
  const u8 mirror = (fam == meta_isa32::f_gates ||
                     fam == meta_isa32::f_gates_arm ||
                     fam == meta_isa32::f_gates_arm2)
                        ? meta_isa32::gates_of(instr)
                        : gates::defaults;
  const bool t0 = word::ts_count(word::timer0_v(st)) != 0;
  const bool t1 = word::ts_count(word::timer1_v(st)) != 0;
  return meta_isa32::make_ack(mirror, meta_isa32::reason_ok, t0, t1);
}

}  // namespace meta_ufco
