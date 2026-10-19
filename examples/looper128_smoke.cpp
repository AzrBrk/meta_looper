#include "meta_states.hpp"

using namespace meta_states;
using namespace meta_objects;

constexpr u64 sentinel = 0xFEDCBA9876543210ull;
using initial = flags_operator<condition_seed>::set<u64, 64, sentinel>;
using armed = set_timer<initial, 0, G_NOT_SKIP, 2>;
static_assert(effective<armed>::get<u64, 64> == sentinel);
static_assert(!effective<armed>::test<G_NOT_SKIP>);
static_assert(tick_timers<armed>::get<u64, 64> == sentinel);

struct executor {
  template<class Stage, class Flags, class Object>
  struct apply {
    static constexpr auto value = [] {
      if constexpr (Stage::value == pre_gen_exec) {
        if constexpr (Flags::template get<u16, position_shift> == 3) {
          return Flags::template opDeact<G_CONTINUE>::value;
        }
      }
      return Flags::template inc<u64, 64>::value;
    }();
  };
};

using condition = make_condition<executor, armed::value>;
using loop = run<condition, meta_empty_o>;
using final_flags = loop::type::states;
static_assert(final_flags::get<u64, 64> == sentinel + 15);
static_assert(timer_count<final_flags, 0> == 0);
static_assert(!final_flags::test<G_CONTINUE>);

constexpr bool check_callbacks() {
  unsigned calls = 0;
  loop::for_each([&]<class Stream>(Stream) {
    constexpr auto upper = Stream::states::template get<u64, 64>;
    static_assert(upper >= sentinel + 4 && upper <= sentinel + 12);
    ++calls;
  });
  return calls == 1;
}
static_assert(check_callbacks());

int main() {}