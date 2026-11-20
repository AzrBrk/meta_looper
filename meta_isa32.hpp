#pragma once

// ============================================================================
// meta_isa32: the 32-bit instruction set the looper's control MSO (UFCO)
// decodes, plus the 64-bit looper state word it owns.
//
// User word U (u64) = [ instruction 32 ][ user flags 32 ]; the low half belongs
// to the condition, the looper never touches it.
//
// Instruction: [31:28] FAMILY(4) + [27:0] PAYLOAD (family-dependent).
// Decoding is a consteval function with plain `if`/`switch`: it never adds a
// template instantiation per instruction kind.
//
// Design: design/09_ufco_isa.md
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace meta_isa32 {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ===== gates byte (bits 56..63), restored to the original bit order =====
// bit 7 = G_CONTINUE, bit 6 = G_NOT_SKIP, bit 5 = G_INITIALIZE,
// bit 4 = G_CONSUME, bit 3 = G_FORWARD, bit 2 = G_FINISH,
// bit 1 = G_CALL_RECEIVER, bit 0 = G_CALL_GENERATOR.
namespace gates {
inline constexpr u8 gen     = u8{1} << 0;  // G_CALL_GENERATOR: advance generator
inline constexpr u8 rec     = u8{1} << 1;  // G_CALL_RECEIVER: receiver receives
inline constexpr u8 finish  = u8{1} << 2;  // G_FINISH: end this round here
inline constexpr u8 fwd     = u8{1} << 3;  // G_FORWARD: Gen -> Rec direction
inline constexpr u8 consume = u8{1} << 4;  // G_CONSUME: for_each_forward consumes
inline constexpr u8 init    = u8{1} << 5;  // G_INITIALIZE: re-initialize instead
inline constexpr u8 obs     = u8{1} << 6;  // G_NOT_SKIP: observable this round
inline constexpr u8 cont    = u8{1} << 7;  // G_CONTINUE: master gate

inline constexpr u8 defaults  = cont | obs | gen | rec | fwd | consume;
inline constexpr u8 stop_mask = cont | rec | gen;

constexpr bool consistent(u8 g) noexcept {
  return ((g & (rec | gen)) == 0) | ((g & cont) != 0);
}
}  // namespace gates

// ===== instruction families =====
inline constexpr u8 f_nop        = 0;
inline constexpr u8 f_gates      = 1;
inline constexpr u8 f_arm        = 2;
inline constexpr u8 f_arm2       = 3;
inline constexpr u8 f_gates_arm  = 4;
inline constexpr u8 f_gates_arm2 = 5;
inline constexpr u8 f_query      = 6;
inline constexpr u8 f_ack        = 7;
inline constexpr u8 f_ext_first  = 8;

// ===== instruction word =====
inline constexpr std::size_t family_shift = 28;
inline constexpr u32 family_mask  = u32{0xF} << family_shift;
inline constexpr u32 payload_mask = u32{0x0FFF'FFFF};

constexpr u32 encode(u8 family, u32 payload) noexcept {
  return (u32{family} << family_shift) | (payload & payload_mask);
}
constexpr u8 family_of(u32 instr) noexcept {
  return static_cast<u8>((instr >> family_shift) & 0xF);
}
constexpr u32 payload_of(u32 instr) noexcept { return instr & payload_mask; }

constexpr bool is_nop(u32 i)        noexcept { return family_of(i) == f_nop; }
constexpr bool is_gates(u32 i)      noexcept { return family_of(i) == f_gates; }
constexpr bool is_arm(u32 i)        noexcept { return family_of(i) == f_arm; }
constexpr bool is_arm2(u32 i)       noexcept { return family_of(i) == f_arm2; }
constexpr bool is_gates_arm(u32 i)  noexcept { return family_of(i) == f_gates_arm; }
constexpr bool is_gates_arm2(u32 i) noexcept { return family_of(i) == f_gates_arm2; }
constexpr bool is_query(u32 i)      noexcept { return family_of(i) == f_query; }
constexpr bool is_ack(u32 i)        noexcept { return family_of(i) == f_ack; }

// ===== payload field accessors =====
// gates-bearing families: payload[7:0]
constexpr u8 gates_of(u32 i) noexcept {
  return static_cast<u8>(payload_of(i) & 0xFF);
}
// single-timer families (ARM, GATES_ARM): slot[8], target[11:9], count5[16:12]
constexpr u8 arm_slot_of(u32 i) noexcept {
  return static_cast<u8>((payload_of(i) >> 8) & 0x1);
}
constexpr u8 arm_target_of(u32 i) noexcept {
  return static_cast<u8>((payload_of(i) >> 9) & 0x7);
}
constexpr u8 arm_count_of(u32 i) noexcept {
  return static_cast<u8>((payload_of(i) >> 12) & 0x1F);
}
// dual-timer families (ARM2, GATES_ARM2): t0 [10:8]/[15:11], t1 [18:16]/[23:19]
constexpr u8 t0_target_of(u32 i) noexcept {
  return static_cast<u8>((payload_of(i) >> 8) & 0x7);
}
constexpr u8 t0_count_of(u32 i) noexcept {
  return static_cast<u8>((payload_of(i) >> 11) & 0x1F);
}
constexpr u8 t1_target_of(u32 i) noexcept {
  return static_cast<u8>((payload_of(i) >> 16) & 0x7);
}
constexpr u8 t1_count_of(u32 i) noexcept {
  return static_cast<u8>((payload_of(i) >> 19) & 0x1F);
}
// QUERY bitmap: payload[7:0]
constexpr u8 query_bitmap_of(u32 i) noexcept {
  return static_cast<u8>(payload_of(i) & 0xFF);
}
// QUERY bitmap bits
inline constexpr u8 q_pos    = u8{1} << 0;
inline constexpr u8 q_gates  = u8{1} << 1;
inline constexpr u8 q_timer0 = u8{1} << 2;
inline constexpr u8 q_timer1 = u8{1} << 3;
inline constexpr u8 q_stage  = u8{1} << 4;
// ACK fields: mirror[7:0], reason[10:8], t0run[11], t1run[12]
constexpr u8 ack_mirror_of(u32 i) noexcept {
  return static_cast<u8>(payload_of(i) & 0xFF);
}
constexpr u8 ack_reason_of(u32 i) noexcept {
  return static_cast<u8>((payload_of(i) >> 8) & 0x7);
}
constexpr bool ack_t0_run_of(u32 i) noexcept {
  return ((payload_of(i) >> 11) & 0x1) != 0;
}
constexpr bool ack_t1_run_of(u32 i) noexcept {
  return ((payload_of(i) >> 12) & 0x1) != 0;
}

// reason codes
inline constexpr u8 reason_ok    = 0;
inline constexpr u8 reason_rec   = 1;
inline constexpr u8 reason_gen   = 2;
inline constexpr u8 reason_timer = 3;

// ===== encoders =====
constexpr u32 make_nop() noexcept { return encode(f_nop, 0); }
constexpr u32 make_gates(u8 g) noexcept { return encode(f_gates, g); }
constexpr u32 make_arm(u8 slot, u8 target, u8 count) noexcept {
  return encode(f_arm, (u32{slot} & 1) << 8 | (u32{target} & 0x7) << 9 |
                           (u32{count} & 0x1F) << 12);
}
constexpr u32 make_arm2(u8 t0t, u8 t0c, u8 t1t, u8 t1c) noexcept {
  return encode(f_arm2, (u32{t0t} & 0x7) << 8 | (u32{t0c} & 0x1F) << 11 |
                             (u32{t1t} & 0x7) << 16 | (u32{t1c} & 0x1F) << 19);
}
constexpr u32 make_gates_arm(u8 g, u8 slot, u8 target, u8 count) noexcept {
  return encode(f_gates_arm, u32{g} | (u32{slot} << 8) | (u32{target} << 9) |
                                 (u32{count} << 12));
}
constexpr u32 make_gates_arm2(u8 g, u8 t0t, u8 t0c, u8 t1t, u8 t1c) noexcept {
  return encode(f_gates_arm2, u32{g} | (u32{t0t} << 8) | (u32{t0c} << 11) |
                                  (u32{t1t} << 16) | (u32{t1c} << 19));
}
constexpr u32 make_query(u8 bitmap) noexcept { return encode(f_query, bitmap); }
constexpr u32 make_ack(u8 mirror, u8 reason, bool t0run, bool t1run) noexcept {
  return encode(f_ack, u32{mirror} | (u32{reason} << 8) | (u32{t0run} << 11) |
                           (u32{t1run} << 12));
}

// ===== validity (out-of-range is a compile error, never truncation) =====
constexpr bool valid(u32 instr) noexcept {
  const u8 fam = family_of(instr);
  const u32 p = payload_of(instr);
  const u8 g = static_cast<u8>(p & 0xFF);

  if (fam == f_nop) return p == 0;
  if (fam == f_gates) return gates::consistent(g);
  if (fam == f_arm) return arm_count_of(instr) >= 1;
  if (fam == f_arm2)
    return t0_count_of(instr) >= 1 && t1_count_of(instr) >= 1;
  if (fam == f_gates_arm)
    return gates::consistent(g) && arm_count_of(instr) >= 1;
  if (fam == f_gates_arm2)
    return gates::consistent(g) && t0_count_of(instr) >= 1 &&
           t1_count_of(instr) >= 1;
  if (fam == f_query) return (p & 0xFF) != 0;
  if (fam == f_ack) return true;
  return false;  // EXT is unregistered
}

// ===== looper state word (UFCO flags, 64 bits, original layout) =====
namespace word {
inline constexpr std::size_t pos_shift    = 0;   // u16
inline constexpr std::size_t user_shift   = 16;  // u8: state nibble + stage nibble
inline constexpr std::size_t timer0_shift = 24;  // u16: [12:0] count, [15:13] target
inline constexpr std::size_t timer1_shift = 40;  // u16
inline constexpr std::size_t gates_shift  = 56;  // u8

inline constexpr u64 pos_mask    = u64{0xFFFF} << pos_shift;
inline constexpr u64 user_mask   = u64{0xFF} << user_shift;
inline constexpr u64 timer0_mask = u64{0xFFFF} << timer0_shift;
inline constexpr u64 timer1_mask = u64{0xFFFF} << timer1_shift;
inline constexpr u64 gates_mask  = u64{0xFF} << gates_shift;

// user byte: bits 16..19 state flags, bits 20..23 one-hot stage mirror
inline constexpr u8 st_generator_delay  = u8{1} << 0;  // bit 16
inline constexpr u8 st_no_generator     = u8{1} << 1;  // bit 17
inline constexpr u8 st_recv_init_failed = u8{1} << 2;  // bit 18
inline constexpr u8 st_init_pending     = u8{1} << 3;  // bit 19
inline constexpr u8 st_mask             = 0x0F;
inline constexpr u8 stage_nibble_mask   = 0xF0;
inline constexpr u8 stage_pre_gen       = 0x10;
inline constexpr u8 stage_post_gen      = 0x20;
inline constexpr u8 stage_pre_rec       = 0x40;
inline constexpr u8 stage_post_rec      = 0x80;

constexpr u8 user_v(u64 w)   noexcept { return static_cast<u8>((w >> user_shift) & 0xFF); }
constexpr u8 state_v(u64 w)  noexcept { return static_cast<u8>(user_v(w) & st_mask); }
constexpr u8 stage_nibble_v(u64 w) noexcept {
  return static_cast<u8>(user_v(w) & stage_nibble_mask);
}
constexpr u64 with_user(u64 w, u8 v) noexcept {
  return (w & ~user_mask) | (u64{v} << user_shift);
}
constexpr u64 with_state(u64 w, u8 v) noexcept {
  return with_user(w, static_cast<u8>((user_v(w) & ~st_mask) | (v & st_mask)));
}
constexpr u64 with_stage_nibble(u64 w, u8 v) noexcept {
  return with_user(w, static_cast<u8>((user_v(w) & ~stage_nibble_mask) |
                                      (v & stage_nibble_mask)));
}

// timer slot: [12:0] count, [15:13] target
inline constexpr u16 ts_count(u16 slot) noexcept {
  return static_cast<u16>(slot & 0x1FFF);
}
inline constexpr u16 ts_target(u16 slot) noexcept {
  return static_cast<u16>((slot >> 13) & 0x7);
}
inline constexpr u16 ts_make(u16 count, u8 target) noexcept {
  return static_cast<u16>((count & 0x1FFF) |
                          (static_cast<u16>(target & 0x7) << 13));
}

constexpr u64 make(u8 g, u8 stage, u16 pos, u16 t0, u16 t1) noexcept {
  return (u64{pos} << pos_shift) | (u64{stage} << user_shift) |
         (u64{t0} << timer0_shift) | (u64{t1} << timer1_shift) |
         (u64{g} << gates_shift);
}
constexpr u16 pos_v(u64 w)    noexcept { return static_cast<u16>(w >> pos_shift); }
constexpr u8  stage_v(u64 w)  noexcept { return stage_nibble_v(w); }
constexpr u16 timer0_v(u64 w) noexcept { return static_cast<u16>((w >> timer0_shift) & 0xFFFF); }
constexpr u16 timer1_v(u64 w) noexcept { return static_cast<u16>((w >> timer1_shift) & 0xFFFF); }
constexpr u8  gates_v(u64 w)  noexcept { return static_cast<u8>((w >> gates_shift) & 0xFF); }

constexpr u64 with_pos(u64 w, u16 v)    noexcept { return (w & ~pos_mask) | (u64{v} << pos_shift); }
constexpr u64 with_timer0(u64 w, u16 v) noexcept { return (w & ~timer0_mask) | (u64{v} << timer0_shift); }
constexpr u64 with_timer1(u64 w, u16 v) noexcept { return (w & ~timer1_mask) | (u64{v} << timer1_shift); }
constexpr u64 with_gates(u64 w, u8 v)   noexcept { return (w & ~gates_mask) | (u64{v} << gates_shift); }

// saturating decrement of one timer slot (count 0 stays 0)
constexpr u16 ts_tick(u16 slot) noexcept {
  const u16 c = ts_count(slot);
  return ts_make(static_cast<u16>(c - (c != 0)), static_cast<u8>(ts_target(slot)));
}
constexpr u64 tick(u64 w) noexcept {
  return with_timer0(with_timer1(w, ts_tick(timer1_v(w))), ts_tick(timer0_v(w)));
}
}  // namespace word

}  // namespace meta_isa32
