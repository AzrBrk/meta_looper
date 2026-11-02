#include "meta_function_binder.hpp"
#include "meta_ios.hpp"

#include <cstddef>
#include <cstdio>
#include <type_traits>

namespace fork_bind_tests {

struct arguments {
  using first = int;
  using second = double;
};

using aligned = meta_iostream::meta_aligned_iterator<>;
using first_cursor = typename meta_invoke<aligned, int>::type;

struct first_lane {
  template <class Frame>
  using initialize = typename meta_invoke<aligned, typename Frame::first>::type;

  template <class Cursor, class Frame>
  using apply = typename meta_invoke<
      typename aligned::template meta_set<Cursor>, typename Frame::first>::type;
};

struct second_lane {
  template <class Frame>
  using initialize = typename meta_invoke<
      typename aligned::template meta_set<first_cursor>, typename Frame::second>::type;

  template <class Cursor, class Frame>
  using apply = typename meta_invoke<
      typename aligned::template meta_set<Cursor>, typename Frame::second>::type;
};

template <std::size_t Count>
struct receive_count {
  template <class Window, class Flags, class Object>
  struct apply {
    static constexpr auto value = [] {
      if constexpr (Window::value == meta_states::pre_gen_exec &&
                    meta_states::user_fn::index_of<Flags>() >= Count)
        return Flags::template opDeact<meta_states::G_CONTINUE>::value;
      else
        return Flags::value;
    }();
  };
};

template <std::size_t Count>
struct wait_main {
  template <class Flags, class Receiver>
  struct apply {
    static constexpr auto value = [] {
      if constexpr (Flags::value.high == 0) {
        using cursor = typename Receiver::type;
        static_assert(cursor::value == (Count - 1) * sizeof(int));
        return Flags::template opDeact<meta_states::G_CONTINUE>::value;
      } else
        return Flags::value;
    }();
  };
};

using source = meta_iostream::meta_repeat_istream<arguments>;
template <std::size_t Count>
using counted_pipeline = typename meta_ios::meta_pipe<source>
    ::all_to<meta_objects::meta_object<first_lane>,
        meta_ios::con<meta_ios::node_con<receive_count<Count>, true>>>
    ::template fork<meta_objects::meta_object<second_lane>,
       meta_ios::con<meta_ios::node_con<receive_count<Count>>>>
    ::template run<void, void,
          meta_ios::main_con<meta_objects::meta_states_object<
              meta_states::condition_seed, wait_main<Count>>>>;

using pipeline = counted_pipeline<1>;

using final_view = typename pipeline::type;
static_assert(final_view::forked);
static_assert(std::is_same_v<typename final_view::main_t::type, int>);
static_assert(std::is_same_v<typename final_view::branch_t::type, double>);
static_assert(final_view::main_t::value == 0);
static_assert(final_view::branch_t::value ==
              ((sizeof(int) + alignof(double) - 1) & ~(alignof(double) - 1)));

constexpr bool static_schedule() {
  unsigned callbacks{};
  pipeline::for_each([&](auto view) {
    using stage = decltype(view);
    static_assert(stage::main_t::value == final_view::main_t::value);
    static_assert(stage::branch_t::value == final_view::branch_t::value);
    ++callbacks;
  });
  return callbacks == 1;
}
static_assert(static_schedule());

constexpr bool two_round_schedule() {
  unsigned callbacks{};
  counted_pipeline<2>::for_each([&](auto view) {
    using stage = decltype(view);
    if constexpr (stage::main_t::value == sizeof(int)) {
      static_assert(stage::branch_t::value == final_view::branch_t::advance_t::value);
    }
    ++callbacks;
  });
  return callbacks == 2;
}
static_assert(two_round_schedule());

template <bool Strict>
double fork_write(int first, double second) {
  auto binder = meta_bind::bind([](int left, double right) {
    return left + right;
  });
  binder.bind_a_lot(0, 0.0);
  auto storage = binder.live_arguments();
  pipeline::for_each([&](auto view) {
    if constexpr (Strict) {
      view.main().get(storage.data()) = first;
      view.branch().get(storage.data()) = second;
    } else {
        using stage = decltype(view);
        *reinterpret_cast<typename stage::main_t::type*>(
          view.main().address_of(storage.data())) = first;
        *reinterpret_cast<typename stage::branch_t::type*>(
          view.branch().address_of(storage.data())) = second;
    }
  });
  return binder();
}

bool run() {
  auto binder = meta_bind::bind([](int first, double second) {
    return first + second;
  });
  bool rejected = false;
  try {
    static_cast<void>(binder.live_arguments());
  } catch (meta_bind::exception const& error) {
    rejected = error.code() == meta_bind::error::not_fully_bound;
  }
  binder.bind_a_lot(0, 0.0);
  auto storage = binder.live_arguments();
  unsigned writes{};
  pipeline::for_each([&](auto view) {
    using stage = decltype(view);
    using layout = type_safe_aligned::type_safe_aligned_range<0, int, double>;
    using left = type_safe_aligned::slot_cursor_t<0, layout>;
    using right = type_safe_aligned::slot_cursor_t<1, layout>;
    static_assert(std::is_same_v<typename stage::main_t, left>);
    static_assert(std::is_same_v<typename stage::branch_t, right>);
    *reinterpret_cast<typename stage::main_t::type*>(
      view.main().address_of(storage.data())) = 21;
    *reinterpret_cast<typename stage::branch_t::type*>(
      view.branch().address_of(storage.data())) = 0.5;
    writes += 2;
  });
  bool ok = rejected && writes == 2 && binder.bound_count() == 2 && binder.current_index() == 2;
  ok = ok && binder() == 21.5;
  binder.reset_argc_stack();
  pipeline::for_each([&](auto view) {
    using stage = decltype(view);
    *reinterpret_cast<typename stage::main_t::type*>(
      view.main().address_of(storage.data())) = 7;
    *reinterpret_cast<typename stage::branch_t::type*>(
      view.branch().address_of(storage.data())) = 1.25;
  });
  ok = ok && binder.current_index() == 0 && binder() == 8.25;

  return ok;
}

}

extern "C" double fork_write_strict(int first, double second) {
  return fork_bind_tests::fork_write<true>(first, second);
}

extern "C" double fork_write_raw(int first, double second) {
  return fork_bind_tests::fork_write<false>(first, second);
}

int main() {
  bool ok = fork_bind_tests::run() &&
            fork_write_strict(21, 0.5) == 21.5 &&
            fork_write_raw(7, 1.25) == 8.25;
  std::printf("fork_bind_smoke: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}