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

// ---- 4) wait_for_end：累积不假设输入——下游只在节点死亡那一轮收到一次
//        冲刷（1 个完整反向列表）。这是新双字 pipe 的 flush-once 语义
//        （见 test/pipe64_test.cpp §3 / examples/deep_smoke.cpp §4）。----
using held = meta_pipe<meta_istream_list<int, double, char>>
                 ::all_to<meta_aligned_iterator<>>
                 ::all_to<meta_rostream<>, con<wait_for_end_con<>>>
                 ::run<meta_ostream<>>;
using held_t = typename held::type::to_t;
static_assert(exp_utilities::exp_size<held_t> == 1);
static_assert(std::is_same_v<
              held_t,
              exp_list<exp_list<typename it2::type, typename it1::type,
                                typename it0::type>>>);

// ---- 5) 没有 From：无源 pipe，receiver 靠元对象系统自然推进 ----
// index_sequence_istream<3> 作为 receiver 每轮接收空投影（meta_empty）后
// 自递减 3→2→1→0。停止不靠引擎隐式规则（流不一定会 eol，meta_repeat_istream
// 等无限流就没有 eol），而是这个显式 condition 在窗口③（post_rec）检查
// receiver 的 produce 是否 eol 再 stop。
struct no_from_drain {
  template <meta_core64::u8 Window, meta_core64::u64 L, meta_core64::u64 U,
            class Obj>
  static constexpr meta_core64::u64 value = [] {
    meta_core64::u64 w = meta_stream64::ops::yield(U);
    if constexpr (Window == meta_states::post_rec_exec) {
      if constexpr (exp_utilities::eol<meta_produce_t<Obj>>) {
        w = meta_stream64::ops::stop(w);
      }
    }
    return w;
  }();
};

using no_from = meta_pipe<>::run<index_sequence_istream<3>,
                                 con<node_con<no_from_drain, true>>>;
static_assert(std::is_same_v<typename no_from::type::to_t,
                             index_sequence_istream_detail::index_counter<0>>);

// ---- 6) filter：用 PIPE_EMIT 跳元素，但节点不死亡 ----
// 旧条件在 post_gen_exec（窗口①）检查 produce 类型，机械迁移到新 Fn 后仍保持
// 窗口①：Obj 是 advance 后的 generator（gen1），meta_produce_t<Obj> 即本轮值。
// keep 重挂 PIPE_EMIT，drop 清；吞掉的轮次由 node_step_cond 的 !HasValue 连
// REC+OBS 一起清，下游收不到也观察不到。
struct filter_int {
  template <meta_core64::u8 Window, meta_core64::u64 L, meta_core64::u64 U,
            class Obj>
  static constexpr meta_core64::u64 value = [] {
    meta_core64::u64 w = meta_stream64::ops::yield(U);
    if constexpr (Window == meta_states::post_gen_exec) {
      constexpr bool keep = std::is_same_v<int, meta_produce_t<Obj>>;
      const meta_core64::u64 user = meta_core64::uw::user_v(w);
      const meta_core64::u64 emit = meta_stream_details::pipe_emit_bit;
      w = meta_core64::uw::with_user(
          w, (user & ~emit) |
                 meta_core64::select64(keep, emit, meta_core64::u64{0}));
    }
    return w;
  }();
};

using filtered = meta_pipe<meta_istream_list<int, double, int, char>>
                     ::all_to<meta_iterator, con<node_con<filter_int, true>>>
                     ::run<meta_ostream<>>;
// filter 吞掉非 int：下游只收 int。源 4 元素中 int 出现 2 次（int / double /
// int / char），所以下游累积 exp_list<int, int>。第 5 轮源死 → 默认 con
// stop_on_finish 在 window 2 清 REC，不再收。
static_assert(std::is_same_v<typename filtered::type::to_t,
                             exp_list<int, int>>);

// ---- 7) 中间节点死亡：本轮传完最终结果后 pipe 停 ----
struct kill_at_one {
  template <meta_core64::u8 Window, meta_core64::u64 L, meta_core64::u64 U,
            class Obj>
  static constexpr meta_core64::u64 value = [] {
    meta_core64::u64 w = meta_stream64::ops::yield(U);
    if constexpr (Window == meta_states::pre_gen_exec) {
      if (meta_core64::lw::pos_v(L) == 1) w = meta_stream64::ops::stop(w);
    }
    return w;
  }();
};

using stopped = meta_pipe<meta_istream_list<int, double, int>>
                    ::all_to<meta_aligned_iterator<>>
                    ::all_to<meta_iterator, con<node_con<kill_at_one, false>>>
                    ::run<meta_ostream<>>;
static_assert(exp_utilities::exp_size<typename stopped::type::to_t> == 1);

// ---- 8) 运行时：回调次数只受门位门控 ----
// wait_for_end 只在死亡那一轮冲刷一次 → 恰好 1 次回调（flush-once）。
constexpr bool check_wait_once() {
  unsigned calls = 0;
  held::for_each([&](auto) { ++calls; });
  return calls == 1;
}
static_assert(check_wait_once());

constexpr bool check_filter() {
  unsigned calls = 0;
  filtered::for_each([&](auto) { ++calls; });
  // filter 吞掉非 int：只有 2 个 int 轮被观察（node_step_cond 的 !HasValue
  // 连 REC+OBS 一起清，吞掉的轮次既收不到也不观察）。
  return calls == 2;
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
  held::for_each([&](auto) { ++held_calls; });
  std::cout << "wait_for_end callbacks: " << held_calls << " (expect 1)\n";

  return (count == 3 && held_calls == 1) ? 0 : 1;
}
