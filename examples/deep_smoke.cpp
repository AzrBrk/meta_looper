#include "meta_ios.hpp"

#include <cstddef>
#include <iostream>
#include <type_traits>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;

namespace deep_tests {

// 用 index_sequence_istream：源是递减整数，不是类型列表，干扰最小。
inline constexpr std::size_t deep_count = 2000;

// ---- 1) 裸 looper：type / for_each / 最后一个回调的返回值 ----
using loop_deep =
    meta_states::run<meta_ios::stream_states_co<>, meta_iterator,
                     index_sequence_istream<deep_count>>;

static_assert(loop_deep::type::to_t::value == deep_count - 1);

constexpr int loop_calls() {
  int calls = 0;
  loop_deep::for_each([&](auto) { ++calls; });
  return calls;
}
static_assert(loop_calls() == static_cast<int>(deep_count));

constexpr int loop_last() {
  return loop_deep::for_each([](auto s) { return static_cast<int>(s.index()); });
}
static_assert(loop_last() == static_cast<int>(deep_count - 1));

// ---- 2) 单节点 pipe ----
using pipe_one =
    meta_pipe<index_sequence_istream<deep_count>>::run<meta_iterator>;

static_assert(pipe_one::type::to_t::value == deep_count - 1);

constexpr int pipe_one_calls() {
  int calls = 0;
  pipe_one::for_each([&](auto) { ++calls; });
  return calls;
}
static_assert(pipe_one_calls() == static_cast<int>(deep_count));

constexpr int pipe_one_last() {
  return pipe_one::for_each([](auto s) { return static_cast<int>(s.index()); });
}
static_assert(pipe_one_last() == static_cast<int>(deep_count - 1));

// ---- 3) 两节点 pipe（对齐迭代器） ----
using pipe_two = meta_pipe<index_sequence_istream<deep_count>>
                     ::all_to<meta_aligned_iterator<>>
                     ::run<meta_iterator>;

static_assert(pipe_two::type::to_t::type::value == deep_count - 1);

constexpr int pipe_two_calls() {
  int calls = 0;
  pipe_two::for_each([&](auto) { ++calls; });
  return calls;
}
static_assert(pipe_two_calls() == static_cast<int>(deep_count));

// ---- 4) 深流 + wait_for_end：只回调最后冲刷的那一次 ----
using pipe_wait = meta_pipe<index_sequence_istream<deep_count>>
                      ::all_to<meta_iterator>
                      ::all_to<meta_iterator, con<wait_for_end_con<>>>
                      ::run<meta_ostream<>>;

static_assert(exp_utilities::exp_size<typename pipe_wait::type::to_t> == 1);

constexpr int pipe_wait_calls() {
  int calls = 0;
  pipe_wait::for_each([&](auto) { ++calls; });
  return calls;
}
static_assert(pipe_wait_calls() == 1);

// ---- 5) 深流 + filter：隔一个放行，跨 chunk 的 skip / 返回值 ----
// 隔一个放行：偶数轮 PIPE_EMIT 置位，奇数轮清位。REC/OBS 由 node_step_cond
// 的 !HasValue 规则自动处理（吞掉的轮次既不收也不观察）。
struct even_filter {
  template <meta_core64::u8 Window, meta_core64::u64 L, meta_core64::u64 U,
            class Obj>
  static constexpr meta_core64::u64 value = [] {
    meta_core64::u64 w = meta_stream64::ops::yield(U);
    if constexpr (Window == meta_states::post_rec_exec) {
      const meta_core64::u32 pos = meta_core64::lw::pos_v(L);
      const bool keep = (pos % 2) == 0;
      const meta_core64::u64 user = meta_core64::uw::user_v(w);
      const meta_core64::u64 emit = meta_stream_details::pipe_emit_bit;
      w = meta_core64::uw::with_user(
          w, (user & ~emit) |
                 meta_core64::select64(keep, emit, meta_core64::u64{0}));
    }
    return w;
  }();
};

using pipe_filter = meta_pipe<index_sequence_istream<deep_count>>
                        ::all_to<meta_iterator, con<node_con<even_filter, true>>>
                        ::run<meta_iterator>;

// 最终 receiver 用 meta_iterator：只看最后一个元素，不构造 1000 项的
// exp_list；大列表现在也能编译（pop_back 已用 take 优化），但没必要。
static_assert(pipe_filter::type::to_t::value == deep_count - 2);

constexpr int pipe_filter_calls() {
  int calls = 0;
  pipe_filter::for_each([&](auto) { ++calls; });
  return calls;
}
static_assert(pipe_filter_calls() == static_cast<int>(deep_count / 2));

constexpr int pipe_filter_last() {
  return pipe_filter::for_each(
      [](auto s) { return static_cast<int>(s.index()); });
}
// 最后一个偶数是 deep_count - 2。
static_assert(pipe_filter_last() == static_cast<int>(deep_count - 2));

}  // namespace deep_tests

int main() {
  std::cout << "deep pipe/looper chunking ok\n";
  return 0;
}
