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

namespace ownership_tests {
namespace windows = meta_states_looper_details::meta_states_condition_funtion_detail;

struct overwrite_core {
  template<class Window, class Flags, class Object>
  struct apply {
    static constexpr u64 visits = Flags::template get<u64, 64>;
    static_assert(Window::value == (pre_gen_exec << (visits % 4)));
    static_assert(user_fn::index_of<Flags>() == visits / 4);
    using counted = typename Flags::template inc<u64, 64>;
    using poisoned = typename counted::template set<u16, position_shift, 65535>
        ::template set<u8, user_shift, 0xF5>;
    static constexpr auto value = [] {
      if constexpr (user_fn::index_of<Flags>() >= 3) {
        return poisoned::template opDeact<G_CONTINUE>::value;
      } else {
        return poisoned::value;
      }
    }();
  };
};

using corrupt_seed = typename flags_operator<condition_seed>
    ::template set<u8, user_shift, 0xF0>;
using raw_loop = meta_states::run<
    make_condition<overwrite_core, corrupt_seed::value>, meta_iterator,
    index_sequence_istream<4>>;
static_assert(raw_loop::type::states::template get<u16, position_shift> == 4);
static_assert((raw_loop::type::states::template get<u8, user_shift> & 0x0F) == 5);
static_assert(raw_loop::type::states::template get<u64, 64> == 16);

struct overwrite_stream_core {
  template<class Window, class Flags, class Object>
  using apply = typename Flags::template set<u16, position_shift, 65535>
      ::template set<u8, user_shift, 0xFF>
      ::template opSet<(u128{1} << 100)>;
};

using guarded_stream = meta_ios::stream_states_co<overwrite_stream_core>;
using stream_loop = meta_states::run<guarded_stream, meta_iterator,
                                      index_sequence_istream<4>>;
using no_gen_loop = meta_states::run<guarded_stream, meta_istream_list<int, char>>;
using needs_input_loop = meta_states::run<guarded_stream, meta_aligned_iterator>;
using guarded_pipe = meta_pipe<index_sequence_istream<4>>::run<
    meta_iterator, con<node_con<overwrite_stream_core, true>>>;
using long_loop = meta_states::run<guarded_stream, meta_iterator,
                                    index_sequence_istream<100>>;
using long_pipe = meta_pipe<index_sequence_istream<100>>::run<
    meta_iterator, con<node_con<overwrite_stream_core, true>>>;
static_assert(stream_loop::type::states::template test<(u128{1} << 100)>);
static_assert((no_gen_loop::type::states::template get<u8, user_shift> & 0x0F) ==
              meta_ios_details::no_generator_state);
static_assert(needs_input_loop::type::states::template get<u16, position_shift> == 1);
static_assert(guarded_pipe::type::to_t::value == 3);

template<u8 FinishWindow>
struct finish_and_overwrite {
  template<class Window, class Flags, class Object>
  struct apply {
    using poisoned = typename Flags::template set<u16, position_shift, 65535>
        ::template set<u8, user_shift, 0xFF>;
    using armed_by_user = override_suppression_timer<
        poisoned, 0, G_NOT_SKIP, 3>;
    static constexpr auto value = armed_by_user::value |
        (Window::value == FinishWindow ? u128{G_FINISH} : u128{0});
  };
};

template<u8 Window>
consteval bool check_finish() {
  using armed = set_suppression_timer<flags_operator<condition_seed>,
                                      1, G_NOT_SKIP, 2>;
  using step = meta_states_looper_details::meta_states_looper_step<
      make_condition<finish_and_overwrite<Window>, armed::value>,
      meta_iterator, index_sequence_istream<4>>;
  using flags = typename step::condition::flags;
  static_assert(flags::template get<u16, position_shift> == 1);
  static_assert((flags::template get<u8, user_shift> & stage_index_mask) ==
                pre_gen_exec);
  static_assert(timer_count<flags, 1> == 1);
  static_assert(timer_count<flags, 0> == 2);
  static_assert(!flags::template test<G_FINISH>);
  return true;
}
static_assert(check_finish<pre_gen_exec>());
static_assert(check_finish<post_gen_exec>());
static_assert(check_finish<pre_rec_exec>());
static_assert(check_finish<post_rec_exec>());

struct overwrite_main_core {
  template<class Flags, class Object>
  struct apply {
    using poisoned = typename Flags::template set<u16, position_shift, 65535>
        ::template set<u64, 64, 0>
        ::template opSet<(u128{1} << 19)>;
    static constexpr auto value = [] {
      if constexpr (user_fn::index_of<Flags>() >= 3) {
        return poisoned::template opDeact<G_CONTINUE>::value;
      } else {
        return poisoned::value;
      }
    }();
  };
};
using main_pipe = meta_pipe<meta_index_istream<0>>::run<
    meta_iterator, con<node_con<>>, main_con<
        meta_states_object<condition_seed, overwrite_main_core>>>;
static_assert(main_pipe::type::states::template get<u16, position_shift> == 3);
static_assert(main_pipe::type::states::template get<u64, 64> == 1);
static_assert(main_pipe::type::states::template test<(u128{1} << 19)>);

using reset_snapshot = flags_operator<u128{condition_seed | 23, 123}>;
using reset_result = meta_states_looper_details::restore_owned_flags<
    windows::scheduler_owned_mask, reset_snapshot, flags_operator<0>>;
static_assert(reset_result::value ==
              (reset_snapshot::value & windows::scheduler_owned_mask));
template<class Runner, unsigned Expected>
void check_callbacks() {
  unsigned calls = 0;
  Runner::for_each([&](auto stage) {
    assert(stage.index() == calls++);
  });
  assert(calls == Expected);
  calls = 0;
  Runner::for_each_forward([&](auto stage, auto value) {
    static_assert(stage.index() == decltype(value)::value);
    assert(stage.index() == calls++);
  }, std::integral_constant<unsigned, 0>{},
     std::integral_constant<unsigned, 1>{},
     std::integral_constant<unsigned, 2>{},
     std::integral_constant<unsigned, 3>{});
  assert(calls == Expected);
}

template<class Runner>
void check_long_callbacks() {
  unsigned calls = 0;
  Runner::for_each([&](auto stage) {
    assert(stage.index() == calls++);
  });
  assert(calls == 100);
}

void run() {
  check_callbacks<raw_loop, 3>();
  check_callbacks<stream_loop, 4>();
  check_callbacks<no_gen_loop, 2>();
  check_callbacks<needs_input_loop, 0>();
  check_callbacks<guarded_pipe, 4>();
  check_callbacks<main_pipe, 3>();
  check_long_callbacks<long_loop>();
  check_long_callbacks<long_pipe>();
}
}  // namespace ownership_tests

int run() {
  ownership_tests::run();
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
