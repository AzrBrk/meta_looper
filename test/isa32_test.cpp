#include "meta_isa32.hpp"

using namespace meta_isa32;

// ---- gates byte: original bit order (bits 56..63) restored ----
static_assert(gates::gen     == 0x01);   // bit 56  G_CALL_GENERATOR
static_assert(gates::rec     == 0x02);   // bit 57  G_CALL_RECEIVER
static_assert(gates::finish  == 0x04);   // bit 58  G_FINISH
static_assert(gates::fwd     == 0x08);   // bit 59  G_FORWARD
static_assert(gates::consume == 0x10);   // bit 60  G_CONSUME
static_assert(gates::init    == 0x20);   // bit 61  G_INITIALIZE
static_assert(gates::obs     == 0x40);   // bit 62  G_NOT_SKIP
static_assert(gates::cont    == 0x80);   // bit 63  G_CONTINUE
static_assert(gates::defaults == 0xDB);  // cont|obs|gen|rec|fwd|consume
static_assert(gates::stop_mask == 0x83); // cont|rec|gen

// ---- family / payload round-trip ----
static_assert(family_of(make_nop()) == f_nop);
static_assert(payload_of(make_nop()) == 0);
static_assert(valid(make_nop()));

static_assert(family_of(make_gates(gates::defaults)) == f_gates);
static_assert(gates_of(make_gates(gates::defaults)) == gates::defaults);
static_assert(valid(make_gates(gates::defaults)));
// REC without CONT is invalid
static_assert(!valid(make_gates(gates::rec)));
static_assert(valid(make_gates(gates::cont | gates::rec)));

// ---- single timer: slot / target / count5 ----
constexpr u32 a1 = make_arm(1, 5, 7);
static_assert(family_of(a1) == f_arm);
static_assert(arm_slot_of(a1) == 1);
static_assert(arm_target_of(a1) == 5);
static_assert(arm_count_of(a1) == 7);
static_assert(valid(a1));
static_assert(!valid(make_arm(0, 0, 0)));   // count must be >= 1
static_assert(valid(make_arm(0, 0, 31)));   // count5 upper bound

// ---- dual timer: one instruction, two timers ----
constexpr u32 a2 = make_arm2(1, 7, 6, 9);
static_assert(family_of(a2) == f_arm2);
static_assert(t0_target_of(a2) == 1);
static_assert(t0_count_of(a2) == 7);
static_assert(t1_target_of(a2) == 6);
static_assert(t1_count_of(a2) == 9);
static_assert(valid(a2));
static_assert(!valid(make_arm2(0, 0, 0, 1)));

// ---- gates + two timers in ONE instruction ----
constexpr u32 ga2 = make_gates_arm2(gates::cont | gates::gen, 1, 3, 2, 4);
static_assert(family_of(ga2) == f_gates_arm2);
static_assert(gates_of(ga2) == (gates::cont | gates::gen));
static_assert(t0_target_of(ga2) == 1);
static_assert(t0_count_of(ga2) == 3);
static_assert(t1_target_of(ga2) == 2);
static_assert(t1_count_of(ga2) == 4);
static_assert(valid(ga2));

// ---- query bitmap ----
constexpr u32 q = make_query(q_pos | q_timer0);
static_assert(is_query(q));
static_assert(query_bitmap_of(q) == (q_pos | q_timer0));
static_assert(valid(q));
static_assert(!valid(make_query(0)));

// ---- ack ----
constexpr u32 ak = make_ack(0xA5, reason_timer, true, false);
static_assert(is_ack(ak));
static_assert(ack_mirror_of(ak) == 0xA5);
static_assert(ack_reason_of(ak) == reason_timer);
static_assert(ack_t0_run_of(ak));
static_assert(!ack_t1_run_of(ak));

// ---- instruction word fits in 32 bits, user keeps the other 32 ----
constexpr u64 user_word = (u64{ga2} << 32) | 0xDEADBEEFull;
static_assert((user_word >> 32) == ga2);
static_assert((user_word & 0xFFFF'FFFFull) == 0xDEADBEEFull);

// ---- looper state word: old layout fully restored ----
constexpr u64 st0 = word::make(gates::defaults, word::stage_pre_gen, 0, 0, 0);
static_assert(word::gates_v(st0) == gates::defaults);      // high byte = action
static_assert(word::stage_v(st0) == word::stage_pre_gen);
static_assert(word::state_v(st0) == 0);
static_assert(word::pos_v(st0) == 0);

// 4 state bits back in bits 16..19
constexpr u8 st_bits = word::st_generator_delay | word::st_no_generator |
                       word::st_recv_init_failed | word::st_init_pending;
constexpr u64 st1 = word::with_state(st0, st_bits);
static_assert(word::state_v(st1) == st_bits);
static_assert(((st1 >> 16) & 0xF) == st_bits);

// stage one-hot in bits 20..23
constexpr u64 st2 = word::with_stage_nibble(st0, word::stage_post_rec);
static_assert(word::stage_v(st2) == word::stage_post_rec);
static_assert(((st2 >> 20) & 0xF) == 0x8);

// timer slot: 13-bit count + 3-bit target inside 16 bits
constexpr u16 slot = word::ts_make(5000, 5);
static_assert(word::ts_count(slot) == 5000);
static_assert(word::ts_target(slot) == 5);

constexpr u64 st3 = word::with_timer0(st0, slot);
static_assert(word::ts_count(word::timer0_v(st3)) == 5000);
static_assert(word::ts_target(word::timer0_v(st3)) == 5);

constexpr u64 st4 = word::tick(st3);
static_assert(word::ts_count(word::timer0_v(st4)) == 4999);
static_assert(word::ts_target(word::timer0_v(st4)) == 5);   // target survives tick

// saturates at 0, target kept
constexpr u16 zero = word::ts_make(0, 3);
static_assert(word::ts_count(word::ts_tick(zero)) == 0);
static_assert(word::ts_target(word::ts_tick(zero)) == 3);

// pos unaffected by tick
constexpr u64 st5 = word::with_pos(st0, 0xFFFF);
static_assert(word::pos_v(word::tick(st5)) == 0xFFFF);

int main() { return 0; }
