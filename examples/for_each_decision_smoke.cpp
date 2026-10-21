#define META_STATES_LOOPER_BUDGET 2

#include "meta_ios.hpp"

#include <cassert>
#include <type_traits>

using namespace meta_ios;
using namespace meta_iostream;
using namespace meta_states;

namespace for_each_decision_tests {

using loop_t =
    meta_states::run<meta_ios::stream_states_co<>, meta_iterator,
                     index_sequence_istream<4>>;

struct stop_cb {
  int* calls;
  template <class Stage>
  auto operator()(Stage) const {
    ++*calls;
    if constexpr (Stage{}.index() == 2) {
      return meta_states_control::stop_t{};
    } else {
      return 0;
    }
  }
};

struct stop_forward_cb {
  int* calls;
  template <class Stage>
  auto operator()(Stage, int) const {
    ++*calls;
    if constexpr (Stage{}.index() == 2) {
      return meta_states_control::stop_t{};
    } else {
      return 0;
    }
  }
};

struct skip_cb {
  int* calls;
  template <class Stage>
  auto operator()(Stage) const {
    ++*calls;
    if constexpr (Stage{}.index() == 1) {
      return meta_states_control::skip_t{};
    } else {
      return 0;
    }
  }
};

struct yield_cb {
  int* calls;
  template <class Stage>
  auto operator()(Stage) const {
    ++*calls;
    if constexpr (Stage{}.index() == 3) {
      return meta_states_control::yield_t{};
    } else {
      return 0;
    }
  }
};

using pipe_t = meta_pipe<index_sequence_istream<4>>::run<meta_ostream<>>;

int run() {
  int calls = 0;
  auto stopped = loop_t::for_each(stop_cb{&calls});
  assert(calls == 3);
  static_assert(std::is_same_v<decltype(stopped),
                               meta_states_control::stop_t>);

  calls = 0;
  auto skipped = loop_t::for_each(skip_cb{&calls});
  assert(calls == 4);
  static_assert(std::is_same_v<decltype(skipped), int>);

  calls = 0;
  auto yielded = loop_t::for_each(yield_cb{&calls});
  assert(calls == 4);
  static_assert(std::is_same_v<decltype(yielded),
                               meta_states_control::yield_t>);

  calls = 0;
  auto forwarded =
      loop_t::for_each_forward(stop_forward_cb{&calls}, 0, 1, 2, 3);
  assert(calls == 3);
  static_assert(std::is_same_v<decltype(forwarded),
                               meta_states_control::stop_t>);

  calls = 0;
  auto pipe_stopped = pipe_t::for_each(stop_cb{&calls});
  assert(calls == 3);
  static_assert(std::is_same_v<decltype(pipe_stopped),
                               meta_states_control::stop_t>);

  calls = 0;
  auto pipe_forwarded =
      pipe_t::for_each_forward(stop_forward_cb{&calls}, 0, 1, 2, 3);
  assert(calls == 3);
  static_assert(std::is_same_v<decltype(pipe_forwarded),
                               meta_states_control::stop_t>);

  return 0;
}

}  // namespace for_each_decision_tests

int main() { return for_each_decision_tests::run(); }
