#include "meta_ios.hpp"

#include <cassert>
#include <cstddef>

using namespace meta_ios;
using namespace meta_iostream;
using namespace meta_states;

namespace driver_flags_tests {

using loop_t =
    meta_states::run<meta_ios::stream_states_co<>, meta_iterator,
                     index_sequence_istream<4>>;

using skip_driver = typename loop_t::template decide<
    meta_states_control::skip_t>;
using yield_driver = typename loop_t::template decide<
    meta_states_control::yield_t>;
using stop_driver = typename loop_t::template decide<
    meta_states_control::stop_t>;

static_assert(skip_driver::flags::template test<
              meta_states_looper_details::driver_control::skip_bit>);
static_assert(yield_driver::flags::template test<
              meta_states_looper_details::driver_control::yielded_bit>);
static_assert(stop_driver::flags::template test<
              meta_states_looper_details::driver_control::stopped_bit>);
static_assert(stop_driver::flags::template get<
                  u8, meta_states_looper_details::driver_control::phase_shift> ==
              meta_states_looper_details::driver_control::phase_done);

using pipe_t = meta_pipe<index_sequence_istream<3>>::run<meta_ostream<>>;

namespace dc = meta_states_looper_details::driver_control;
using meta_objects::flags_operator;

struct incomplete;
static_assert(meta_states_looper_details::any_observes<true, incomplete>::value);
static_assert(meta_states_looper_details::any_within<true, 0, incomplete>::value);
static_assert(std::is_same_v<
    meta_states_looper_details::enter_window_step<
        false, pre_gen_exec, incomplete, void>, incomplete>);
static_assert(std::is_same_v<
    meta_states_looper_details::normalize_decision<incomplete, void, 0, 0>,
    incomplete>);
static_assert(meta_states_looper_details::is_for_each_stop_v<const stop_t&>);
static_assert(meta_states_looper_details::is_for_each_decision_v<volatile skip_t&&>);
static_assert(!meta_states_looper_details::is_for_each_decision_v<incomplete>);

template <bool Observe, bool Continue>
struct driver_step {
  static constexpr bool observe_ = Observe;
  static constexpr bool continue_ = Continue;
};

template <u8 Remaining, u8 Timer0, u8 Timer1, bool Observe, bool Continue,
          bool Sticky = false>
consteval bool check_round() {
  using seed = typename flags_operator<u128{0, ~u64{0}}>
      ::template opDeact<dc::observes_bit | dc::has_more_bit |
                         dc::boundary_bit | dc::stopped_bit>
      ::template opSet<Sticky ? dc::observes_bit | dc::has_more_bit |
                                  dc::boundary_bit | dc::stopped_bit : u128{0}>
      ::template set<u16, dc::pc_shift, 65535>
      ::template set<u8, dc::chunk_round_shift, 255>
      ::template set<u8, dc::remaining_shift, Remaining>
      ::template set<u8, dc::timer0_shift, Timer0>
      ::template set<u8, dc::timer1_shift, Timer1>;
  using result = typename meta_states_looper_details::default_driver_policy<1>
      ::template round<seed, meta_states_looper_details::driver_control::input<
                                driver_step<Observe, Continue>,
                                dc::window_round>>::type;
  constexpr u8 phase = !Continue ? dc::phase_done :
      Remaining == 0 ? dc::phase_more : dc::phase_run;
  static_assert(result::template get<u8, dc::remaining_shift> ==
                (Remaining == 0 ? 0 : Remaining - 1));
  static_assert(result::template get<u8, dc::timer0_shift> ==
                (Timer0 == 0 ? 0 : Timer0 - 1));
  static_assert(result::template get<u8, dc::timer1_shift> ==
                (Timer1 == 0 ? 0 : Timer1 - 1));
  static_assert(result::template get<u16, dc::pc_shift> == 0);
  static_assert(result::template get<u8, dc::chunk_round_shift> == 0);
  static_assert(result::template get<u8, dc::phase_shift> == phase);
  static_assert(result::template test<dc::observes_bit> == (Sticky || Observe));
  static_assert(result::template test<dc::stopped_bit> == (Sticky || !Continue));
  static_assert(result::template test<dc::has_more_bit> ==
                (Sticky || phase == dc::phase_more));
  static_assert(result::template test<dc::boundary_bit> ==
                (Sticky || phase == dc::phase_more));
  static_assert(result::template get<u16, dc::policy_shift> == 65535);
  static_assert(result::template get<std::uint32_t, dc::user_shift> ==
                ~std::uint32_t{0});
  return true;
}

static_assert(check_round<0, 0, 0, false, true>());
static_assert(check_round<0, 1, 255, true, false>());
static_assert(check_round<1, 255, 1, true, true>());
static_assert(check_round<255, 0, 255, false, false>());
static_assert(check_round<1, 1, 0, false, true, true>());

using full_flags = flags_operator<u128{~u64{0}, ~u64{0}}>;
using no_decision = typename meta_states_looper_details::default_driver_policy<1>
    ::decide<full_flags, dc::input<void, dc::window_decide>>::type;
static_assert(std::is_same_v<no_decision, full_flags>);
using stopped_flags = typename meta_states_looper_details::default_driver_policy<1>
    ::decide<full_flags, dc::input<void, dc::window_decide, stop_t>>::type;
static_assert(stopped_flags::value ==
    ((full_flags::value & ~full_flags::mask<u8, dc::phase_shift>) |
     (u128{dc::phase_done} << dc::phase_shift)));

using normalized_skip = meta_states_looper_details::normalize_decision<
    skip_t, full_flags, u128{1} << 127, 0>;
static_assert(normalized_skip::value == (full_flags::value & ~(u128{1} << 127)));
using normalized_yield = meta_states_looper_details::normalize_decision<
    yield_t, flags_operator<0>, 0, u128{1} << 127>;
static_assert(normalized_yield::value == (u128{1} << 127));

struct timer_policy : meta_states_looper_details::default_driver_policy<64> {
  template <class Flags, class Input>
  struct round {
    using base =
        typename meta_states_looper_details::default_driver_policy<64>::
            template round<Flags, Input>;
    static constexpr bool first =
        (Flags::template get<
             u16, meta_states_looper_details::driver_control::pc_shift> == 0);
    using type = std::conditional_t<
        first,
        typename base::type::template set<
            u8, meta_states_looper_details::driver_control::timer0_shift, 3>,
        typename base::type>;
    static constexpr u128 value = type::value;
  };
};

using timed_t = meta_states::run_with_policy<
    meta_ios::stream_states_co<>, meta_iterator,
    index_sequence_istream<3>, timer_policy>;

int run() {
  int loop_calls = 0;
  loop_t::for_each([&](auto stage) {
    using driver = typename decltype(stage)::driver;
    static_assert(driver::flags::template get<
                      u8, meta_states_looper_details::driver_control::phase_shift> ==
                  meta_states_looper_details::driver_control::phase_run);
    assert((driver::flags::template get<
                u8, meta_states_looper_details::driver_control::remaining_shift> ==
            63 - (u8)stage.index()));
    assert((driver::flags::template get<
                u16, meta_states_looper_details::driver_control::pc_shift> ==
            stage.index()));
    ++loop_calls;
  });
  assert(loop_calls == 4);

  int pipe_calls = 0;
  pipe_t::for_each([&](auto stage) {
    using driver = typename decltype(stage)::driver;
    static_assert(driver::flags::template get<
                      u8, meta_states_looper_details::driver_control::phase_shift> ==
                  meta_states_looper_details::driver_control::phase_run);
    assert((driver::flags::template get<
                u8, meta_states_looper_details::driver_control::remaining_shift> ==
            50 - (u8)stage.index()));
    ++pipe_calls;
  });
  assert(pipe_calls == 3);

  const int expected_timer[] = {0, 3, 2};
  int timer_round = 0;
  timed_t::for_each([&](auto stage) {
    using driver = typename decltype(stage)::driver;
    assert((driver::flags::template get<
                u8, meta_states_looper_details::driver_control::timer0_shift> ==
            expected_timer[timer_round]));
    ++timer_round;
  });
  assert(timer_round == 3);

  return 0;
}

}  // namespace driver_flags_tests

int main() { return driver_flags_tests::run(); }
