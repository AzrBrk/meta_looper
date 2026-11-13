#pragma once

// ============================================================================
// meta_stream —— 基于 meta_states_looper 单步逻辑的编译期管道（meta_pipe）
//
// 旧 meta_stream 把 (To, From) 塞进一个 meta object，由那个对象自己维护
// stream_op 位、changed_pred、index 记账……控制逻辑长在对象身上。
// 这里改成：
//
//   * pipe 自己就是 looper：状态是一组扁平的 slot，每轮对它们做一次 fold；
//   * 每一段是一个节点 (rec, con)：con 只控制自己这一段的流程，不参与最终产物；
//   * 上一个节点的单步结果直接当下一段的 generator（链上中间没有 generator 槽）；
//   * 节点单步复用底层窗口原语，独立处理输入可用性和 emit/flush；
//     四窗口 / 计时器 / position / settle 语义保持一致；
//   * main 是第一个 con，负责全局状态：高 64 位是节点存活位图，低 64 位是
//     pipe 自己的 position；它决定整个 pipe 是否继续。
//
// 内部形态（输出 → 输入，build 时反过来写，和 ranges 一样）：
//
//   meta_pipe_impl<
//       main,
//       recN, conN,          // 最后一个节点（最终产物）
//       ...
//       rec0, con0,          // 第一个节点（挨着 gen）
//       gen>
//
// 每轮从 gen 往输出方向 fold：
//
//   node0: 输入 = gen（默认 con 的 G_CALL_GENERATOR 开，推进 gen）
//   nodek: 输入 = 上一个节点的 rec'（默认 con 的 G_CALL_GENERATOR 关，中间只有 receiver）
//   每个节点的 rec' 就是下一段的 generator。
//
// 每个节点 con 的 flags 里有两个 pipe 保留位（都在高 64 位，和 main 的位图
// 不冲突，因为它们各用各的 flags）：
//
//   PIPE_EMIT  (bit 64): 本轮收到值后是否往下游发（filter 把它清掉）
//   PIPE_FLUSH (bit 65): 节点死掉的那一轮是否把当前 rec 冲刷给下游
//                        （wait_for_end 用：活着时只收不发，死时发最终累积值）
//
// 结束规则：
//   * 非输出最末节点死亡 → 本轮照常把它的最终结果传下去，本轮结束后 pipe 停；
//   * 输出最末节点死亡 → 清存活位，以后不再调用它，main 按位图决定是否继续；
//   * main 默认在所有节点都死亡（位图 == 0）时停。
//
// 用户 API（公开在 meta_ios 里）：
//
//   meta_pipe<gen>
//     ::all_to<rec>                       // 默认 con
//     ::all_to<rec, con<C>>               // 显式指定 con（con 置后）
//     ::all_to<rec, con<wait_for_end_con<>>>  // 只收不发，死时冲刷
//     ::run<rec>                          // 最后一段 + 默认 main
//     ::run<rec, con<C>, main_con<M>>
//     ::run<...>::for_each / ::for_each<Ps...>
//     ::run<...>::type                    // pipe_view: to / to_t / from / states
// ============================================================================

#include "meta_stream_control.hpp"
#include "meta_iostream.hpp"

namespace meta_ios {

// 显式标记：con 永远放在 rec 后面。
template <class C>
struct con {};
template <class M>
struct main_con {};

}  // namespace meta_ios

namespace meta_stream_details {
using namespace exp_utilities;
using namespace literal_types;
using namespace meta_objects;
using namespace meta_states;
using namespace meta_states_looper_details;
using namespace meta_invoke_protocols;

// ============================================================================
// 1) pipe 自己的 flags 约定
// ============================================================================
//
// 每个节点 con 的 flags 高 64 位归 pipe 用：
//   bit 64 = PIPE_EMIT，bit 65 = PIPE_FLUSH。
// main 的 flags 高 64 位是节点存活位图（bit k = 节点 k 还活着，k 从输入端数），
// 因为 main 不是节点 con，两者各用各的 flags，不会撞。

inline constexpr u128 pipe_emit_bit = u128{1} << 64;
inline constexpr u128 pipe_flush_bit = u128{1} << 65;
// 引擎拥有的高 64 位段：bit 64/65 = emit/flush；66..127 归 con latch。
inline constexpr u128 pipe_engine_high_mask = pipe_emit_bit | pipe_flush_bit;
inline constexpr std::size_t pipe_mask_shift = 64;

// 默认 con 的隐式行为：看到 G_FINISH（上游结束）就收工 ——
//   pre_rec_exec 清 G_CALL_RECEIVER（上游结束了，不再 invoke 收尾）；
//   post_rec_exec 清 G_CONTINUE（本轮末停止）。
// 引擎不替节点判死，只把"上游结束"翻译成 G_FINISH 事件；默认节点把
// "上游结束了所以我也停"写在自己的 con 里。先跑用户 Fn，再响应 G_FINISH；
// 默认 Fn（unchanged）时整个组合 = 纯 stop_on_finish。显式自定义 Fn 的
// node_con 不挂这一层，看没看到 G_FINISH、停不停都是它自己的事（drain
// 就靠这个 —— 上游死后继续按 G_CALL_RECEIVER 收冲刷产物）。
template <class Fn>
struct stop_on_finish_or_fn {
  template <class Window, class Flags, class Object>
  struct apply {
    using inner = meta_invoke<Fn, Window, Flags, Object>;
    static constexpr bool finish =
        inner::template test<meta_states::G_FINISH>;
    static constexpr bool pre_rec_stop =
        Window::value == pre_rec_exec && finish;
    static constexpr bool post_rec_stop =
        Window::value == post_rec_exec && finish;
    static constexpr u128 value = {
        inner::value.low &
            ~(meta_states::G_CALL_RECEIVER & (0 - u64{pre_rec_stop})) &
            ~(meta_states::G_CONTINUE & (0 - u64{post_rec_stop})),
        inner::value.high};
  };
};

// 默认节点 con 的起始 flags：
//   ADVANCE 开：节点 0（它负责推进 gen）
//   ADVANCE 关：其余节点（中间没有 generator，只有 receiver）
//   EMIT 开、FLUSH 关
inline constexpr u128 node_seed_advance =
    u128{meta_states::condition_seed} | pipe_emit_bit;
inline constexpr u128 node_seed_static =
    u128{meta_states::condition_seed & ~meta_states::G_CALL_GENERATOR} | pipe_emit_bit;
// wait_for_end：活着时只接收不发射，死亡那一轮由引擎按 FLUSH 冲刷。
inline constexpr u128 node_seed_wait =
    u128{meta_states::condition_seed & ~meta_states::G_CALL_GENERATOR} | pipe_flush_bit;
inline constexpr u128 node_seed_wait_advance =
    u128{meta_states::condition_seed} | pipe_flush_bit;

// pipe 专用的流协议（fork stream_fn）。与共享 stream_fn 的唯一差别在窗口②：
// gen eol 只在"本轮推进了 gen（G_CALL_GENERATOR 置）"或"gen 初始就是空的
// （position==0）"时清 G_CONTINUE。支链的输入最端节点 advance 关、只借主线
// 源元素，源耗尽由 G_FINISH 事件（SourceDied）传达给 con，不该被流协议
// 直接判死 —— 生死只看 con 的门控位。
template <class Fn,
          meta_objects::meta_op_bits::u128 SkipMask =
              meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP,
          meta_objects::meta_op_bits::u128 YieldMask =
              meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP>
struct pipe_stream_fn {
  template <class Window, class Flags, class Object>
  struct apply {
    static constexpr auto controlled = [] {
      constexpr auto index = user_fn::index_of<Flags>();
      constexpr u128 flags = Flags::value;
      constexpr bool no_generator =
          ((Flags::value.low >> user_shift) & meta_ios_details::no_generator_state) != 0;

      // MSO 观察者收到完整对象；数据 MSO 的 flags 不属于流控制布局，
      // 不对它自动设置首轮延迟或判断流结束，交给用户条件决定。
      if constexpr (meta_states_object_t<Object> || !meta_object_t<Object>) {
        return flags;
      } else if constexpr (Window::value == pre_gen_exec) {
        if constexpr (meta_empty_object_t<Object>) {
          return (flags & ~u128{G_CALL_GENERATOR}) |
              (u128{meta_ios_details::no_generator_state} << user_shift);
        } else {
          constexpr u128 delay =
              u128{meta_ios_details::generator_delay_state} << user_shift;
          if ((flags.low & delay.low) != 0) {
            return (flags & ~delay) | u128{G_CALL_GENERATOR};
          }
          if constexpr (initialized_meta_object_t<Object> && index == 0) {
            // 只恢复自己关闭的推进门，不能开启 pipe 的中间节点或支链。
            if ((flags.low & G_CALL_GENERATOR) != 0) {
              return (flags & ~u128{G_CALL_GENERATOR}) | delay;
            }
          }
        }
      } else if constexpr (Window::value == post_gen_exec) {
        if constexpr (!meta_empty_object_t<Object> &&
                      exp_utilities::eol<meta_produce_t<Object>> &&
                      (((flags.low & G_CALL_GENERATOR) != 0) || index == 0)) {
          return flags & ~u128{G_CONTINUE};
        }
      } else if constexpr (Window::value == pre_rec_exec) {
        if constexpr (no_generator && uninitialized_meta_object_t<Object> &&
                      !initialize_details::no_arguments_initializable<typename Object::function>) {
          return (flags & ~u128{G_CONTINUE}) | receiver_init_failed_mask;
        } else if constexpr (no_generator && initialized_meta_object_t<Object> && index == 0) {
          return meta_loop::meta_states_looper_bits::details::arm_timer_value(
              flags, timer0_shift,
              (u16{1} << bits::timer_count_shift) | std::countr_zero(G_CALL_RECEIVER),
              false);
        }
      } else if constexpr (Window::value == post_rec_exec) {
        if constexpr (no_generator && exp_utilities::eol<meta_produce_t<Object>>) {
          return flags & ~u128{G_CONTINUE};
        }
      }
      return flags;
    }();

    using controlled_flags = meta_objects::flags_operator<controlled>;
    static constexpr auto value = [] {
      if constexpr ((controlled.low & meta_states::G_CONTINUE) != 0) {
        using raw = meta_invoke<Fn, Window, controlled_flags, Object>;
        // 用户 Fn 可以返回 skip_t / yield_t / stop_t；归一化成 flags 后
        // 再恢复基础控制拥有的字段，用户门位/计时器和高位保持可写。
        return meta_states_looper_details::restore_owned_value(
            meta_ios_details::stream_owned_mask, controlled,
            meta_states_looper_details::decision_value<raw>(
                controlled, SkipMask, YieldMask));
      } else {
        return controlled;
      }
    }();
  };
};

// 造一个 pipe 节点 con：用 pipe 自己的流协议（pipe_stream_fn），
// skip/yield 的归一化要连同 PIPE_EMIT 一起处理（节点 0 还带 ADVANCE）。
template <class Fn, u128 Flags>
using pipe_stream_co = meta_states_object<
    Flags, meta_states::state_fn<pipe_stream_fn<
              Fn,
              meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP | pipe_emit_bit,
              meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP |
                  pipe_emit_bit>>>;

template <bool Advance>
using default_node_con = pipe_stream_co<
    stop_on_finish_or_fn<meta_ios_details::unchanged>,
    Advance ? node_seed_advance : node_seed_static>;

// 用户自定义节点 con：Advance = true 时默认推进输入（节点 0），否则不推进。
// Fn 选择：默认/显式 unchanged → 隐式 stop_on_finish（看到 G_FINISH 在 post_rec
// 清 G_CONTINUE）；显式自定义 Fn → 完全接管，不组合 stop_on_finish ——
// 上游死后继续 drain 的 con 就靠"不理会 G_FINISH"。
template <class Fn, bool Advance>
struct node_con_fn {
  using type = Fn;
};
template <bool Advance>
struct node_con_fn<meta_ios_details::unchanged, Advance> {
  using type = stop_on_finish_or_fn<meta_ios_details::unchanged>;
};

template <class Fn = meta_ios_details::unchanged, bool Advance = false>
using node_con = pipe_stream_co<
    typename node_con_fn<Fn, Advance>::type,
    Advance ? node_seed_advance : node_seed_static>;

// wait_for_end：可以在中间段用（ADVANCE 关、EMIT 关、FLUSH 开）。
// 用在节点 0 时把 Advance 设成 true。上游结束 → stop_on_finish 让它死，
// 死亡那一轮由引擎按 FLUSH 冲刷最终产物。
template <class Fn = meta_ios_details::unchanged, bool Advance = false>
using wait_for_end_con =
    pipe_stream_co<stop_on_finish_or_fn<Fn>,
                   Advance ? node_seed_wait_advance : node_seed_wait>;

// 节点 0 用：wait_for_end 同时推进 gen。
template <class Fn = meta_ios_details::unchanged>
using wait_for_end_advance_con =
    pipe_stream_co<stop_on_finish_or_fn<Fn>, node_seed_wait_advance>;

// main 的默认 con：只看存活位图，还有节点活着就继续，全死就停。
struct pipe_main_fn {
  template <class Flags, class Rec>
  struct apply {
    static constexpr u64 mask = Flags::value.high;
    // mask == 0（所有节点都死了）时清 G_CONTINUE。
    // 用全 1/全 0 掩码代替条件表达式：G_CONTINUE & (0 - (mask == 0))。
    static constexpr u128 value = {
        Flags::value.low & ~(meta_states::G_CONTINUE & (0 - u64{mask == 0})),
        Flags::value.high};
  };
};

using pipe_main_co =
    meta_states_object<u128{meta_states::condition_seed}, pipe_main_fn>;

// ============================================================================
// 1b) fork：一条支链与主线吃同一个源元素（同一个 index 帧）
// ============================================================================
//
// 主线占存活位图的低段，支链接在后面（编译期偏移），总节点数 ≤ 64。
// 支链不参与 continue 判定：主线活着就继续；支链死活只体现在合并产物和位图里。

// 保存两条线的末端接收器；pipe_view 用产出协议提供 .main() / .branch()。
template <class Main, class Branch>
struct pipe_fork {
  using main_t = Main;
  using branch_t = Branch;
};

template <class T>
struct fork_parts {
  using main_t = T;
  using branch_t = void;
  static constexpr bool forked = false;
};
template <class M, class B>
struct fork_parts<pipe_fork<M, B>> {
  using main_t = M;
  using branch_t = B;
  static constexpr bool forked = true;
};

// branch_size_of / branch_rec 在 output_node 之后定义（它们要用到它）。

// ============================================================================
// 2) 节点与节点列表
// ============================================================================
template <class Con, class Rec>
struct pipe_node {
  using con = Con;
  using rec = Rec;
};

template <class... Nodes>
struct pipe_nodes {
  static constexpr std::size_t size = sizeof...(Nodes);
};

template <class Node, class List>
struct prepend_node;
template <class Node, class... Nodes>
struct prepend_node<Node, pipe_nodes<Nodes...>> {
  using type = pipe_nodes<Node, Nodes...>;
};

// 节点列表是输出 → 输入；head 是最终产物节点。
template <class Nodes>
struct output_node;
template <class Head, class... Rest>
struct output_node<pipe_nodes<Head, Rest...>> {
  using rec = typename Head::rec;
};

// 输出节点的 con：回调门控（G_NOT_SKIP / G_CONTINUE）看它的 flags。
template <class Nodes>
struct output_con;
template <class Head, class... Rest>
struct output_con<pipe_nodes<Head, Rest...>> {
  using type = typename Head::con;
};

template <class Nodes, class Gen>
struct output_prev;
template <class Head, class Gen>
struct output_prev<pipe_nodes<Head>, Gen> {
  using type = Gen;
};
template <class Head, class Second, class... Rest, class Gen>
struct output_prev<pipe_nodes<Head, Second, Rest...>, Gen> {
  using type = typename Second::rec;
};

// Branch 是“支链”的节点列表（pipe_nodes<...>）或 void（没 fork）。
// 这两个 trait 必须定义在 output_node 之后。
template <class T>
struct branch_size_of {
  static constexpr std::size_t value = 0;
};
template <class T>
  requires (!std::is_void_v<T>)
struct branch_size_of<T> {
  static constexpr std::size_t value = T::size;
};

template <class T>
struct branch_rec {
  using type = void;
};
template <class T>
  requires (!std::is_void_v<T>)
struct branch_rec<T> {
  using type = typename output_node<T>::rec;
};

template <std::size_t Count>
inline constexpr u64 initial_alive_mask =
    Count >= 64 ? ~u64{0} : ((u64{1} << Count) - 1);

// 位移安全的小工具：n == 64 时按饱和处理，避免 1 << 64 / x << 64 的 UB。
constexpr u64 low_mask(std::size_t n) noexcept {
  return n >= 64 ? ~u64{0} : ((u64{1} << n) - 1);
}
constexpr u64 mask_shr(u64 v, std::size_t n) noexcept {
  return n >= 64 ? u64{0} : (v >> n);
}
constexpr u64 mask_shl(u64 v, std::size_t n) noexcept {
  return n >= 64 ? u64{0} : (v << n);
}

// looper 的位级工具（gates 字节、门掩码查询），pipe 这一层直接复用。
namespace lb = meta_loop::meta_states_looper_bits::details;

// ============================================================================
// 2b) pipe fork：把 looper 的窗口 / settle 记账从 pipe 里剥出去
// ============================================================================
//
// looper 的 G_FINISH 是"本轮完结"触发器（settle 时消费：提前记账 + 阶段复位）。
// pipe 里它只是事件位：引擎把"上游结束"翻译成 G_FINISH seed 给 con 看，
// 死不死只看 con 清不清 G_CONTINUE。因此 pipe 不能再用 looper 的
// user_flags_control settle —— 那会把轮次记账和阶段滚回提前触发。
//
// 高位规划：bit 64/65 = pipe_emit_bit / pipe_flush_bit（引擎读、con 的
// skip/yield 归一化写）；bit 66..127 = con 自己的 latch（pipe_latch_bit<N>）。
namespace pfd = meta_states_condition_funtion_detail;

// con 的 latch 位段：N = 0..61 → 整体 bit 66..127。restore 的 owned mask 不含
// 高位，con 写的 latch 跨窗口存活。
template <std::size_t N>
  requires (N < 62)
inline constexpr u128 pipe_latch_bit = u128{1} << (66 + N);

// pipe 自己拥有的调度位：窗口号 + position。高 64 位（emit/flush/latch）
// 全部归 con：restore 只从 before 恢复这两个字段，其余位采纳 con 的输出。
inline constexpr u128 pipe_owned_mask = u128{position_mask} | pfd::stage_mask;

// fork receiver_low：接收器初始化失败是 looper 唯一替上游决定的例外
// （对象没了，只能停），原样保留。
constexpr u64 pipe_receiver_low(u64 low, bool failed) noexcept {
  const u64 enabled = 0 - u64{failed};
  return (low & ~(G_CONTINUE & enabled)) |
         ((u64{pfd::receiver_init_failed_state} << user_shift) & enabled);
}

constexpr u128 pipe_receiver_value(u128 flags, bool failed) noexcept {
  flags.low = pipe_receiver_low(flags.low, failed);
  return flags;
}

// fork settle_low。与 looper 的差别只有三点：
//   1) G_FINISH 不进 finished 判定：事件位原样保留（不清除），下一轮由 seed 重写；
//   2) 记账（position+1）与计时器递减只在轮末（post_rec）做，G_FINISH 不提前触发；
//   3) 阶段无条件推进：next_stage 内建 0x80 → 0x10 的回绕，不需要 finished 分支。
constexpr u64 pipe_settle_low(u64 low, u8 window,
                              bool receiver_failed = false) noexcept {
  low = pipe_receiver_low(low, window == post_rec_exec && receiver_failed);
  constexpr u64 stage_mask_low = u64{pfd::stage_index_mask} << user_stage_shift;
  const u64 round_end = 0 - u64{window == post_rec_exec};  // 轮末 = 全 1，否则 0
  const u64 counted = (low & ~position_mask) | ((low + 1) & position_mask);
  const u64 kept = (lb::tick_low(counted) & round_end) | (low & ~round_end);
  return (kept & ~stage_mask_low) |
         (u64{pfd::next_stage(window)} << user_stage_shift);
}

constexpr u128 pipe_settle_value(u128 flags, u8 window,
                                 bool receiver_failed = false) noexcept {
  flags.low = pipe_settle_low(flags.low, window, receiver_failed);
  return flags;
}

// 引擎翻译（分支无关）：上游结束 → G_FINISH 事件。G_CALL_RECEIVER 是 con 自己的门，
// 引擎不碰它：上游无值是"本轮物理上收不到"的事实，由接收判定（_receive_）
// 叠加 HasValue 表达，不写进 con 的 flags —— con 对 G_CALL_RECEIVER 的表态
// （何时打开、何时关闭）跨轮保持，引擎每轮只重写 G_FINISH 事件位。
constexpr u128 pipe_seed_value(u128 flags, bool UpstreamDied) noexcept {
  const u64 finish = G_FINISH & (0 - u64{UpstreamDied});
  return {(flags.low & ~G_FINISH) | finish, flags.high};
}

// fork invoke_window：保留 meta_object_invoke<staged, Obj> + restore_owned_value，
// owned mask 换成 pipe 自己的、settle 换成 pipe 版。
template <u8 Stage, class Cond_o, class Obj>
struct pipe_invoke_window_impl {
  static constexpr u128 staged_value =
      pfd::with_stage_value(Cond_o::flags::value, Stage);
  static consteval auto result_type() {
    if constexpr (Stage == post_rec_exec &&
                  pfd::initialization_failed(staged_value,
                                             uninitialized_meta_object_t<Obj>)) {
      return std::type_identity<typename Cond_o::template meta_set<
          flags_operator<pipe_settle_value(staged_value, Stage, true)>>>{};
    } else {
      using staged =
          typename Cond_o::template meta_set<flags_operator<staged_value>>;
      using called = meta_object_invoke<staged, Obj>;
      constexpr u128 value = pipe_settle_value(
          restore_owned_value(pipe_owned_mask, staged_value,
                              called::flags::value),
          Stage);
      return std::type_identity<
          typename called::template meta_set<flags_operator<value>>>{};
    }
  }
  using type = typename decltype(result_type())::type;
};

template <u8 Stage, class Cond_o, class Obj>
using pipe_invoke_window =
    typename pipe_invoke_window_impl<Stage, Cond_o, Obj>::type;

// fork enter_window_step：pipe 里 G_FINISH 不再掐掉后续窗口（事件位要一路
// 传到 post_rec 给 stop_on_finish 看）；Gate 改用 G_CONTINUE —— 总闸被清的
// 轮后续窗口走轻量分支（Fn 不会再被调用），行为等价但少实例化。
template <bool Gate, u8 Stage, class Cond_o, class Obj>
struct pipe_enter_window_step_impl {
  static consteval auto result_type() {
    if constexpr (!Gate) {
      if constexpr (Stage == post_rec_exec &&
                    uninitialized_meta_object_t<Obj>) {
        if constexpr (pfd::initialization_failed(Cond_o::flags::value, true)) {
          return std::type_identity<typename Cond_o::template meta_set<
              flags_operator<
                  pipe_receiver_value(Cond_o::flags::value, true)>>>{};
        } else return std::type_identity<Cond_o>{};
      } else return std::type_identity<Cond_o>{};
    } else {
      return std::type_identity<pipe_invoke_window<Stage, Cond_o, Obj>>{};
    }
  }
  using type = typename decltype(result_type())::type;
};

template <bool Gate, u8 Stage, class Cond_o, class Obj>
using pipe_enter_window_step =
    typename pipe_enter_window_step_impl<Gate, Stage, Cond_o, Obj>::type;

// ============================================================================
// 3) 单步：共享窗口原语，独立保留管道的输入/发射/冲刷调度
// ============================================================================
//
// 输入：
//   HasValue      上游本轮是否真的发来一个值（filter 吞掉时为 false）
//   UpstreamDied  上游本轮是否死亡（本节点因此也要死，但仍可接收最后一次冲刷）
// 输出：
//   con / rec     新的节点状态
//   emitted       本节点本轮是否往下游发值
//   just_died     本节点本轮是否死亡
template <bool GenValid, class Con, class Rec, class Gen, bool HasValue,
          bool UpstreamDied, bool IsSource, class... Args>
struct pipe_node_step_impl;

// 上游没有形成产物：不跑四个窗口（窗口① 需要推进 gen，而 gen 还没成型）。
// 只结算一轮：引擎 seed 事件位，con 在窗口④ 表态（stop_on_finish 看 G_FINISH）。
template <class Con, class Rec, class Gen, bool HasValue, bool UpstreamDied,
          bool IsSource, class... Args>
struct pipe_node_step_impl<false, Con, Rec, Gen, HasValue, UpstreamDied, IsSource,
                           Args...> {
  static constexpr bool initialize = gated_bits<G_CONTINUE | G_INITIALIZE>(
      lb::effective_value(Con::flags::value));
  static constexpr u128 executed_flags =
      pfd::after_execution(Con::flags::value, initialize, false);
  // 引擎翻译：上游结束 → G_FINISH 事件（G_CALL_RECEIVER 归 con，引擎不碰）。
  static constexpr u128 seeded_value =
      pipe_seed_value(executed_flags, UpstreamDied);
  using seeded_con =
      typename Con::template meta_set<flags_operator<seeded_value>>;
  using rec = std::conditional_t<initialize, meta_object_initialize<Rec>, Rec>;
  // 窗口④ 结算：con 在 post_rec 看到 G_FINISH 自己决定停不停；接收器初始化
  // 失败是唯一例外（pipe_invoke_window 的 receiver_failed 特例）。
  using con = pipe_invoke_window<post_rec_exec, seeded_con, rec>;
  using gen_out = Gen;
  static constexpr bool _receive_ = false;
  // 死不死只看 con 清没清 G_CONTINUE。
  static constexpr bool just_died = !con::flags::template test<G_CONTINUE>;
  static constexpr bool emit_flag = con::flags::template test<pipe_emit_bit>;
  static constexpr bool flush_flag = con::flags::template test<pipe_flush_bit>;
  static constexpr bool has_produce = exp_utilities::has_type<rec>;
  static constexpr bool emitted = just_died && flush_flag && has_produce;
};
// 正常一步：四窗口逻辑。
template <class Con, class Rec, class Gen, bool HasValue, bool UpstreamDied,
          bool IsSource, class... Args>
struct pipe_node_step_impl<true, Con, Rec, Gen, HasValue, UpstreamDied,
                           IsSource, Args...> {
  // ---- seed：引擎翻译上游状态（窗口① 之前）----
  static constexpr u128 seeded_value =
      pipe_seed_value(Con::flags::value, UpstreamDied);
  using seeded_con =
      typename Con::template meta_set<flags_operator<seeded_value>>;

  // ---- 窗口① pre_gen_exec ----
  using pre_gen_exec_stage_o = pipe_invoke_window<pre_gen_exec, seeded_con, Gen>;
  static constexpr u128 pre_gen_judge =
      lb::effective_value(pre_gen_exec_stage_o::flags::value);
  static constexpr u8 pre_gen_gates = lb::gates_of(pre_gen_judge);

  static constexpr bool _entered_ = pre_gen_gates & lb::gate_continue;
  // G_FINISH 不再参与引擎判定：它只是 con 可见的事件位（seed 负责）。
  static constexpr bool _advance_ = lb::gates_have(
      pre_gen_gates, lb::gate_continue | lb::gate_advance);
  static constexpr bool _initialize_gen_ = IsSource && lb::gates_have(
      pre_gen_gates, lb::gate_continue | lb::gate_initialize);

  // 节点 0 默认 ADVANCE 开 → 真正推进 gen；
  // 中间节点默认 ADVANCE 关 → invoke_if<false> 不调用上游（上游已经更新过）。
  static consteval auto generator_type() {
    if constexpr (_initialize_gen_) return std::type_identity<meta_object_initialize<Gen>>{};
    else return std::type_identity<meta_invoke<invoke_if<_advance_>, Gen, Args...>>{};
  }
  using generator_stage_o = typename decltype(generator_type())::type;
  using after_gen_condition = typename pre_gen_exec_stage_o::template meta_set<
      flags_operator<pfd::after_execution(
          pre_gen_exec_stage_o::flags::value, _initialize_gen_, false)>>;

  // ---- 窗口② post_gen_exec：G_FINISH 不掐窗口（事件位一路传到 post_rec），
  //      但 G_CONTINUE 被清的轮后续窗口走轻量分支（Fn 不会再被调用）。----
  using post_gen_exec_stage_o = pipe_enter_window_step<
      _entered_, post_gen_exec, after_gen_condition, generator_stage_o>;
  static constexpr u128 post_gen_judge =
      lb::effective_value(post_gen_exec_stage_o::flags::value);

  // ---- 窗口③ pre_rec_exec ----
  static constexpr bool _post_gen_gate_ =
      lb::gates_of(post_gen_judge) & lb::gate_continue;
  using pre_rec_exec_stage_o = pipe_enter_window_step<
      _post_gen_gate_, pre_rec_exec, post_gen_exec_stage_o, Rec>;
  static constexpr u128 pre_rec_judge =
      lb::effective_value(pre_rec_exec_stage_o::flags::value);
  static constexpr u8 pre_rec_gates = lb::gates_of(pre_rec_judge);

  // 收不收只看 con 的门（G_CONTINUE / G_CALL_RECEIVER）。pipe 不假设有没有输入：
  // G_CALL_RECEIVER 开就 invoke，参数 = 上游 rec 的 produce —— 它是什么（真值、
  // end_of_list 标记、未初始化对象）由 meta_object_invoke 协议和 con 的 Fn
  // 决定；receiver 本身也可以是 generator，行为由协议规定。
  static constexpr bool _receive_ =
      _entered_ &&
      lb::gates_have(pre_rec_gates, lb::gate_continue | lb::gate_receive);
  static constexpr bool _initialize_rec_ =
      _entered_ &&
      lb::gates_have(pre_rec_gates, lb::gate_continue | lb::gate_initialize);

  static consteval auto receiver_type() {
    if constexpr (_initialize_rec_) return std::type_identity<meta_object_initialize<Rec>>{};
    else return std::type_identity<
        meta_invoke<invoke_object_if<_receive_>, Rec, generator_stage_o>>{};
  }
  using result_stage_o = typename decltype(receiver_type())::type;
  using after_rec_condition = typename pre_rec_exec_stage_o::template meta_set<
      flags_operator<pfd::after_execution(
          pre_rec_exec_stage_o::flags::value, _initialize_rec_, _receive_)>>;

  // ---- 窗口④ post_rec_exec ----
  static constexpr bool _pre_rec_gate_ = pre_rec_gates & lb::gate_continue;
  using post_rec_exec_stage_o = pipe_enter_window_step<
      _pre_rec_gate_, post_rec_exec, after_rec_condition, result_stage_o>;
  static constexpr u128 post_rec_judge =
      lb::effective_value(post_rec_exec_stage_o::flags::value);
  static constexpr u8 post_rec_gates = lb::gates_of(post_rec_judge);

  static constexpr bool _continue_ =
      _entered_ && lb::gates_have(post_rec_gates, lb::gate_continue);
  // 死不死只看 con 清没清 G_CONTINUE（引擎不替节点判死）。
  static constexpr bool just_died = !_continue_;

  using con = post_rec_exec_stage_o;  // G_FINISH 保留，下一轮由 seed 重写
  using rec = result_stage_o;
  // 只有最输入端的节点会把 gen 推进成新类型，需要带回下一轮。
  using gen_out = generator_stage_o;

  // ---- 发射 / 冲刷 ----
  static constexpr bool emit_flag = con::flags::template test<pipe_emit_bit>;
  static constexpr bool flush_flag = con::flags::template test<pipe_flush_bit>;
  // 未初始化的 receiver 没有合法产物，不能发射。
  static constexpr bool has_produce = exp_utilities::has_type<rec>;
  // 三个条件并成一条位式：普通轮看 receive & emit，死亡轮只认 flush。
  static constexpr bool emitted =
      has_produce && (((_receive_ && !_initialize_rec_) & emit_flag) | (just_died & flush_flag));
};

// 输入端（IsSource）即使 gen 未初始化也要正常走第一步（con 会推进它）；
// 下游节点拿到的上游未形成产物时：上游已死 → 本节点死亡；上游还活着 → 空转。
template <class Con, class Rec, class Gen, bool HasValue, bool UpstreamDied,
          bool IsSource, class... Args>
struct pipe_node_step
    : pipe_node_step_impl<IsSource || exp_utilities::has_type<Gen>, Con, Rec,
                          Gen, HasValue, UpstreamDied, IsSource, Args...> {};

// 一个节点在 fold 里的结果。
template <bool Alive, bool IsOutput, bool IsInputMost, std::size_t Index,
          u64 Mask, class GenOut, class InValue, bool HasValue,
          bool UpstreamDied, class Node, class... Args>
struct pipe_fold_node;

// 已死的节点：不调用，原样保留，不发射。
template <bool IsOutput, bool IsInputMost, std::size_t Index, u64 Mask,
          class GenOut, class InValue, bool HasValue, bool UpstreamDied,
          class Node, class... Args>
struct pipe_fold_node<false, IsOutput, IsInputMost, Index, Mask, GenOut,
                      InValue, HasValue, UpstreamDied, Node, Args...> {
  using node = Node;
  using out = typename Node::rec;
  using gen_out = GenOut;
  // 死亡节点的最终产物：只有带 FLUSH 的节点（wait_for_end 之类）死时才把
  // 产物交给下游，之后下游按自己的节奏 drain 它 —— has_value 持续为真。
  // 未初始化的 rec 没有产物。
  static constexpr bool flush_dead =
      Node::con::flags::template test<pipe_flush_bit>;
  static constexpr bool has_value = exp_utilities::has_type<out> && flush_dead;
  static constexpr bool just_died = false;
  static constexpr u64 mask = Mask;
};

// 活着的节点：走一步。
template <bool IsOutput, bool IsInputMost, std::size_t Index, u64 Mask,
          class GenOut, class InValue, bool HasValue, bool UpstreamDied,
          class Node, class... Args>
struct pipe_fold_node<true, IsOutput, IsInputMost, Index, Mask, GenOut,
                      InValue, HasValue, UpstreamDied, Node, Args...> {
  using step = pipe_node_step<typename Node::con, typename Node::rec, InValue,
                              HasValue, UpstreamDied, IsInputMost, Args...>;
  using node = pipe_node<typename step::con, typename step::rec>;
  using out = typename step::rec;
  using gen_out =
      std::conditional_t<IsInputMost, typename step::gen_out, GenOut>;
  static constexpr bool has_value = step::emitted;
  static constexpr bool just_died = step::just_died;
  // just_died 是 0/1，左移 Index 后正好是要清的那一位：一条与-非就完成归约
  static constexpr u64 mask = Mask & ~(u64{just_died} << Index);
};

// ============================================================================
// 4) 一轮的 fold（输入 → 输出，右 fold）
// ============================================================================
template <class Nodes, std::size_t Index, bool IsOutput, u64 Mask,
          class InValue, bool InHasValue, bool InDied, class... Args>
struct pipe_fold;

template <std::size_t Index, bool IsOutput, u64 Mask, class InValue,
          bool InHasValue, bool InDied, class... Args>
struct pipe_fold<pipe_nodes<>, Index, IsOutput, Mask, InValue, InHasValue,
                 InDied, Args...> {
  using nodes = pipe_nodes<>;
  using out = InValue;
  using prev = InValue;
  using gen_out = InValue;
  // 输入最端节点的 HasValue = fold 传入的物理事实（主线恒 true：节点 0 自己
  // 推进 gen；支链 = 源元素帧 produce 不是 end_of_list）。
  static constexpr bool has_value = InHasValue;
  static constexpr bool just_died = false;
  static constexpr u64 mask = Mask;
  static constexpr bool middle_died = false;
  static constexpr bool output_died = false;
};

template <class Head, class... Rest, std::size_t Index, bool IsOutput, u64 Mask,
          class InValue, bool InHasValue, bool InDied, class... Args>
struct pipe_fold<pipe_nodes<Head, Rest...>, Index, IsOutput, Mask, InValue,
                 InHasValue, InDied, Args...> {
  // 先处理更靠输入端的节点（列表尾部），再把结果交给 head。
  using inner = pipe_fold<pipe_nodes<Rest...>, Index - 1, false, Mask, InValue,
                          InHasValue, InDied, Args...>;

  static constexpr bool alive = (Mask >> Index) & 1;
  static constexpr bool is_input_most = (sizeof...(Rest) == 0);
  // 上游已死：中间节点看上游存活位（轮后位图里清了 = 本轮死或之前死，
  // G_FINISH 同轮可见）；输入最端的节点没有位图上游，用 fold 传入的 InDied
  // （主线 fold 恒 false——节点 0 自己推进 gen，eol 由窗口② 检查；
  //   支链 fold 传"主线源节点已死"——支链和主线吃同一个源元素）。
  // 位图清位由上游节点自己的 just_died 执行，引擎只负责把"上游已死"
  // 翻译成 G_FINISH 事件，不替下游判死。
  static constexpr bool upstream_dead =
      is_input_most ? InDied : ((inner::mask >> (Index - 1)) & 1) == 0;

  using node_result = pipe_fold_node<
      alive, IsOutput, is_input_most, Index, inner::mask, typename inner::gen_out,
      typename inner::out, inner::has_value, upstream_dead, Head, Args...>;

  using nodes = typename prepend_node<typename node_result::node,
                                      typename inner::nodes>::type;
  using out = typename node_result::out;
  using prev = std::conditional_t<IsOutput, typename inner::out,
                                  typename inner::prev>;
  using gen_out = typename node_result::gen_out;

  static constexpr bool has_value = node_result::has_value;
  static constexpr bool just_died = node_result::just_died;
  static constexpr u64 mask = node_result::mask;
  static constexpr bool middle_died =
      inner::middle_died || (node_result::just_died && !IsOutput);
  static constexpr bool output_died =
      inner::output_died || (node_result::just_died && IsOutput);
};

// ============================================================================
// 5) 一轮：节点 fold + 支链 + main 更新
// ============================================================================
// 支链的一轮：拿主线本轮消费的那个元素（folded::gen_out），完整跑一遍 fold。
// 没 fork（Branch = void）时什么都不做。SourceDied = 主线源节点（位图 bit0）
// 已死：支链与主线吃同一个源元素，源耗尽就是支链的"上游结束"。
template <bool Forked, class Branch, class InValue, u64 Mask, bool SourceDied,
          class... Args>
struct pipe_branch_fold_result {
  using nodes = void;
  using out = void;
  static constexpr u64 mask = 0;
  static constexpr bool died = false;
};
template <class Branch, class InValue, u64 Mask, bool SourceDied, class... Args>
struct pipe_branch_fold_result<true, Branch, InValue, Mask, SourceDied,
                               Args...> {
  // 支链与主线吃同一个源元素帧；接收不假设输入（G_CALL_RECEIVER 门控 invoke，
  // 参数由 meta_object_invoke 协议解包），InHasValue 不再参与接收判定。
  using folded = pipe_fold<Branch, Branch::size - 1, true, Mask, InValue, true,
                           SourceDied, Args...>;
  using nodes = typename folded::nodes;
  using out = typename folded::out;
  static constexpr u64 mask = folded::mask;
  static constexpr bool died = folded::output_died;
};

template <class Main, class Nodes, class Gen, class Branch = void,
          class... Args>
struct pipe_round {
  static constexpr bool forked = !std::is_void_v<Branch>;
  static constexpr std::size_t main_nodes = Nodes::size;
  static constexpr std::size_t branch_nodes = branch_size_of<Branch>::value;
  static constexpr u64 mask_raw = Main::flags::value.high;

  // 存活位图分两段：主线 bit 0..M-1，支链 bit M..M+B-1（编译期偏移）。
  static constexpr u64 mask_main = mask_raw & low_mask(main_nodes);
  static constexpr u64 mask_branch =
      forked ? (mask_shr(mask_raw, main_nodes) & low_mask(branch_nodes))
             : u64{0};
  static constexpr std::size_t index =
      Main::flags::value.low & meta_states::position_mask;

  using folded = pipe_fold<Nodes, Nodes::size - 1, true, mask_main, Gen, true,
                           false, Args...>;
  using branch_fold =
      pipe_branch_fold_result<forked, Branch, typename folded::gen_out,
                              mask_branch, (folded::mask & 1) == 0, Args...>;

  using nodes = typename folded::nodes;
  using branch = typename branch_fold::nodes;
  using out = std::conditional_t<forked,
                                 pipe_fork<typename folded::out,
                                           typename branch_fold::out>,
                                 typename folded::out>;
  using prev = typename folded::prev;
  using gen_out = typename folded::gen_out;

  static constexpr u64 mask_after =
      folded::mask | mask_shl(branch_fold::mask, main_nodes);

  // 把新的存活位图写进 main 的高 64 位，并记一步 position。
  // position 是低 16 位计数器：+1 后按掩码回绕，其余位原样。
  static constexpr u64 low_before = Main::flags::value.low;
  static constexpr u64 low_counted =
      (low_before & ~meta_states::position_mask) |
      ((low_before + 1) & meta_states::position_mask);
  using flags_counted = flags_operator<u128{low_counted, folded::mask}>;
  using main_staged = typename Main::template meta_set<flags_counted>;
  // main 的算法接收整个节点对象，与 MSO 观察者的对象调用协议一致。
  // main 只看主线位图，支链死活不影响 continue。
  using main_out_raw = meta_invoke<main_staged, typename folded::out>;
  // position 与存活位图由引擎维护，不能由 main 的 Fn 覆盖。
  static constexpr u128 main_owned_mask =
      u128{meta_states::position_mask} | (~u128{0} << pipe_mask_shift);
  using main_out_restored = typename main_out_raw::template meta_set<
      restore_owned_flags<main_owned_mask, flags_counted,
                          typename main_out_raw::flags>>;
  using main_out = typename main_out_restored::template meta_set<
      flags_operator<u128{main_out_restored::flags::value.low, mask_after}>>;

  static constexpr bool has_value = folded::has_value;
  static constexpr bool middle_died = folded::middle_died;
  static constexpr bool output_died = folded::output_died;
  // 回调门控：只受输出节点 con 的 G_CONTINUE / G_NOT_SKIP 影响（与 looper 的
  // observe 一致），不依赖本轮有没有 emit 值。节点死亡时 G_CONTINUE 必被清
  // （just_died = !_continue_），所以死节点自动不再回调。
  static constexpr bool observe_ = lb::gates_have(
      lb::gates_of(output_con<typename folded::nodes>::type::flags::value),
      lb::gate_not_skip | lb::gate_continue);
  // 停机判定：位图全 0 = 没有任何活节点，继续跑也只是空转 —— 这是 pipe 的
  // 物理下限，不是替节点判死（节点的死活由 con 决定）。之上只看 main 的
  // G_CONTINUE（默认 main 在 mask==0 时清它，自定义 main 自己决定更早停）。
  static constexpr bool continue_ =
      (mask_after != 0) &&
      main_out::flags::template test<meta_states::G_CONTINUE>;
};

// ============================================================================
// 6) 最终 / 每轮的视图
// ============================================================================
template <class To, class From, class States, std::size_t Index,
          class Driver = void, std::size_t BranchNodes = 0>
struct pipe_view {
  using to = To;
  using from = From;
  using states = States;
  using driver = Driver;
  static constexpr std::size_t branch_nodes = BranchNodes;
  using to_t = get_type<to>;
  using from_t = get_type<from>;
  using cache = meta_produce_t<from>;

  // 主线/支链按对象协议取产物；未 fork 时不实例化 void 支链的产出。
  using parts = fork_parts<to>;
  static constexpr bool forked = parts::forked;
  using main_t = meta_produce_t<typename parts::main_t>;
  using branch_t = select_if_t<forked, meta_quote::unary<meta_produce_t>,
                               always_fn<void>, typename parts::branch_t>;

  consteval std::size_t index() const { return Index; }
  consteval std::size_t left() const { return exp_size<from_t>; }
  consteval auto value() const { return value_of<to_t>; }
  constexpr auto object() const { return to_t{}; }
  // fork 后：s.main() 是主线产物、s.branch() 是支链产物（同一轮、同一个 index）。
  constexpr auto main() const { return main_t{}; }
  constexpr auto branch() const requires forked { return branch_t{}; }
  consteval const std::type_info& target_type() const { return typeid(to_t); }
};

// ============================================================================
// 7) 引擎：每轮跑 pipe_round，然后递归
// ============================================================================
// ============================================================================
// 7) 分块续跑：pipe 状态 / chunk / driver
// ============================================================================
template <class Main, class Nodes, class Gen, class Branch = void>
struct pipe_state {};

// 每轮 fold 的深度约为节点数；预算按节点数自适应，
// 让“段内轮数 × 每轮深度 + 段数”保持在模板深度限制之下。
template <std::size_t NodeCount>
inline constexpr std::size_t pipe_chunk_budget =
    std::clamp<std::size_t>(256 / (NodeCount + 4), 1, 64);

template <class Round, class Driver = void>
using pipe_round_view = pipe_view<typename Round::out, typename Round::prev,
                                  typename Round::main_out::flags,
                                  Round::index, Driver, Round::branch_nodes>;

template <class State>
struct pipe_state_budget;
template <class Main, class Nodes, class Gen, class Branch>
struct pipe_state_budget<pipe_state<Main, Nodes, Gen, Branch>> {
  static constexpr std::size_t value = pipe_chunk_budget<Nodes::size>;
};

template <class Main, class Nodes, class Gen, class Branch = void>
using default_pipe_driver_policy = default_driver_policy<
    pipe_state_budget<pipe_state<Main, Nodes, Gen, Branch>>::value>;

// ============================================================================
// 7b) flags 驱动的 pipe chunk / driver
//
// 与 looper 共用 flags_driver_mso / driver_control；
// pipe 的 step 适配器把 Round::observe_/continue_ 交给 driver policy。
// ============================================================================
template <class Round>
struct pipe_driver_step {
  using round = Round;
  static constexpr bool continue_ = Round::continue_;
  static constexpr bool observe_ = Round::observe_;
  static constexpr bool consume_ = Round::main_out::flags::template test<G_CONSUME>;
};

template <class Driver, class State>
struct pipe_flags_chunk;
template <class Driver, class State>
struct pipe_flags_driver;

template <class Driver, class Main, class Nodes, class Gen, class Branch>
struct pipe_flags_chunk<Driver, pipe_state<Main, Nodes, Gen, Branch>> {
  using round = pipe_round<Main, Nodes, Gen, Branch>;
  using step = pipe_driver_step<round>;
  using round_driver = meta_invoke<Driver,
      driver_control::input<step, driver_control::window_round>>;
  // 回调看到的是“本轮开始前”的 driver 状态，与 looper 的 meta_pack_stream 一致。
  using view = pipe_round_view<round, Driver>;

  static constexpr u8 phase =
      round_driver::flags::template get<u8, driver_control::phase_shift>;

  using step_next_state = pipe_state<typename round::main_out,
                                     typename round::nodes,
                                     typename round::gen_out,
                                     typename round::branch>;
  using step_next_driver = meta_invoke<round_driver,
      driver_control::input<step, driver_control::window_more>>;
  using rest = pipe_flags_chunk<round_driver, step_next_state>;

  static consteval bool has_more_fn() {
    if constexpr (phase == driver_control::phase_more) {
      return true;
    } else if constexpr (phase == driver_control::phase_run) {
      return rest::has_more;
    } else {
      return false;
    }
  }
  static constexpr bool has_more = has_more_fn();

  static consteval auto next_driver_fn() {
    if constexpr (phase == driver_control::phase_run) {
      return std::type_identity<typename rest::next_driver>{};
    } else {
      return std::type_identity<step_next_driver>{};
    }
  }
  using next_driver = typename decltype(next_driver_fn())::type;

  static consteval auto next_state_fn() {
    if constexpr (phase == driver_control::phase_run) {
      return std::type_identity<typename rest::next_state>{};
    } else {
      return std::type_identity<step_next_state>{};
    }
  }
  using next_state = typename decltype(next_state_fn())::type;

  static consteval auto feed_fn() {
    if constexpr (phase == driver_control::phase_done) {
      return std::type_identity<view>{};
    } else if constexpr (phase == driver_control::phase_more) {
      return std::type_identity<void>{};
    } else {
      return std::type_identity<typename rest::feed>{};
    }
  }
  using feed = typename decltype(feed_fn())::type;

  static constexpr bool observes = [] {
    if constexpr (phase == driver_control::phase_done ||
                  phase == driver_control::phase_more) {
      return step::observe_;
    } else {
      return any_observes<step::observe_, rest>::value;
    }
  }();

  template <std::size_t N>
  static consteval bool observes_within_fn() {
    if constexpr (phase != driver_control::phase_run) {
      return N > 0 && step::observe_;
    } else if constexpr (N == 0) {
      return false;
    } else {
      return any_within<step::observe_, N - step::consume_, rest>::value;
    }
  }

  static constexpr std::size_t consumed = [] {
    if constexpr (phase == driver_control::phase_run)
      return std::size_t{step::consume_} + rest::consumed;
    else return std::size_t{step::consume_};
  }();

  template <class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (phase != driver_control::phase_run) {
      if constexpr (step::observe_) {
        return std::invoke(f, view{}, std::forward<A>(a)...);
      } else {
        return;
      }
    } else {
      if constexpr (step::observe_) {
        using return_type =
            std::invoke_result_t<F&, view, A...>;
        if constexpr (is_for_each_decision_v<return_type>) {
          if constexpr (is_for_each_stop_v<return_type>) {
            return std::invoke(f, view{}, std::forward<A>(a)...);
          } else if constexpr (rest::observes) {
            (void)std::invoke(f, view{}, std::forward<A>(a)...);
            return rest::for_each(std::forward<F>(f),
                                  std::forward<A>(a)...);
          } else {
            return std::invoke(f, view{}, std::forward<A>(a)...);
          }
        } else if constexpr (rest::observes) {
          if constexpr (std::is_void_v<return_type>) {
            std::invoke(f, view{}, std::forward<A>(a)...);
          } else {
            (void)std::invoke(f, view{}, std::forward<A>(a)...);
          }
          return rest::for_each(std::forward<F>(f),
                                std::forward<A>(a)...);
        } else {
          return std::invoke(f, view{}, std::forward<A>(a)...);
        }
      } else {
        if constexpr (rest::observes) {
          return rest::for_each(std::forward<F>(f),
                                std::forward<A>(a)...);
        } else {
          return;
        }
      }
    }
  }

  static consteval auto forward_next_type() {
    if constexpr (phase == driver_control::phase_more)
      return std::type_identity<pipe_flags_driver<next_driver, next_state>>{};
    else return std::type_identity<rest>{};
  }
  using forward_next = typename decltype(forward_next_type())::type;
  static constexpr bool consume = round::main_out::flags::template test<G_CONSUME>;

  template<std::size_t N>
  static consteval bool forward_observes() {
    if constexpr (N == 0) return false;
    else if constexpr (observes_within_fn<N>()) return true;
    else if constexpr (has_more && N > consumed)
      return pipe_flags_driver<next_driver, next_state>
          ::template forward_observes<N - consumed>();
    else return false;
  }

  template<class F, class First, class... A>
  static constexpr decltype(auto) forward_tail(F&& f, First&& first, A&&... a) {
    if constexpr (consume)
      return forward_next::for_each_forward(std::forward<F>(f), std::forward<A>(a)...);
    else return forward_next::for_each_forward(
        std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
  }

  template<class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first, A&&... a) {
    constexpr bool future = [] {
      if constexpr (phase == driver_control::phase_done) return false;
      else if constexpr (step::observe_) {
        if constexpr (is_for_each_stop_v<std::invoke_result_t<F&, view, First>>) return false;
        else return forward_next::template forward_observes<sizeof...(A) + !consume>();
      } else return forward_next::template forward_observes<sizeof...(A) + !consume>();
    }();
    if constexpr (step::observe_) {
      using result = std::invoke_result_t<F&, view, First>;
      if constexpr (is_for_each_stop_v<result> || !future) {
        return std::invoke(f, view{}, std::forward<First>(first));
      } else {
        (void)std::invoke(f, view{}, std::forward<First>(first));
        return forward_tail(std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
      }
    } else if constexpr (future) {
      return forward_tail(std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
    }
  }
};

template <class Driver, class Main, class Nodes, class Gen, class Branch>
struct pipe_flags_driver<Driver, pipe_state<Main, Nodes, Gen, Branch>> {
  using chunk = pipe_flags_chunk<Driver, pipe_state<Main, Nodes, Gen, Branch>>;

  template<std::size_t N>
  static consteval bool forward_observes() {
    return chunk::template forward_observes<N>();
  }

  template <class Decision>
  using decide = meta_invoke<Driver,
      driver_control::input<typename chunk::step,
                            driver_control::window_decide, Decision>>;

  static consteval auto result_type() {
    if constexpr (chunk::has_more) {
      return std::type_identity<typename pipe_flags_driver<
          typename chunk::next_driver,
          typename chunk::next_state>::type>{};
    } else {
      return std::type_identity<typename chunk::feed>{};
    }
  }
  using type = typename decltype(result_type())::type;

  struct observes_t {
    static constexpr bool value = [] {
      if constexpr (!chunk::has_more) {
        return chunk::observes;
      } else {
        using next = pipe_flags_driver<typename chunk::next_driver,
                                       typename chunk::next_state>;
        return any_observes<chunk::observes, next>::value;
      }
    }();
  };
  static constexpr bool observes = observes_t::value;

  template <class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (chunk::has_more) {
      using chunk_return = decltype(chunk::for_each(
          std::forward<F>(f), std::forward<A>(a)...));
      if constexpr (is_for_each_stop_v<chunk_return>) {
        return chunk::for_each(std::forward<F>(f),
                               std::forward<A>(a)...);
      } else {
        using next = pipe_flags_driver<typename chunk::next_driver,
                                       typename chunk::next_state>;
        if constexpr (next::observes_t::value) {
          if constexpr (chunk::observes) {
            (void)chunk::for_each(std::forward<F>(f),
                                  std::forward<A>(a)...);
          }
          return next::for_each(std::forward<F>(f),
                                std::forward<A>(a)...);
        } else {
          if constexpr (chunk::observes) {
            return chunk::for_each(std::forward<F>(f),
                                   std::forward<A>(a)...);
          } else {
            return;
          }
        }
      }
    } else {
      if constexpr (chunk::observes) {
        return chunk::for_each(std::forward<F>(f),
                               std::forward<A>(a)...);
      } else {
        return;
      }
    }
  }

  template <class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   A&&... a) {
    return chunk::for_each_forward(std::forward<F>(f),
                                   std::forward<First>(first),
                                   std::forward<A>(a)...);
  }
};


// 初始 main 就关闭 G_CONTINUE：一轮都不跑，直接给初始视图。
template <class Main, class Nodes, class Gen, class Branch = void>
using pipe_initial_view = pipe_view<
    std::conditional_t<std::is_void_v<Branch>,
                       typename output_node<Nodes>::rec,
                       pipe_fork<typename output_node<Nodes>::rec,
                                 typename branch_rec<Branch>::type>>,
    typename output_prev<Nodes, Gen>::type, typename Main::flags, 0>;

template <bool Start, class Main, class Nodes, class Gen, class DriverPolicy,
          class Branch>
struct pipe_runner_engine {
  using state = pipe_state<Main, Nodes, Gen, Branch>;
  static constexpr std::size_t budget = pipe_state_budget<state>::value;
  static consteval auto engine_type() {
    if constexpr (Start) {
      return std::type_identity<
          pipe_flags_driver<flags_driver_mso<budget, DriverPolicy>, state>>{};
    } else {
      return std::type_identity<pipe_initial_view<Main, Nodes, Gen, Branch>>{};
    }
  }
  using engine = typename decltype(engine_type())::type;
  static consteval auto result_type() {
    if constexpr (Start) {
      return std::type_identity<typename engine::type>{};
    } else {
      return std::type_identity<engine>{};
    }
  }
  using type = typename decltype(result_type())::type;

  template <class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (Start) {
      return engine::for_each(std::forward<F>(f), std::forward<A>(a)...);
    }
  }

  template <class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   A&&... a) {
    if constexpr (Start) {
      return engine::for_each_forward(std::forward<F>(f),
                                      std::forward<First>(first),
                                      std::forward<A>(a)...);
    }
  }
};

// ============================================================================
// 8) runner：设置初始存活位图，暴露 ::type / for_each
// ============================================================================
template <class Main, class Nodes, class Gen, class DriverPolicy = void,
          class Branch = void>
struct meta_pipe_runner {
  static constexpr bool forked = !std::is_void_v<Branch>;
  static constexpr std::size_t main_nodes = Nodes::size;
  static constexpr std::size_t branch_nodes = branch_size_of<Branch>::value;
  static_assert(main_nodes >= 1, "meta_pipe: run<> 需要至少一个主线节点");
  static_assert(main_nodes + branch_nodes <= 64,
                "meta_pipe: 存活位图只有 64 位（主线 + 支链）");

  using policy = std::conditional_t<
      std::is_void_v<DriverPolicy>,
      default_pipe_driver_policy<Main, Nodes, Gen, Branch>, DriverPolicy>;

  static constexpr u64 initial_mask =
      initial_alive_mask<main_nodes> |
      (forked ? mask_shl(initial_alive_mask<branch_nodes>, main_nodes) : u64{0});
  using main_init = typename Main::template meta_set<
      typename Main::flags::template set<u64, pipe_mask_shift, initial_mask>>;
  // 初始 main 已经关掉总闸时，一轮都不跑。
  static constexpr bool start =
      main_init::flags::template test<meta_states::G_CONTINUE>;
  using engine = pipe_runner_engine<start, main_init, Nodes, Gen, policy, Branch>;
  using type = typename engine::type;

  template <template <class> class... Ps, class F, class... Args>
  static constexpr decltype(auto) for_each(F&& f, Args&&... args) {
    return engine::for_each(
        [&f]<class Stage, class... A>(Stage, A&&... a) -> decltype(auto) {
          return std::invoke(f, meta_fold<Stage, Ps...>{},
                             std::forward<A>(a)...);
        },
        std::forward<Args>(args)...);
  }

  template <template <class> class... Ps, class F, class First, class... Args>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   Args&&... args) {
    return engine::for_each_forward(
        [&f]<class Stage, class V>(Stage, V&& v) -> decltype(auto) {
          return std::invoke(f, meta_fold<Stage, Ps...>{},
                             std::forward<V>(v));
        },
        std::forward<First>(first), std::forward<Args>(args)...);
  }
};

// 默认 con 的显式标记解包：允许 con<C>，也允许直接给 con 类型。
template <class T>
struct unwrap_con {
  using type = T;
};
template <class C>
struct unwrap_con<meta_ios::con<C>> {
  using type = C;
};
template <class T>
struct unwrap_main {
  using type = T;
};
template <class M>
struct unwrap_main<meta_ios::main_con<M>> {
  using type = M;
};

// ============================================================================
// 9) builder：写的时候 gen → 输出，内部 prepend 到输出端（和 ranges 一样）
// ============================================================================
// 生成 run 的节点列表：Rec 为 void 表示不追加节点（fork 后直接 run<>）。
template <class Nodes, class Rec, class Con>
struct builder_run_nodes {
  using type = Nodes;
};
template <class Nodes, class Rec, class Con>
  requires (!std::is_void_v<Rec>)
struct builder_run_nodes<Nodes, Rec, Con> {
  using con = std::conditional_t<
      std::is_void_v<Con>, default_node_con<Nodes::size == 0>,
      typename unwrap_con<Con>::type>;
  using type = typename prepend_node<pipe_node<con, Rec>, Nodes>::type;
};

// fork 之后 all_to 接到支链上（主线已经写完了）。
template <bool Forked, class Nodes, class Branch, class Node>
struct builder_append {
  using nodes = typename prepend_node<Node, Nodes>::type;
  using branch = Branch;
};
template <class Nodes, class Branch, class Node>
struct builder_append<true, Nodes, Branch, Node> {
  using nodes = Nodes;
  using branch = typename prepend_node<Node, Branch>::type;
};

template <class Nodes, class Gen, class Branch = void>
struct meta_pipe_builder {
  static constexpr bool empty = (Nodes::size == 0);
  static constexpr bool forked = !std::is_void_v<Branch>;

  template <class Rec, class Con = default_node_con<Nodes::size == 0>>
  using all_to = meta_pipe_builder<
      typename builder_append<forked, Nodes, Branch,
                              pipe_node<typename unwrap_con<Con>::type,
                                        Rec>>::nodes,
      Gen,
      typename builder_append<forked, Nodes, Branch,
                              pipe_node<typename unwrap_con<Con>::type,
                                        Rec>>::branch>;

  // 分叉：从当前写到的位置再挂一条支链，与主线吃同一个源元素。
  // 之后写的 all_to 都接在支链上；run<> 把两边产物合成 pipe_fork<主线, 支链>。
  template <class Rec, class Con = default_node_con<false>>
  using fork = meta_pipe_builder<
      Nodes, Gen,
      typename prepend_node<pipe_node<typename unwrap_con<Con>::type, Rec>,
                            pipe_nodes<>>::type>;

  template <class Rec = void, class Con = void, class Main = pipe_main_co,
            class DriverPolicy = void>
  using run = meta_pipe_runner<
      typename unwrap_main<Main>::type,
      typename builder_run_nodes<Nodes, Rec, Con>::type, Gen, DriverPolicy,
      Branch>;

  using gen = Gen;
};

}  // namespace meta_stream_details

// ============================================================================
// 公开 API
// ============================================================================
namespace meta_ios {

using meta_stream_details::wait_for_end_con;
using meta_stream_details::wait_for_end_advance_con;
using meta_stream_details::node_con;
using meta_stream_details::pipe_stream_co;
using meta_stream_details::pipe_main_co;
using meta_stream_details::pipe_main_fn;
using meta_stream_details::pipe_view;

// 节点 con 的用户 Fn 可以返回 skip_t / yield_t / stop_t。
using meta_states_control::skip_t;
using meta_states_control::yield_t;
using meta_states_control::stop_t;
using meta_states_control::skip;
using meta_states_control::yield;
using meta_states_control::stop;

// 对 pipe_view 的常用投影，供 for_each<Ps...>(f) 使用。
namespace protocols {
template <class Stage>
using stream_to_t = typename Stage::to_t;
template <class Stage>
using stream_from_t = typename Stage::from_t;
template <class Stage>
using stream_cache_t = typename Stage::cache;
}  // namespace protocols

// ----------------------------------------------------------------------------
// meta_pipe<Gen>
//
// Gen 是管道最上游：任意元对象；默认 meta_empty_o 表示没有上游。
// 写的时候按 gen → 输出顺序追加 all_to，内部反过来 prepend（ranges 风格）。
// ----------------------------------------------------------------------------
template <class Gen = meta_objects::meta_empty_o>
  requires meta_objects::meta_object_t<Gen>
struct meta_pipe {
  template <class Rec, class Con = meta_stream_details::default_node_con<true>>
  using all_to = meta_stream_details::meta_pipe_builder<
      meta_stream_details::pipe_nodes<
          meta_stream_details::pipe_node<
              typename meta_stream_details::unwrap_con<Con>::type, Rec>>,
      Gen>;

  // fork：主链若干节点 + 支链若干节点，两者吃同一个源元素。
  // fork 之后再写的 all_to 都接在支链上。
  template <class Rec,
            class Con = meta_stream_details::default_node_con<false>>
  using fork = meta_stream_details::meta_pipe_builder<
      meta_stream_details::pipe_nodes<>, Gen,
      meta_stream_details::pipe_nodes<meta_stream_details::pipe_node<
          typename meta_stream_details::unwrap_con<Con>::type, Rec>>>;

  template <class Rec = void, class Con = void,
            class Main = meta_stream_details::pipe_main_co,
            class DriverPolicy = void>
  using run = meta_stream_details::meta_pipe_runner<
      typename meta_stream_details::unwrap_main<Main>::type,
      typename meta_stream_details::builder_run_nodes<
          meta_stream_details::pipe_nodes<>, Rec, Con>::type,
      Gen, DriverPolicy, void>;

  using gen = Gen;
};

}  // namespace meta_ios
