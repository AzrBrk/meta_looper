#include "meta_core64.hpp"
#include <cstdint>

using namespace meta_core64;

// ---------------- ISA ----------------
consteval bool isa_checks() {
  static_assert(isa::valid(isa::encode(isa::f_gates, 0x55)));
  static_assert(isa::valid(isa::encode(isa::f_nop, 0)));
  static_assert(isa::valid(isa::encode(isa::f_timer, (3 << 8) | 5)));
  static_assert(isa::valid(isa::encode(isa::f_gates_arm, (2 << 11) | (1 << 8) | 0x0B)));
  static_assert(isa::encode(isa::f_gates, 0x55) == u16((0x55 << 3) | isa::f_gates));

  constexpr u64 dg = isa::decode(isa::encode(isa::f_gates, 0x55));
  static_assert(isa::d_gates_v(dg) == 0x55);
  static_assert(isa::d_family_v(dg) == isa::f_gates);
  static_assert(isa::d_rebuild_v(dg) && !isa::d_arm_v(dg));

  constexpr u64 dn = isa::decode(isa::encode(isa::f_nop, 0));
  static_assert(isa::d_gates_v(dn) == gates::defaults);

  constexpr u64 dt = isa::decode(isa::encode(isa::f_timer, (3 << 8) | 5));
  static_assert(isa::d_count_v(dt) == 5 && isa::d_target_v(dt) == 3);
  static_assert(isa::d_arm_v(dt) && !isa::d_rebuild_v(dt));

  constexpr u64 dga = isa::decode(isa::encode(isa::f_gates_arm, (2 << 11) | (1 << 8) | 0x0B));
  static_assert(isa::d_count_v(dga) == 2 && isa::d_target_v(dga) == 1);

  constexpr u64 bad = isa::decode(isa::encode(isa::f_gates, gates::rec));
  static_assert(isa::d_reason_v(bad) == isa::reason_rec);
  return true;
}
static_assert(isa_checks());

// ---------------- apply ----------------
consteval bool apply_checks() {
  constexpr u64 L0 = lw::make(0xAA, 2, 0, 0, 41);
  constexpr u64 a1 = apply(L0, isa::encode(isa::f_gates, 0x0F));
  static_assert(lw::gates_v(a1) == 0x0F);
  static_assert(lw::window_v(a1) == 2);
  static_assert(lw::pos_v(a1) == 41);

  constexpr u64 Z = lw::make(0, 0, 0, 0, 0);
  constexpr u64 t1 = apply(Z, isa::encode(isa::f_timer, (7 << 8) | 9));
  static_assert(lw::t0_v(t1) == 9 && lw::t1_v(t1) == 0);
  constexpr u64 t2 = apply(t1, isa::encode(isa::f_timer, (7 << 8) | 4));
  static_assert(lw::t0_v(t2) == 9 && lw::t1_v(t2) == 4);
  constexpr u64 t3 = apply(t2, isa::encode(isa::f_timer, (7 << 8) | 2));
  static_assert(lw::t0_v(t3) == 9 && lw::t1_v(t3) == 4);   // 槽满，不覆盖
  static_assert(reason_of(t2, isa::encode(isa::f_timer, (7 << 8) | 2)) == isa::reason_timer);
  return true;
}
static_assert(apply_checks());

// ---------------- communicate：一次收敛 ----------------
struct user_quiet {
  template <u8 W, u64 L, u64 U, class Obj>
  static constexpr u64 value = U;
};

consteval bool converge_once() {
  using Obj = int;
  constexpr u64 U0 = uw::make(isa::encode(isa::f_gates, 0x0Du), 0x1234'5678'9ABCull);
  constexpr u64 R = communicate<lw::make(0, 1, 0, 0, 7), U0, Obj, user_quiet>();
  // 返回的是用户最终请求（ACK 只在握手内部出现）
  static_assert(R == U0);
  static_assert(uw::user_v(R) == 0x1234'5678'9ABCull);   // USER 不动
  constexpr u16 m = uw::mail_v(R);
  constexpr u64 L1 = apply(lw::make(0, 1, 0, 0, 7), m);
  static_assert(lw::gates_v(L1) == 0x0D);
  static_assert(lw::pos_v(L1) == 7 && lw::window_v(L1) == 1);
  return true;
}
static_assert(converge_once());

// ---------------- communicate：看到回执后修正一次 ----------------
struct user_retry {
  template <u8 W, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    constexpr u16 m = uw::mail_v(U);
    return isa::is_ack(m) && isa::ack_reason(m) == isa::reason_timer
               ? uw::with_mail(U, isa::encode(isa::f_gates, gates::obs))
               : U;
  }();
};

consteval bool converge_twice() {
  using Obj = int;
  constexpr u64 L0 = lw::make(0, 0, 1, 1, 0);            // 两个槽都忙
  constexpr u64 U0 = uw::make(isa::encode(isa::f_timer, (7 << 8) | 3), 0);
  constexpr u64 R = communicate<L0, U0, Obj, user_retry>();
  constexpr u16 m = uw::mail_v(R);
  constexpr u64 L1 = apply(L0, m);
  static_assert(lw::gates_v(L1) == gates::obs);
  static_assert(lw::t0_v(L1) == 1 && lw::t1_v(L1) == 1);   // 未装表
  return true;
}
static_assert(converge_twice());

// ---------------- 输入已是 ACK：立即收敛 ----------------
consteval bool already_acked() {
  using Obj = int;
  constexpr u64 U0 = uw::make(isa::ack(0, 0, false, false), 0);
  constexpr u64 R = communicate<0, U0, Obj, user_quiet>();
  static_assert(R == U0);
  return true;
}
static_assert(already_acked());

int main() { return 0; }
