#include "meta_ios.hpp"

#include <cstddef>
#include <iostream>
#include <type_traits>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;
using meta_objects::meta_produce_t;

namespace pipe_tests {

// ---- 1) 最基础：类型列表 -> 对齐迭代器 -> 收集偏移 ----
using offsets = meta_pipe<meta_istream_list<int, double, char>>
                    ::all_to<meta_aligned_iterator<>>
                    ::run<meta_ostream<>>
                    ::type::to_t;

using it0 = meta_invoke<meta_aligned_iterator<>, int>;
using it1 = meta_invoke<it0, double>;
using it2 = meta_invoke<it1, char>;

static_assert(std::is_same_v<offsets, exp_list<typename it0::type,
                                               typename it1::type,
                                               typename it2::type>>);

// ---- 2) 反向收集：run<meta_rostream<>>::type::to::type ----
using rev_offsets = meta_pipe<meta_istream_list<int, double, char>>
                        ::all_to<meta_aligned_iterator<>>
                        ::run<meta_rostream<>>
                        ::type::to::type;

static_assert(std::is_same_v<rev_offsets, exp_list<typename it2::type,
                                                   typename it1::type,
                                                   typename it0::type>>);

// ---- 3) 串两段（普通 all_to）：rostream 每轮吐出整个累积列表 ----
using chained = meta_pipe<meta_istream_list<int, double, char>>
                    ::all_to<meta_aligned_iterator<>>
                    ::all_to<meta_rostream<>>
                    ::run<meta_ostream<>>;
static_assert(exp_utilities::exp_size<typename chained::type::to_t> == 3);

// ---- 4) wait_for_end：接收不假设输入（快照模型）——下游每轮收上游当前
//        累积快照，轮 0-2 各收一次（1/2/3 元素），轮 3 上游死 → 默认 con
//        stop_on_finish 在 pre_rec 清 G_CALL_RECEIVER，不再收。----
using reversed_aligned = typename meta_rostream<>::template meta_set<
    exp_list<typename it2::type, typename it1::type, typename it0::type>>;

using held = meta_pipe<meta_istream_list<int, double, char>>
                 ::all_to<meta_aligned_iterator<>>
                 ::all_to<meta_rostream<>, con<wait_for_end_con<>>>
                 ::run<meta_ostream<>>;
using held_t = typename held::type::to_t;
static_assert(exp_utilities::exp_size<held_t> == 3);
static_assert(std::is_same_v<
              held_t,
              exp_list<exp_list<typename it0::type>,
                       exp_list<typename it1::type, typename it0::type>,
                       exp_list<typename it2::type, typename it1::type,
                                typename it0::type>>>);

// ---- 5) 没有 From ----
using no_from = meta_pipe<>::run<index_sequence_istream<3>>;
static_assert(std::is_same_v<typename no_from::type::to_t,
                             index_sequence_istream_detail::index_counter<0>>);

// ---- 6) filter：用 PIPE_EMIT 跳元素，但节点不死亡 ----
template <bool IsPostGen, class Flags, class Object>
struct filter_step {
  using type = Flags;
};
template <class Flags, class Object>
struct filter_step<true, Flags, Object> {
  using type = std::conditional_t<
      std::is_same_v<int, meta_produce_t<Object>>,
      typename Flags::template opSet<G_CALL_RECEIVER | G_NOT_SKIP |
                                     meta_stream_details::pipe_emit_bit>,
      typename Flags::template opDeact<G_CALL_RECEIVER | G_NOT_SKIP |
                                       meta_stream_details::pipe_emit_bit>>;
};

struct filter_int {
  template <class Window, class Flags, class Object>
  using apply = typename filter_step<Window::value == post_gen_exec, Flags,
                                     Object>::type;
};

using filtered = meta_pipe<meta_istream_list<int, double, int, char>>
                     ::all_to<meta_iterator, con<node_con<filter_int, true>>>
                     ::run<meta_ostream<>>;
// 接收不假设输入：下游每轮收上游快照（G_CALL_RECEIVER 门控）。
// filter 滤掉的轮 filter 节点不更新 → 快照保持旧值 → 下游重复收到。
// 轮 0-3 各收一次（int / 旧 int / int / 旧 int），轮 4 上游死 → 默认 con
// stop_on_finish 在 pre_rec 清 G_CALL_RECEIVER 不再收。
static_assert(std::is_same_v<typename filtered::type::to_t,
                             exp_list<int, int, int, int>>);

// ---- 7) 中间节点死亡：本轮传完最终结果后 pipe 停 ----
struct kill_at_one {
  template <class Window, class Flags, class Object>
  using apply = std::conditional_t<
      Window::value == pre_gen_exec && user_fn::index_of<Flags>() == 1,
      typename Flags::template opDeact<G_CONTINUE>, Flags>;
};

using stopped = meta_pipe<meta_istream_list<int, double, int>>
                    ::all_to<meta_aligned_iterator<>>
                    ::all_to<meta_iterator, con<node_con<kill_at_one, false>>>
                    ::run<meta_ostream<>>;
static_assert(exp_utilities::exp_size<typename stopped::type::to_t> == 1);

// ---- 8) 运行时：回调次数只受 G_CONTINUE / G_NOT_SKIP 门控 ----
// wait_for_end 活着时不发值，但下游 G_NOT_SKIP 置 → 空转轮也回调；
// 冲刷那一轮下游同轮死亡（stop_on_finish 清 G_CONTINUE）→ 不回调。
constexpr bool check_wait_once() {
  unsigned calls = 0;
  held::for_each<protocols::stream_to_t>([&]<class TL>(TL) { ++calls; });
  return calls == 3;
}
static_assert(check_wait_once());

constexpr bool check_filter() {
  unsigned calls = 0;
  filtered::for_each<protocols::stream_to_t>(
      [&]<class TL>(TL) { ++calls; });
  // 回调只受 G_CONTINUE / G_NOT_SKIP 门控：下游（输出节点）G_NOT_SKIP 恒置，
  // filter 滤掉的轮也回调（view 与上一轮相同）。源 4 元素 + 1 耗尽轮 = 4 次。
  return calls == 4;
}
static_assert(check_filter());

}  // namespace pipe_tests

int main() {
  using namespace pipe_tests;

  int count = 0;
  meta_pipe<meta_istream_list<int, double, char>>
      ::all_to<meta_aligned_iterator<>>
      ::run<meta_ostream<>>::for_each([&]<class V>(V view) {
        std::cout << "index " << view.index() << " value "
                  << V::from_t::value << '\n';
        ++count;
      });
  std::cout << "pipe callbacks: " << count << " (expect 3)\n";

  unsigned held_calls = 0;
  held::for_each<protocols::stream_to_t>([&]<class TL>(TL) { ++held_calls; });
  std::cout << "wait_for_end callbacks: " << held_calls << " (expect 3)\n";

  return (count == 3 && held_calls == 3) ? 0 : 1;
}
