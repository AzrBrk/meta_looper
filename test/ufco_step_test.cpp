#include "meta_looper_step.hpp"
#include "meta_iostream.hpp"

#include <type_traits>

using namespace meta_isa32;
using namespace meta_looper;
using namespace meta_iostream;

// Condition sees ONLY the USER word, a window tag and the object.
struct yield_cond {
  template <class UFlags, class Window, class Obj>
  struct apply {
    static constexpr u64 value = u64{make_gates(gates::defaults)} << 32;
  };
};
using cond_o = meta_objects::meta_states_object<u64{0}, yield_cond>;
using gen2 = index_sequence_istream<2>;

using S0 = state<meta_ufco::control_o, cond_o, meta_iterator, gen2>;
using st0 = step<S0>;

// The window logic ran and the loop wants to continue.
static_assert(st0::entered);
static_assert(st0::cont);
static_assert(st0::observed);   // OBS + CONT
static_assert(st0::consume);    // CONSUME default
static_assert(!st0::swap_);     // FWD on

// Gates come from UFCO's high byte, not from the condition's raw word.
static_assert(lw::gates_v(st0::e0::L1) == gates::defaults);

// Generator advanced; receiver received.
static_assert(std::is_same_v<typename st0::gen1, meta_invoke<gen2>>);
static_assert(std::is_same_v<typename st0::next::gen, meta_invoke<gen2>>);

// Accounting: POS incremented once, stage reset, one-shots consumed.
static_assert(lw::pos_v(st0::L) == 1);
static_assert(lw::stage_v(st0::L) == lw::stage_pre_gen);
static_assert((lw::gates_v(st0::L) & gates::finish) == 0);
static_assert((lw::gates_v(st0::L) & gates::fwd) == 0);

// The condition's USER word keeps the 32 user bits and receives the ACK.
static_assert((st0::e0::U2 & 0xFFFF'FFFFull) == 0);            // user flags preserved
static_assert(is_ack(static_cast<u32>(st0::e0::U2 >> 32)));    // reply in instruction slot

int main() { return 0; }
