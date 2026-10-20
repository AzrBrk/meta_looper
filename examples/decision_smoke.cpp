#include "meta_ios.hpp"

#include <iostream>
#include <type_traits>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;

namespace decision_tests {

// ---- 1) pipe filter：skip / yield 决定，不碰 G_CONTINUE ----
template <bool IsPostGen, class Flags, class Object>
struct even_step {
  using type = Flags;
};
template <class Flags, class Object>
struct even_step<true, Flags, Object> {
  static constexpr bool keep =
      (Flags::template get<u16, position_shift> % 2) == 0;
  using type = std::conditional_t<keep, yield_t, skip_t>;
};

struct even_con {
  template <class Window, class Flags, class Object>
  using apply = typename even_step<Window::value == post_gen_exec, Flags,
                                   Object>::type;
};

using filter_pipe = meta_pipe<index_sequence_istream<10>>
                        ::all_to<meta_iterator, con<node_con<even_con, true>>>
                        ::run<meta_iterator>;

static_assert(filter_pipe::type::to_t::value == 8);

constexpr int filter_calls() {
  int calls = 0;
  filter_pipe::for_each([&](auto) { ++calls; });
  return calls;
}
static_assert(filter_calls() == 5);

// ---- 2) pipe stop：第 4 轮前结束，只发 0..3 ----
struct stop_step {
  template <class Window, class Flags, class Object>
  using apply = std::conditional_t<
      (Flags::template get<u16, position_shift> >= 4), stop_t, Flags>;
};

using stop_pipe = meta_pipe<index_sequence_istream<10>>
                      ::all_to<meta_iterator, con<node_con<stop_step, true>>>
                      ::run<meta_iterator>;

static_assert(stop_pipe::type::to_t::value == 3);

constexpr int stop_calls() {
  int calls = 0;
  stop_pipe::for_each([&](auto) { ++calls; });
  return calls;
}
static_assert(stop_calls() == 4);

// ---- 3) looper：同一个 stream_states_co 条件里用 skip / yield ----
template <bool IsPreRec, class Flags, class Object>
struct even_loop_step {
  using type = Flags;
};
template <class Flags, class Object>
struct even_loop_step<true, Flags, Object> {
  static constexpr bool keep =
      (Flags::template get<u16, position_shift> % 2) == 0;
  using type = std::conditional_t<keep, yield_t, skip_t>;
};

struct even_loop_fn {
  template <class Window, class Flags, class Object>
  using apply = typename even_loop_step<Window::value == pre_rec_exec, Flags,
                                        Object>::type;
};

using loop_t =
    meta_states::run<meta_ios::stream_states_co<even_loop_fn>, meta_iterator,
                     index_sequence_istream<10>>;

static_assert(loop_t::type::to_t::value == 8);

constexpr int loop_calls() {
  int calls = 0;
  loop_t::for_each([&](auto) { ++calls; });
  return calls;
}
static_assert(loop_calls() == 5);

// ---- 3b) looper stop：在 post_rec 第 4 轮结束时停（已接收 0..4） ----
struct stop_loop_fn {
  template <class Window, class Flags, class Object>
  using apply = std::conditional_t<
      (Window::value == post_rec_exec &&
       Flags::template get<u16, position_shift> >= 4),
      stop_t, Flags>;
};

using loop_stop =
    meta_states::run<meta_ios::stream_states_co<stop_loop_fn>, meta_iterator,
                     index_sequence_istream<10>>;

static_assert(loop_stop::type::to_t::value == 4);

constexpr int loop_stop_calls() {
  int calls = 0;
  loop_stop::for_each([&](auto) { ++calls; });
  return calls;
}
static_assert(loop_stop_calls() == 4);
// 注：post_rec 的 stop 让这一轮的 _continue_ 为 false，因此本轮不触发回调；
// 但 result 已经接收，最终 to_t 是 4。

// ---- 4) 函数形式与类型形式等价 ----
static_assert(std::is_same_v<decltype(meta_states::skip()),
                             meta_states::skip_t>);
static_assert(std::is_same_v<decltype(meta_states::yield()),
                             meta_states::yield_t>);
static_assert(std::is_same_v<decltype(meta_states::stop()),
                             meta_states::stop_t>);

}  // namespace decision_tests

int main() {
  std::cout << "decision smoke: skip/yield/stop ok\n";
  return 0;
}
