#include "meta_stream64.hpp"
#include "meta_iostream.hpp"

using namespace meta_core64;
using namespace meta_stream64;
using namespace meta_objects;
using namespace meta_iostream;

// 初始化过的 generator，advance 后 produce 为 eol
using gen1     = index_sequence_istream<1>;
using gen1_adv = meta_invoke<gen1>;
// 计时器：1 次机会，用掉后 timer 为 false
using timer1     = to_timer<meta_empty_o, 1>;
using timer1_used = meta_invoke<timer1>;

constexpr u64 L(u32 pos) { return lw::make(0, 0, 0, 0, pos); }
constexpr u64 U = uw::make(isa::encode(isa::f_gates, gates::defaults), 0xDEAD'BEEF'1234ull);

consteval bool pre_gen_delay() {
  // 第一轮：initialized generator 被压一次（GEN 掉）
  constexpr u64 w0 = stream_cond64<>::value<0, L(0), U, gen1>;
  static_assert((uw::mail_v(w0) >> 3 & 0xFF) == (gates::defaults & ~gates::gen));
  // 第二轮：GEN 恢复
  constexpr u64 w1 = stream_cond64<>::value<0, L(1), U, gen1>;
  static_assert((uw::mail_v(w1) >> 3 & 0xFF) == gates::defaults);
  return true;
}
static_assert(pre_gen_delay());

consteval bool empty_generator() {
  constexpr u64 w = stream_cond64<>::value<0, L(5), U, meta_empty_o>;
  static_assert(uw::mail_v(w) == isa::encode(isa::f_gates, gates::defaults & ~gates::gen));
  return true;
}
static_assert(empty_generator());

consteval bool exhausted_timer() {
  constexpr u64 w = stream_cond64<>::value<0, L(5), U, timer1_used>;
  static_assert((uw::mail_v(w) >> 3 & 0xFF) == (gates::defaults & static_cast<u8>(~meta_stream64::details::stop_mask)));
  return true;
}
static_assert(exhausted_timer());

consteval bool eol_after_gen() {
  constexpr u64 w = stream_cond64<>::value<1, L(0), U, gen1_adv>;
  static_assert((uw::mail_v(w) >> 3 & 0xFF) == (gates::defaults & static_cast<u8>(~meta_stream64::details::stop_mask)));
  return true;
}
static_assert(eol_after_gen());

consteval bool receiver_timer() {
  constexpr u64 w = stream_cond64<>::value<2, L(0), U, timer1_used>;
  static_assert((uw::mail_v(w) >> 3 & 0xFF) == (gates::defaults & static_cast<u8>(~meta_stream64::details::stop_mask)));
  return true;
}
static_assert(receiver_timer());

struct stopper {
  template <u8 Window, u64 Lw, u64 Uw, class Obj>
  static constexpr u64 value = ops::stop(Uw);
};

consteval bool user_stop_kept() {
  constexpr u64 w = stream_cond64<stopper>::value<0, L(3), U, gen1>;
  static_assert((uw::mail_v(w) >> 3 & 0xFF) == gates::obs);
  return true;
}
static_assert(user_stop_kept());

consteval bool user_bits_survive() {
  constexpr u64 w = stream_cond64<>::value<1, L(0), U, gen1_adv>;
  static_assert(uw::user_v(w) == 0xDEAD'BEEF'1234ull);
  return true;
}
static_assert(user_bits_survive());

// 没有规则触发时，TIMER 指令原样保留
struct armer {
  template <u8 Window, u64 Lw, u64 Uw, class Obj>
  static constexpr u64 value = ops::timer(Uw, 5, 3);
};

consteval bool timer_request_survives() {
  constexpr u64 w = stream_cond64<armer>::value<2, L(4), U, meta_iterator>;
  static_assert((uw::mail_v(w) & 0x7) == isa::f_timer);
  constexpr u16 m = uw::mail_v(w);
  static_assert(isa::valid(m));
  constexpr u64 d = isa::decode(m);
  static_assert(isa::d_count_v(d) == 5 && isa::d_target_v(d) == 3);
  return true;
}
static_assert(timer_request_survives());

// 规则触发时，TIMER 被改写成 GATES（arm 丢弃），USER 仍保留
consteval bool timer_overridden_on_rule() {
  constexpr u64 w = stream_cond64<armer>::value<2, L(4), U, timer1_used>;
  static_assert((uw::mail_v(w) & 0x7) == isa::f_gates);
  static_assert(uw::user_v(w) == 0xDEAD'BEEF'1234ull);
  return true;
}
static_assert(timer_overridden_on_rule());

// ---- 便利构造函数 ----
consteval bool ops_checks() {
  constexpr u64 w = uw::make(isa::encode(isa::f_gates, gates::defaults), 0x1234);
  static_assert(ops::gates_of(w) == gates::defaults);
  static_assert(ops::gates_of(ops::set_gates(w, gates::fwd, gates::gen)) ==
                ((gates::defaults | gates::fwd) & ~gates::gen));
  static_assert(ops::gates_of(ops::with_obs(w, false)) ==
                (gates::defaults & ~gates::obs));
  static_assert(uw::user_v(ops::with_gen(w, false)) == 0x1234);   // USER 不动
  static_assert(ops::gates_of(ops::skip(w)) ==
                (gates::cont | gates::gen | gates::fwd | gates::consume));
  static_assert(ops::gates_of(ops::stop(w)) == gates::obs);
  static_assert(ops::gates_of(ops::yield(w)) == gates::defaults);
  return true;
}
static_assert(ops_checks());

int main() { return 0; }
