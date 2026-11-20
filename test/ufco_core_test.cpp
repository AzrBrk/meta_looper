#include "meta_ufco.hpp"

using namespace meta_isa32;
using meta_objects::flags_operator;

struct win0 {};
struct win1 {};

// Condition: sees only the USER word (plus window tag and object). No looper
// word anywhere in its signature.
struct yield_fn {
  template <class UFlags, class Window, class Obj>
  struct apply {
    static constexpr u64 value = u64{make_gates(gates::defaults)} << 32;
  };
};
using cond_o = meta_objects::meta_states_object<u64{0}, yield_fn>;

// The condition produces a USER word; the looper word is never passed in.
using C0 = meta_invoke<cond_o, win0, void>;
static_assert(C0::flags::value == (u64{make_gates(gates::defaults)} << 32));
using C1 = meta_invoke<C0, win1, int>;
static_assert(C1::flags::value == (u64{make_gates(gates::defaults)} << 32));

// UFCO decodes GATES: the looper word's high byte becomes the action.
constexpr u64 U_gates = u64{make_gates(gates::defaults)} << 32;
using F_gates = meta_invoke<meta_ufco::control_o, flags_operator<U_gates>, void>;
static_assert(word::gates_v(F_gates::flags::value) == gates::defaults);

// One instruction arms two timers (the long-instruction case).
constexpr u64 U_arm2 = u64{make_arm2(1, 7, 6, 9)} << 32;
using F_arm2 = meta_invoke<meta_ufco::control_o, flags_operator<U_arm2>, void>;
static_assert(word::ts_count(word::timer0_v(F_arm2::flags::value)) == 7);
static_assert(word::ts_target(word::timer0_v(F_arm2::flags::value)) == 1);
static_assert(word::ts_count(word::timer1_v(F_arm2::flags::value)) == 9);
static_assert(word::ts_target(word::timer1_v(F_arm2::flags::value)) == 6);
// Implicit micro-op: the arming window clears both target gates at once.
static_assert((word::gates_v(F_arm2::flags::value) & (u8{1} << 1)) == 0);
static_assert((word::gates_v(F_arm2::flags::value) & (u8{1} << 6)) == 0);

// QUERY: reply packs only what was asked.
constexpr u64 st = word::with_pos(meta_ufco::seed, 42);
constexpr u32 rep_pos = meta_ufco::reply_for(make_query(q_pos), st);
static_assert(is_ack(rep_pos));
static_assert((payload_of(rep_pos) & 0xFFFF) == 42);
static_assert(((payload_of(rep_pos) >> 16) & 0xFFFF) == 0);  // nothing else packed

constexpr u32 rep_both =
    meta_ufco::reply_for(make_query(q_pos | q_gates), st);
static_assert((payload_of(rep_both) & 0xFFFF) == 42);
static_assert(((payload_of(rep_both) >> 16) & 0xFF) == gates::defaults);

// Action reply is a normal ACK with timer activity.
constexpr u32 rep_arm = meta_ufco::reply_for(make_arm(0, 2, 3), F_arm2::flags::value);
static_assert(is_ack(rep_arm));
static_assert(ack_t0_run_of(rep_arm));
static_assert(ack_t1_run_of(rep_arm));

int main() { return 0; }
