#pragma once

// ============================================================================
// meta_core64: the 64-bit control plane, single-register edition.
//
// Two words, no u128, no composite values:
//   looper word : GATES(8) WINDOW(8) T0C(8) T1C(8) POS(32)
//   user word   : MAIL(16) USER(48)
//
// Hard rule (deliberate, readability is the casualty): every value that crosses
// a function boundary is one scalar (u64/u32/u16/u8/bool). No struct, no pair,
// no tuple, and no "family + arg" parameter pairs - the instruction travels as
// one u16 MAIL and is unpacked with shifts.
//
// decode() returns ONE packed u64 instead of a struct:
//   bits 0..7   gates
//   bits 8..15  count
//   bits 16..23 target
//   bits 24..31 family
//   bit  32     rebuild
//   bit  33     arm
//   bits 40..47 reason
// Accessors are shifts/masks on that word.
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace meta_core64 {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ===== mask helpers =====
constexpr u8  mask8(bool on) noexcept  { return static_cast<u8>(0 - u8{on}); }
constexpr u64 mask64(bool on) noexcept { return 0 - u64{on}; }

constexpr u8 select8(bool c, u8 a, u8 b) noexcept {
  const u8 m = mask8(c);
  return static_cast<u8>((a & m) | (b & static_cast<u8>(~m)));
}
constexpr u64 select64(bool c, u64 a, u64 b) noexcept {
  const u64 m = mask64(c);
  return (a & m) | (b & ~m);
}

// ===== looper word =====
namespace lw {
inline constexpr std::size_t gates_shift  = 0;
inline constexpr std::size_t window_shift = 8;
inline constexpr std::size_t target0_shift = 10;
inline constexpr std::size_t target1_shift = 13;
inline constexpr std::size_t t0_shift     = 16;
inline constexpr std::size_t t1_shift     = 24;
inline constexpr std::size_t pos_shift    = 32;

inline constexpr u64 gates_mask   = u64{0xFF} << gates_shift;
inline constexpr u64 window_mask  = u64{0b11} << window_shift;   // 2 bits (0..3)
inline constexpr u64 target0_mask = u64{0b111} << target0_shift; // 3 bits
inline constexpr u64 target1_mask = u64{0b111} << target1_shift; // 3 bits
inline constexpr u64 t0_mask      = u64{0xFF} << t0_shift;
inline constexpr u64 t1_mask      = u64{0xFF} << t1_shift;
inline constexpr u64 pos_mask     = u64{0xFFFF'FFFF} << pos_shift;

constexpr u64 make(u8 gates_v, u8 window_v, u8 t0_v, u8 t1_v, u32 pos_v) noexcept {
  return (u64{gates_v} << gates_shift) | (u64{window_v} << window_shift) |
         (u64{t0_v} << t0_shift) | (u64{t1_v} << t1_shift) |
         (u64{pos_v} << pos_shift);
}
constexpr u8  gates_v(u64 w)  noexcept { return static_cast<u8>(w & gates_mask); }
constexpr u8  window_v(u64 w) noexcept { return static_cast<u8>((w >> window_shift) & 0b11); }
constexpr u8  target0_v(u64 w) noexcept { return static_cast<u8>((w >> target0_shift) & 0b111); }
constexpr u8  target1_v(u64 w) noexcept { return static_cast<u8>((w >> target1_shift) & 0b111); }
constexpr u8  t0_v(u64 w)     noexcept { return static_cast<u8>((w >> t0_shift) & 0xFF); }
constexpr u8  t1_v(u64 w)     noexcept { return static_cast<u8>((w >> t1_shift) & 0xFF); }
constexpr u32 pos_v(u64 w)    noexcept { return static_cast<u32>(w >> pos_shift); }

constexpr u64 with_gates(u64 w, u8 g) noexcept {
  return (w & ~gates_mask) | (u64{g} << gates_shift);
}
constexpr u64 with_window(u64 w, u8 win) noexcept {
  return (w & ~window_mask) | (u64{win} << window_shift);
}
constexpr u64 with_target0(u64 w, u8 t) noexcept {
  return (w & ~target0_mask) | (u64{t} << target0_shift);
}
constexpr u64 with_target1(u64 w, u8 t) noexcept {
  return (w & ~target1_mask) | (u64{t} << target1_shift);
}
constexpr u64 with_pos(u64 w, u32 p) noexcept {
  return (w & ~pos_mask) | (u64{p} << pos_shift);
}
constexpr u64 with_t0(u64 w, u8 c) noexcept {
  return (w & ~t0_mask) | (u64{c} << t0_shift);
}
constexpr u64 with_t1(u64 w, u8 c) noexcept {
  return (w & ~t1_mask) | (u64{c} << t1_shift);
}

// Only GATES may change; WINDOW / timers / POS come back from the looper's own
// snapshot, so a stray write cannot accumulate.
constexpr u64 restore_owned(u64 before, u64 after) noexcept {
  return (after & gates_mask) | (before & ~gates_mask);
}
}  // namespace lw

// ===== gates (positive polarity; OBS replaces the old G_NOT_SKIP) =====
namespace gates {
inline constexpr u8 cont    = u8{1} << 0;
inline constexpr u8 rec     = u8{1} << 1;
inline constexpr u8 gen     = u8{1} << 2;
inline constexpr u8 obs     = u8{1} << 3;
inline constexpr u8 finish  = u8{1} << 4;
inline constexpr u8 fwd     = u8{1} << 5;
inline constexpr u8 init    = u8{1} << 6;
inline constexpr u8 consume = u8{1} << 7;

inline constexpr u8 defaults = cont | rec | gen | obs | fwd | consume;
inline constexpr u8 mask_all = 0xFF;
inline constexpr u8 stop_mask = cont | rec | gen;

constexpr bool consistent(u8 g) noexcept {
  return ((g & (rec | gen)) == 0) | ((g & cont) != 0);
}
constexpr u8 reason_for(u8 g) noexcept {
  const bool needs_cont = (g & (rec | gen)) != 0;
  const bool has_cont = (g & cont) != 0;
  const u8 culprit = select8((g & rec) != 0, 1, 2);   // reason_rec / reason_gen
  return select8(needs_cont, select8(has_cont, 0, culprit), 0);
}
}  // namespace gates

// ===== value-plane decisions a callback may return =====
// skip / yield only say "keep going"; stop says "end the walk here". Whether a
// round is observed at all is decided by the gates (OBS), not by these tags.
namespace control {
struct skip_t {};
struct yield_t {};
struct stop_t {};
constexpr skip_t  skip()  noexcept { return {}; }
constexpr yield_t yield() noexcept { return {}; }
constexpr stop_t  stop()  noexcept { return {}; }
}  // namespace control

// ===== user word =====
namespace uw {
inline constexpr std::size_t mail_shift = 0;
inline constexpr std::size_t user_shift = 16;
inline constexpr u64 mail_mask = u64{0xFFFF};
inline constexpr u64 user_mask = ~mail_mask;
inline constexpr u64 user_bits = 0xFFFF'FFFF'FFFF;   // 48 bits

constexpr u64 make(u16 mail_v, u64 user48) noexcept {
  return (u64{mail_v} << mail_shift) | ((user48 & user_bits) << user_shift);
}
constexpr u16 mail_v(u64 w) noexcept { return static_cast<u16>(w & mail_mask); }
constexpr u64 user_v(u64 w) noexcept { return (w >> user_shift) & user_bits; }
constexpr u64 with_mail(u64 w, u16 m) noexcept {
  return (w & ~mail_mask) | (u64{m} << mail_shift);
}
constexpr u64 with_user(u64 w, u64 u) noexcept {
  return (w & mail_mask) | ((u & user_bits) << user_shift);
}
}  // namespace uw

// ===== ISA: everything takes the one u16 MAIL =====
namespace isa {
inline constexpr u8 f_nop       = 0;
inline constexpr u8 f_gates     = 1;
inline constexpr u8 f_timer     = 2;
inline constexpr u8 f_gates_arm = 3;
inline constexpr u8 f_ack       = 4;
inline constexpr u8 f_ext_first = 5;

inline constexpr u8 reason_ok    = 0;
inline constexpr u8 reason_rec   = 1;
inline constexpr u8 reason_gen   = 2;
inline constexpr u8 reason_timer = 3;

inline constexpr std::size_t d_gates_shift   = 0;
inline constexpr std::size_t d_count_shift   = 8;
inline constexpr std::size_t d_target_shift  = 16;
inline constexpr std::size_t d_family_shift  = 24;
inline constexpr std::size_t d_rebuild_shift = 32;
inline constexpr std::size_t d_arm_shift     = 33;
inline constexpr std::size_t d_reason_shift  = 40;

constexpr u16 encode(u8 family, u16 arg) noexcept {
  return static_cast<u16>((u16{family} & 0x7) | (u16{arg} << 3));
}
constexpr u8  family_of(u16 m) noexcept { return static_cast<u8>(m & 0x7); }
constexpr u16 arg_of(u16 m) noexcept { return static_cast<u16>(m >> 3); }

constexpr bool is_nop(u16 m)   noexcept { return family_of(m) == f_nop; }
constexpr bool is_gates(u16 m) noexcept { return family_of(m) == f_gates; }
constexpr bool is_timer(u16 m) noexcept { return family_of(m) == f_timer; }
constexpr bool is_ga(u16 m)    noexcept { return family_of(m) == f_gates_arm; }
constexpr bool is_ack(u16 m)   noexcept { return family_of(m) == f_ack; }

// Pure boolean arithmetic over the whole mail word.
constexpr bool valid(u16 mail) noexcept {
  const u8 family = family_of(mail);
  const u16 arg = arg_of(mail);
  const u8 g     = static_cast<u8>(arg & 0xFF);
  const u8 cnt8  = static_cast<u8>(arg & 0xFF);
  const u8 cnt2  = static_cast<u8>((arg >> 11) & 0x3);
  const bool hi_zero = (arg >> 8) == 0;
  const bool known = (family <= f_gates_arm) | (family == f_ack);

  return ((!is_nop(mail)) | (arg == 0)) &
         ((!is_gates(mail)) | (hi_zero & gates::consistent(g))) &
         ((!is_timer(mail)) | (cnt8 >= 1)) &
         ((!is_ga(mail)) | ((cnt2 >= 1) & gates::consistent(g))) &
         known;
}

// One u64, bit-hacked: no struct, no per-instruction type.
constexpr u64 decode(u16 mail) noexcept {
  const u8 family = family_of(mail);
  const u16 arg   = arg_of(mail);
  const u8 g      = static_cast<u8>(arg & 0xFF);

  const u8 out_gates = select8(is_nop(mail), gates::defaults,
                               select8(is_gates(mail) | is_ga(mail), g, u8{0}));
  const u8 out_count = select8(is_timer(mail), static_cast<u8>(arg & 0xFF),
                               static_cast<u8>((arg >> 11) & 0x3));
  const u8 out_target = static_cast<u8>((arg >> 8) & 0x7);
  const u8 out_reason = gates::reason_for(out_gates);
  const u64 out_rebuild = static_cast<u64>(is_nop(mail) | is_gates(mail) | is_ga(mail));
  const u64 out_arm = static_cast<u64>(is_timer(mail) | is_ga(mail));

  return u64{out_gates} | (u64{out_count} << d_count_shift) |
         (u64{out_target} << d_target_shift) |
         (u64{family} << d_family_shift) |
         (out_rebuild << d_rebuild_shift) | (out_arm << d_arm_shift) |
         (u64{out_reason} << d_reason_shift);
}

constexpr u8   d_gates_v(u64 d)  noexcept { return static_cast<u8>(d >> d_gates_shift); }
constexpr u8   d_count_v(u64 d)  noexcept { return static_cast<u8>(d >> d_count_shift); }
constexpr u8   d_target_v(u64 d) noexcept { return static_cast<u8>(d >> d_target_shift); }
constexpr u8   d_family_v(u64 d) noexcept { return static_cast<u8>(d >> d_family_shift); }
constexpr bool d_rebuild_v(u64 d) noexcept { return ((d >> d_rebuild_shift) & 1) != 0; }
constexpr bool d_arm_v(u64 d) noexcept { return ((d >> d_arm_shift) & 1) != 0; }
constexpr u8   d_reason_v(u64 d) noexcept { return static_cast<u8>(d >> d_reason_shift); }

// ACK layout: FAMILY=f_ack, then ARG carries mirror(8) reason(2) t0(1) t1(1).
constexpr u16 ack(u8 mirror, u8 reason, bool t0_active, bool t1_active) noexcept {
  const u32 t0 = (0u - u32{t0_active}) & (1u << 13);
  const u32 t1 = (0u - u32{t1_active}) & (1u << 14);
  return static_cast<u16>(u32{f_ack} | (u32{mirror} << 3) |
                          (u32{reason} << 11) | t0 | t1);
}
constexpr u8 ack_mirror(u16 m) noexcept { return static_cast<u8>((m >> 3) & 0xFF); }
constexpr u8 ack_reason(u16 m) noexcept { return static_cast<u8>((m >> 11) & 0x3); }
constexpr bool ack_t0_active(u16 m) noexcept { return ((m >> 13) & 1) != 0; }
constexpr bool ack_t1_active(u16 m) noexcept { return ((m >> 14) & 1) != 0; }

// Request identity: same key means the same request.
constexpr u32 key_of(u16 mail) noexcept {
  const u64 d = decode(mail);
  return (u32{d_family_v(d)} << 24) | (u32{d_gates_v(d)} << 16) |
         (u32{d_target_v(d)} << 8) | u32{d_count_v(d)};
}
}  // namespace isa

// ===== applying one instruction: one u64 in, one u64 out =====
constexpr u64 apply(u64 looper, u16 mail) noexcept {
  const u64 d = isa::decode(mail);
  const u64 rebuild = mask64(isa::d_rebuild_v(d));

  u64 out = (looper & ~(lw::gates_mask & rebuild)) |
            ((u64{isa::d_gates_v(d)} << lw::gates_shift) & (lw::gates_mask & rebuild));

  const bool arm   = isa::d_arm_v(d);
  const u8 count   = isa::d_count_v(d);
  const u8 target  = isa::d_target_v(d);   // bit index (0..7)
  const bool free0 = lw::t0_v(out) == 0;
  const bool free1 = lw::t1_v(out) == 0;
  const bool arm0  = arm & free0;
  const bool arm1  = arm & !free0 & free1;
  const u64 w0 = mask64(arm0);
  const u64 w1 = mask64(arm1);
  out = (out & ~(lw::t0_mask & w0)) |
        ((u64{count} << lw::t0_shift) & (lw::t0_mask & w0));
  out = (out & ~(lw::t1_mask & w1)) |
        ((u64{count} << lw::t1_shift) & (lw::t1_mask & w1));
  out = (out & ~(lw::target0_mask & w0)) |
        ((u64{target} << lw::target0_shift) & (lw::target0_mask & w0));
  out = (out & ~(lw::target1_mask & w1)) |
        ((u64{target} << lw::target1_shift) & (lw::target1_mask & w1));

  // Immediate suppression: a running slot clears its target gate THIS window, so
  // arming in pre_gen already suppresses the generator gate before it advances.
  const u8 g = lw::gates_v(out);
  const u8 drop0 = static_cast<u8>(select8(
      lw::t0_v(out) != 0,
      static_cast<u8>((u8{1} << lw::target0_v(out)) & ~gates::finish), u8{0}));
  const u8 drop1 = static_cast<u8>(select8(
      lw::t1_v(out) != 0,
      static_cast<u8>((u8{1} << lw::target1_v(out)) & ~gates::finish), u8{0}));
  out = lw::with_gates(out, static_cast<u8>(g & ~drop0 & ~drop1));
  return out;
}

// Reason this instruction would be ignored (timer slots full / gates invalid).
constexpr u8 reason_of(u64 looper, u16 mail) noexcept {
  const u64 d = isa::decode(mail);
  const bool full = (lw::t0_v(looper) != 0) & (lw::t1_v(looper) != 0);
  return select8(isa::d_arm_v(d) & full, isa::reason_timer, isa::d_reason_v(d));
}

// ===== communicate: bounded handshake, one u64 out =====
// UFun holds the user's reaction:
//   template<u8 Window, u64 LooperWord, u64 Uword, class Obj>
//   static constexpr u64 value;
// It sees the ACK inside Uword during the exchange. The function returns the
// user's final request word; the caller applies it once with `apply`.
template <u64 L, u64 U, class Obj, class UFun, std::size_t Fuel = 2,
          u32 PrevKey = 0xFFFF'FFFF>
consteval u64 communicate() {
  constexpr u16 mail = uw::mail_v(U);
  if constexpr (isa::is_ack(mail)) {
    return U;
  } else {
    static_assert(isa::valid(mail),
                  "meta_core64::communicate: invalid instruction");
    constexpr u32 key = isa::key_of(mail);

    if constexpr (key == PrevKey) {
      // Same request as the previous exchange: the ACK stays inside, the caller
      // gets the request it already sent.
      return U;
    } else if constexpr (Fuel == 0) {
      static_assert(isa::is_ack(mail),
                    "communicate: the user kept sending instructions; "
                    "convergence needs a larger Fuel");
      return U;
    } else {
      // No apply here: the caller applies the final request exactly once. The
      // ACK only needs numbers that can be computed from L and the mail word.
      constexpr u64 L1 = apply(L, mail);
      constexpr u64 d = isa::decode(mail);
      constexpr u8 mirror = select8(isa::d_rebuild_v(d), isa::d_gates_v(d), u8{0});
      constexpr u16 reply = isa::ack(mirror, reason_of(L, mail),
                                     lw::t0_v(L1) != 0, lw::t1_v(L1) != 0);
      constexpr u64 U1 = uw::with_mail(U, reply);
      constexpr u64 U2 = UFun::template value<
          static_cast<u8>(lw::window_v(L1)), L1, U1, Obj>;
      if constexpr (U2 == U1) {
        return U;   // the request that produced this ACK
      } else {
        return communicate<L1, U2, Obj, UFun, Fuel - 1, key>();
      }
    }
  }
}

}  // namespace meta_core64
