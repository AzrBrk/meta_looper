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

