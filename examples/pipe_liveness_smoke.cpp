// 节点 con 的生命周期控制权：
//   * 引擎只翻译状态：上游结束 → G_FINISH 事件、上游有值 → G_CALL_RECEIVER；
//   * 死不死只看 con 清不清 G_CONTINUE（just_died = !_continue_，引擎不判死）；
//   * 默认 con（stop_on_finish）看到 G_FINISH 在 post_rec 清 G_CONTINUE；
//   * 自定义 con 不理会 G_FINISH 就能在上游死后继续 drain，何时停自己定；
//   * 带 FLUSH 的节点死时把最终产物交给下游，且该产物持续可读；
//   * 中间节点死亡不再替下游停 pipe：停机只看 main 的存活位图。
//
// 这组用例把"第几步停"写成显式条件（dies_at_fn<N> / drain_until_fn<N>），
// 控制权在节点 con 手里，测试就把条件自己写出来。
#include "meta_ios.hpp"

#include <cstdio>
#include <type_traits>

using namespace meta_ios;
using namespace meta_iostream;
using namespace meta_states;

namespace pipe_liveness_tests {

// 上游死后继续活着：什么也不做（自定义 Fn，不隐含 stop_on_finish）。
struct never_dies_fn {
  template <class Window, class Flags, class Object>
  struct apply {
    static constexpr auto value = Flags::value;
  };
};

// 第 At 轮（0-based）在 post_rec_exec 清 G_CONTINUE：显式声明"我跑几轮"。
template <std::size_t At>
struct dies_at_fn {
  template <class Window, class Flags, class Object>
  struct apply {
    static constexpr bool kill =
        Window::value == post_rec_exec && user_fn::index_of<Flags>() == At;
    static constexpr u128 value = {
        Flags::value.low & ~(G_CONTINUE & (0 - u64{kill})),
        Flags::value.high};
  };
};

// 第 At 轮（0-based）在 pre_gen_exec 清 G_CONTINUE：窗口① 就表态，
// 之后本轮的接收门自然关掉（drain 到点就停，不再 invoke rec）。
// 接收不假设输入：rec 的 produce 已经是 end_of_list（自推进型 rec 收满）
// 时在 pre_rec 清 G_CALL_RECEIVER，不再 invoke。
template <std::size_t At>
struct drain_until_fn {
  template <class Window, class Flags, class Object>
  struct apply {
    static constexpr bool die =
        Window::value == pre_gen_exec && user_fn::index_of<Flags>() == At;
    static constexpr bool rec_done =
        Window::value == pre_rec_exec &&
        exp_utilities::eol<meta_objects::meta_produce_t<Object>>;
    static constexpr u128 value = {
        Flags::value.low &
            ~(G_CALL_RECEIVER & (0 - u64{rec_done})) &
            ~(G_CONTINUE & (0 - u64{die})),
        Flags::value.high};
  };
};

// con 高位 latch：pre_gen 置位（整体 bit66）、post_rec 检查 ——
// 编译能结束且 rounds == 1 = latch 跨窗口存活（高位归 con）。
inline constexpr u64 latch_bit = u64{1} << 2;  // 高位 word bit2 == 整体 bit66

struct latch_test_fn {
  template <class Window, class Flags, class Object>
  struct apply {
    static constexpr bool is_pre = Window::value == pre_gen_exec;
    static constexpr bool is_post = Window::value == post_rec_exec;
    static constexpr bool latched = (Flags::value.high & latch_bit) != 0;
    static constexpr u128 value = {
        Flags::value.low &
            ~(G_CONTINUE & (0 - u64{is_post && latched})),
        Flags::value.high | (latch_bit & (0 - u64{is_pre}))};
  };
};

using source = meta_istream_list<int, double, char, long>;
using sink = meta_ostream<>;

// 1) 对照：4 个元素 → 4 轮数据 + 1 轮耗尽，4 次回调。
using control = meta_pipe<source>::all_to<meta_iterator>::run<sink>;
static_assert(control::type::states::template get<u16, position_shift> == 5);

// 2) 源节点第 1 轮自杀 → 下游默认 con（stop_on_finish）看到 G_FINISH
//    同轮跟着死：2 轮。注意显式 node_con<> 不挂 stop_on_finish —— 谁显式
//    声明"自己管命运"，上游死了它也不会被引擎判死。
using source_dies = meta_pipe<source>::all_to<meta_iterator,
                                               con<node_con<dies_at_fn<1>, true>>>
    ::run<sink>;
static_assert(source_dies::type::states::template get<u16, position_shift> == 2);

// 3) 源节点第 1 轮自杀，下游自定义 con 不理会 G_FINISH，自己第 3 轮才停：
//    pipe 跑到 4 轮；回调 = 轮 0/1 两个真实值（上游无 FLUSH，死后不重读）。
using source_drain = meta_pipe<source>::all_to<meta_iterator,
                                               con<node_con<dies_at_fn<1>, true>>>
    ::run<sink, con<node_con<dies_at_fn<3>>>>;
static_assert(source_drain::type::states::template get<u16, position_shift> == 4);
static_assert(source_drain::type::states::template get<meta_objects::u64, 64> == 0);

// 4) 中间节点第 1 轮自杀：下游默认 con 看到 G_FINISH 同轮死，pipe 不停机
//    （停机只看 main 位图）→ 源节点继续把 4 元素 drain 完，5 轮。
using middle_dies =
    meta_pipe<source>::all_to<meta_iterator, con<node_con<never_dies_fn, true>>>
        ::all_to<meta_iterator, con<node_con<dies_at_fn<1>>>>::run<sink>;
static_assert(middle_dies::type::states::template get<u16, position_shift> == 5);

// 5) wait_for_end 死时 FLUSH 一次，最终产物持续可读：下游自定义 con 不理会
//    G_FINISH，按自己的节奏 drain（每轮 invoke rec），第 7 轮窗口① 才停。
//    源 3 元素：轮 0-2 上游收，轮 3 上游死 + FLUSH；节点 2 轮 3..6 drain 4 次
//    （index_counter<3..0>），轮 7 窗口① index==7 停机 → 4 次回调。
using drain_pipe = meta_pipe<meta_istream_list<int, double, char>>
                       ::all_to<meta_aligned_iterator<>>
                       ::all_to<meta_rostream<>, con<wait_for_end_con<>>>
                       ::all_to<index_sequence_istream<4>,
                                con<node_con<drain_until_fn<7>>>>
                       ::run<>;
static_assert(std::is_same_v<typename drain_pipe::type::to_t,
                             index_sequence_istream_detail::index_counter<0>>);
static_assert(drain_pipe::type::states::template get<u16, position_shift> == 8);

// 6) latch 位跨窗口存活：pre_gen 置 bit66，post_rec 看到 → 当轮停机。
using latch_pipe = meta_pipe<source>::all_to<meta_iterator,
                                             con<node_con<latch_test_fn, true>>>
    ::run<sink>;
static_assert(latch_pipe::type::states::template get<u16, position_shift> == 1);

constexpr bool check_callbacks() {
  unsigned control_calls = 0;
  control::for_each([&](auto) { ++control_calls; });
  unsigned dies_calls = 0;
  source_dies::for_each([&](auto) { ++dies_calls; });
  unsigned drain_calls = 0;
  source_drain::for_each([&](auto) { ++drain_calls; });
  unsigned middle_calls = 0;
  middle_dies::for_each([&](auto) { ++middle_calls; });
  unsigned drain_pipe_calls = 0;
  drain_pipe::for_each([&](auto) { ++drain_pipe_calls; });
  // 回调只受 G_CONTINUE / G_NOT_SKIP 门控（死亡轮不回调）：
  //   control     轮 0-3 输出节点活着 → 4
  //   source_dies 轮 0 活着（轮 1 跟着上游死）→ 1
  //   source_drain 轮 0-2 活着（轮 3 自杀）→ 3
  //   middle_dies 轮 0 活着（轮 1 跟着上游死）→ 1
  //   drain_pipe  轮 0-6 活着（轮 7 窗口① 自杀）→ 7
  return control_calls == 4 && dies_calls == 1 && drain_calls == 3 &&
         middle_calls == 1 && drain_pipe_calls == 7;
}
static_assert(check_callbacks());

}  // namespace pipe_liveness_tests

int main() {
  using namespace pipe_liveness_tests;
  const bool ok = check_callbacks();
  std::printf("control rounds=%u source_dies=%u source_drain=%u "
              "middle_dies=%u drain_pipe=%u latch=%u\n",
              static_cast<unsigned>(control::type::states::template get<u16, position_shift>),
              static_cast<unsigned>(source_dies::type::states::template get<u16, position_shift>),
              static_cast<unsigned>(source_drain::type::states::template get<u16, position_shift>),
              static_cast<unsigned>(middle_dies::type::states::template get<u16, position_shift>),
              static_cast<unsigned>(drain_pipe::type::states::template get<u16, position_shift>),
              static_cast<unsigned>(latch_pipe::type::states::template get<u16, position_shift>));
  std::puts(ok ? "pipe_liveness_smoke: ok" : "pipe_liveness_smoke: FAILED");
  return ok ? 0 : 1;
}
