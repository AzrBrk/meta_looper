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
//   node0: 输入 = gen（默认 con 的 G_ADVANCE 开，推进 gen）
//   nodek: 输入 = 上一个节点的 rec'（默认 con 的 G_ADVANCE 关，中间只有 receiver）
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
inline constexpr std::size_t pipe_mask_shift = 64;

// 默认节点 con 的起始 flags：
//   ADVANCE 开：节点 0（它负责推进 gen）
//   ADVANCE 关：其余节点（中间没有 generator，只有 receiver）
//   EMIT 开、FLUSH 关
inline constexpr u128 node_seed_advance =
    u128{meta_states::condition_seed} | pipe_emit_bit;
inline constexpr u128 node_seed_static =
    u128{meta_states::condition_seed & ~meta_states::G_ADVANCE} | pipe_emit_bit;
// wait_for_end：活着时只接收不发射，死亡那一轮由引擎按 FLUSH 冲刷。
inline constexpr u128 node_seed_wait =
    u128{meta_states::condition_seed & ~meta_states::G_ADVANCE} | pipe_flush_bit;
inline constexpr u128 node_seed_wait_advance =
    u128{meta_states::condition_seed} | pipe_flush_bit;

// 造一个 pipe 节点 con：和 stream_states_co 同一套 stream_fn，只是换起始 flags。
// skip/yield 的归一化要连同 PIPE_EMIT 一起处理（节点 0 还带 ADVANCE）。
template <class Fn, u128 Flags>
using pipe_stream_co = meta_states_object<
    Flags, meta_states::state_fn<meta_ios_details::stream_fn<
              Fn,
              meta_states::G_RECEIVE | meta_states::G_NOT_SKIP | pipe_emit_bit,
              meta_states::G_RECEIVE | meta_states::G_NOT_SKIP |
                  pipe_emit_bit>>>;

template <bool Advance>
using default_node_con = pipe_stream_co<
    meta_ios_details::unchanged,
    Advance ? node_seed_advance : node_seed_static>;

// 用户自定义节点 con：Advance = true 时默认推进输入（节点 0），否则不推进。
template <class Fn = meta_ios_details::unchanged, bool Advance = false>
using node_con = pipe_stream_co<
    Fn, Advance ? node_seed_advance : node_seed_static>;

// wait_for_end：可以在中间段用（ADVANCE 关、EMIT 关、FLUSH 开）。
// 用在节点 0 时把 Advance 设成 true。
template <class Fn = meta_ios_details::unchanged, bool Advance = false>
using wait_for_end_con =
    pipe_stream_co<Fn, Advance ? node_seed_wait_advance : node_seed_wait>;

// 节点 0 用：wait_for_end 同时推进 gen。
template <class Fn = meta_ios_details::unchanged>
using wait_for_end_advance_con =
    pipe_stream_co<Fn, node_seed_wait_advance>;

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
// 主线占存活位图的 bit 0..62，bit 63 专门留给支链（所以 fork 时主线最多 63 节点）。
// 支链不参与 continue 判定：主线活着就继续；支链死活只体现在合并产物和位图里。
inline constexpr std::size_t pipe_branch_slot = 63;
inline constexpr u64 pipe_branch_bit = u64{1} << pipe_branch_slot;

// 合并产物：主线产物 + 支链产物。pipe_view 用 .main() / .branch() 取。
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

// Branch 可能是 void：只在非 void 时取 ::rec。
template <class T>
struct branch_rec {
  using type = void;
};
template <class T>
  requires (!std::is_void_v<T>)
struct branch_rec<T> {
  using type = typename T::rec;
};

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

template <std::size_t Count>
inline constexpr u64 initial_alive_mask =
    Count >= 64 ? ~u64{0} : ((u64{1} << Count) - 1);

// looper 的位级工具（gates 字节、门掩码查询），pipe 这一层直接复用。
namespace lb = meta_loop::meta_states_looper_bits::details;

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

// 上游没有形成产物：已死时只冲刷已有产物；仍存活时只记账和 tick。
template <class Con, class Rec, class Gen, bool HasValue, bool UpstreamDied,
          bool IsSource, class... Args>
struct pipe_node_step_impl<false, Con, Rec, Gen, HasValue, UpstreamDied, IsSource,
                           Args...> {
  static consteval auto condition_type() {
    if constexpr (UpstreamDied) {
      return std::type_identity<Con>{};
    } else {
      return std::type_identity<
          typename Con::template meta_set<flags_operator<
              meta_loop::meta_states_looper_bits::details::count_round(
                  Con::flags::value)>>>{};
    }
  }
  using con = typename decltype(condition_type())::type;
  using rec = Rec;
  using gen_out = Gen;
  static constexpr bool _receive_ = false;
  static constexpr bool just_died = UpstreamDied;
  static constexpr bool emit_flag = Con::flags::template test<pipe_emit_bit>;
  static constexpr bool flush_flag = Con::flags::template test<pipe_flush_bit>;
  // 没有产物就不要假装能吐一个值（否则会把未初始化 receiver 的 produce
  // 塞给下游，空源 + wait_for_end 就是这种情况）。
  static constexpr bool has_produce = exp_utilities::has_type<Rec>;
  static constexpr bool emitted = UpstreamDied && flush_flag && has_produce;
};
// 正常一步：四窗口逻辑。
template <class Con, class Rec, class Gen, bool HasValue, bool UpstreamDied,
          bool IsSource, class... Args>
struct pipe_node_step_impl<true, Con, Rec, Gen, HasValue, UpstreamDied,
                           IsSource, Args...> {
  // ---- 窗口① pre_gen_exec ----
  using pre_gen_exec_stage_o = invoke_window<pre_gen_exec, Con, Gen>;
  static constexpr u128 pre_gen_judge =
      meta_loop::meta_states_looper_bits::details::effective_value(pre_gen_exec_stage_o::flags::value);
  static constexpr u8 pre_gen_gates = lb::gates_of(pre_gen_judge);

  static constexpr bool _entered_ = pre_gen_gates & lb::gate_continue;
  static constexpr bool _advance_ = lb::gates_have(
      pre_gen_gates, lb::gate_continue | lb::gate_advance, lb::gate_finish);

  // 节点 0 默认 ADVANCE 开 → 真正推进 gen；
  // 中间节点默认 ADVANCE 关 → invoke_if<false> 不调用上游（上游已经更新过）。
  using generator_stage_o = meta_invoke<invoke_if<_advance_>, Gen, Args...>;

  // ---- 窗口② post_gen_exec ----
  static constexpr bool _pre_gen_finished_ = pre_gen_gates & lb::gate_finish;
  using post_gen_exec_stage_o = enter_window_step<
      !_pre_gen_finished_, post_gen_exec, pre_gen_exec_stage_o,
      generator_stage_o>;
  static constexpr u128 post_gen_judge =
      meta_loop::meta_states_looper_bits::details::effective_value(post_gen_exec_stage_o::flags::value);

  // ---- 窗口③ pre_rec_exec ----
  static constexpr u8 post_gen_gates = lb::gates_of(post_gen_judge);
  static constexpr bool _post_gen_finished_ = post_gen_gates & lb::gate_finish;
  using pre_rec_exec_stage_o = enter_window_step<
      !_post_gen_finished_, pre_rec_exec, post_gen_exec_stage_o, Rec>;
  static constexpr u128 pre_rec_judge =
      meta_loop::meta_states_looper_bits::details::effective_value(pre_rec_exec_stage_o::flags::value);
  static constexpr u8 pre_rec_gates = lb::gates_of(pre_rec_judge);

  // 上游没有发值 / 已经死亡时都不接收（死亡那一轮仍可接收最后一次冲刷值）。
  // 最后一个条件是窗口②的 G_FINISH，就是 !_post_gen_finished_，直接复用。
  static constexpr bool _receive_ =
      HasValue & _entered_ & !_post_gen_finished_ &
      lb::gates_have(pre_rec_gates, lb::gate_continue | lb::gate_receive);

  using result_stage_o =
      meta_invoke<invoke_object_if<_receive_>, Rec, generator_stage_o>;

  // ---- 窗口④ post_rec_exec ----
  static constexpr bool _pre_rec_finished_ = pre_rec_gates & lb::gate_finish;
  using post_rec_exec_stage_o = enter_window_step<
      !_pre_rec_finished_, post_rec_exec, pre_rec_exec_stage_o,
      result_stage_o>;
  static constexpr u128 post_rec_judge =
      meta_loop::meta_states_looper_bits::details::effective_value(post_rec_exec_stage_o::flags::value);
  static constexpr u8 post_rec_gates = lb::gates_of(post_rec_judge);

  static constexpr bool _continue_ =
      _entered_ && lb::gates_have(post_rec_gates, lb::gate_continue);
  static constexpr bool just_died = UpstreamDied || !_continue_;

  using settle_flags = meta_states_settle<typename post_rec_exec_stage_o::flags>;
  using con = typename post_rec_exec_stage_o::template meta_set<settle_flags>;
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
      has_produce && ((_receive_ & emit_flag) | (just_died & flush_flag));
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
  static constexpr bool has_value = false;
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
  static constexpr bool has_value = true;
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

  using node_result = pipe_fold_node<
      alive, IsOutput, is_input_most, Index, inner::mask, typename inner::gen_out,
      typename inner::out, inner::has_value, inner::just_died, Head, Args...>;

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
// 支链的一轮：与主线吃同一个出入值（不推进 gen），自己跑四窗口。
template <bool Alive, class Branch, class Gen, class... Args>
struct pipe_branch_fold {
  using con = typename Branch::con;
  using rec = typename Branch::rec;
  static constexpr bool died = false;
};
template <class Branch, class Gen, class... Args>
struct pipe_branch_fold<true, Branch, Gen, Args...> {
  // 支链是它自己那条链的入口（拿到的就是源的当前状态），所以 IsSource = true：
  // 这只影响“Gen 是否有效”，推进与否仍看支链自己的 G_ADVANCE。
  using step = pipe_node_step<typename Branch::con, typename Branch::rec, Gen,
                              true, false, true, Args...>;
  using con = typename step::con;
  using rec = typename step::rec;
  static constexpr bool died = step::just_died;
};
// 没 fork（或支链已死）时不去碰 Branch::con。
template <bool Forked, bool Alive, class Branch, class Gen, class... Args>
struct pipe_branch_result {
  using con = void;
  using rec = void;
  static constexpr bool died = false;
};
template <bool Alive, class Branch, class Gen, class... Args>
struct pipe_branch_result<true, Alive, Branch, Gen, Args...> {
  using folded = pipe_branch_fold<Alive, Branch, Gen, Args...>;
  using con = typename folded::con;
  using rec = typename folded::rec;
  static constexpr bool died = folded::died;
};

template <class Main, class Nodes, class Gen, class Branch = void,
          class... Args>
struct pipe_round {
  static constexpr bool forked = !std::is_void_v<Branch>;
  static constexpr u64 mask_raw = Main::flags::value.high;
  // 主线只认自己的节点位。
  static constexpr u64 mask0 =
      mask_raw & ~u64{forked ? pipe_branch_bit : 0};
  static constexpr std::size_t index =
      Main::flags::value.low & meta_states::position_mask;

  using folded = pipe_fold<Nodes, Nodes::size - 1, true, mask0, Gen, true,
                           false, Args...>;

  // 支链：吃主线本轮刚消费的那个元素（folded::gen_out 就是推进后的源）。
  // 支链自己的 G_ADVANCE 关，所以它不会再推进一次。
  static constexpr bool branch_alive =
      forked && ((mask_raw & pipe_branch_bit) != 0);
  using branch_result =
      pipe_branch_result<forked, branch_alive, Branch,
                         typename folded::gen_out, Args...>;
  using branch =
      std::conditional_t<forked,
                         pipe_node<typename branch_result::con,
                                   typename branch_result::rec>,
                         void>;

  using nodes = typename folded::nodes;
  using out = std::conditional_t<forked,
                                 pipe_fork<typename folded::out,
                                           typename branch_result::rec>,
                                 typename folded::out>;
  using prev = typename folded::prev;
  using gen_out = typename folded::gen_out;

  // 支链存活位折回 bit 63；主线位图单独交给 main 判定 continue。
  static constexpr u64 mask_main =
      folded::mask & ~u64{forked ? pipe_branch_bit : 0};
  static constexpr u64 mask_after =
      mask_main | u64{forked && !branch_result::died ? pipe_branch_bit : 0};

  // 把新的存活位图写进 main 的高 64 位，并记一步 position。
  // position 是低 16 位计数器：+1 后按掩码回绕，其余位原样。
  static constexpr u64 low_before = Main::flags::value.low;
  static constexpr u64 low_counted =
      (low_before & ~meta_states::position_mask) |
      ((low_before + 1) & meta_states::position_mask);
  using flags_counted = flags_operator<u128{low_counted, mask_main}>;
  using main_staged = typename Main::template meta_set<flags_counted>;
  // main 的算法接收整个节点对象，与 MSO 观察者的对象调用协议一致。
  using main_out_raw = meta_invoke<main_staged, typename folded::out>;
  // position 与存活位图由引擎维护，不能由 main 的 Fn 覆盖。
  static constexpr u128 main_owned_mask =
      u128{meta_states::position_mask} | (~u128{0} << pipe_mask_shift);
  using main_out_restored = typename main_out_raw::template meta_set<
      restore_owned_flags<main_owned_mask, flags_counted,
                          typename main_out_raw::flags>>;
  // 写回时把支链位放回去（main 看不到它，也不会被它影响 continue）。
  using main_out = typename main_out_restored::template meta_set<
      flags_operator<u128{main_out_restored::flags::value.low, mask_after}>>;

  static constexpr bool has_value = folded::has_value;
  static constexpr bool middle_died = folded::middle_died;
  static constexpr bool output_died = folded::output_died;
  static constexpr bool continue_ =
      !middle_died && main_out::flags::template test<meta_states::G_CONTINUE>;
};

// ============================================================================
// 6) 最终 / 每轮的视图
// ============================================================================
template <class To, class From, class States, std::size_t Index,
          class Driver = void>
struct pipe_view {
  using to = To;
  using from = From;
  using states = States;
  using driver = Driver;
  using to_t = get_type<to>;
  using from_t = get_type<from>;
  using cache = meta_produce_t<from>;

  // fork 视图：主线/支链产物。未 fork 时 forked == false，main_t == to_t。
  using parts = fork_parts<to>;
  static constexpr bool forked = parts::forked;
  using main_t = get_type<typename parts::main_t>;
  using branch_t = get_type<typename parts::branch_t>;

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
                                  Round::index, Driver>;

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
// pipe 的 step 适配器把 Round::has_value/continue_ 交给 driver policy。
// ============================================================================
template <class Round>
struct pipe_driver_step {
  using round = Round;
  static constexpr bool continue_ = Round::continue_;
  static constexpr bool observe_ = Round::has_value;
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
  static constexpr std::size_t rounds =
      Driver::flags::template get<u8, driver_control::remaining_shift> + 1;

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
      return any_within<step::observe_, N - 1, rest>::value;
    }
  }

  template <std::size_t N>
  static consteval bool future_within_args() {
    if constexpr (phase != driver_control::phase_run) {
      return false;
    } else if constexpr (N == 0) {
      return false;
    } else if constexpr (rest::template observes_within_fn<N>()) {
      return true;
    } else if constexpr (rest::has_more && N > rest::rounds) {
      return pipe_flags_driver<
          typename rest::next_driver,
          typename rest::next_state>::template observes_within_fn<
          N - rest::rounds>();
    } else {
      return false;
    }
  }

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

  template <class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   A&&... a) {
    if constexpr (phase == driver_control::phase_done) {
      if constexpr (step::observe_) {
        return std::invoke(f, view{}, std::forward<First>(first));
      } else {
        return;
      }
    } else if constexpr (phase == driver_control::phase_more) {
      using driver = pipe_flags_driver<next_driver, next_state>;
      if constexpr (step::observe_) {
        using return_type =
            std::invoke_result_t<F&, view, First>;
        if constexpr (is_for_each_stop_v<return_type>) {
          return std::invoke(f, view{}, std::forward<First>(first));
        } else if constexpr (is_for_each_decision_v<return_type>) {
          if constexpr (driver::template observes_within_fn<
                            sizeof...(A)>()) {
            (void)std::invoke(f, view{}, std::forward<First>(first));
            return driver::for_each_forward(std::forward<F>(f),
                                            std::forward<A>(a)...);
          } else {
            return std::invoke(f, view{}, std::forward<First>(first));
          }
        } else if constexpr (driver::template observes_within_fn<
                                 sizeof...(A)>()) {
          if constexpr (std::is_void_v<return_type>) {
            std::invoke(f, view{}, std::forward<First>(first));
            return driver::for_each_forward(std::forward<F>(f),
                                            std::forward<A>(a)...);
          } else {
            (void)std::invoke(f, view{}, std::forward<First>(first));
            return driver::for_each_forward(std::forward<F>(f),
                                            std::forward<A>(a)...);
          }
        } else {
          return std::invoke(f, view{}, std::forward<First>(first));
        }
      } else {
        if constexpr (driver::template observes_within_fn<
                          sizeof...(A)>()) {
          return driver::for_each_forward(std::forward<F>(f),
                                          std::forward<A>(a)...);
        } else {
          return;
        }
      }
    } else {
      if constexpr (step::observe_) {
        using return_type =
            std::invoke_result_t<F&, view, First>;
        if constexpr (is_for_each_stop_v<return_type>) {
          return std::invoke(f, view{}, std::forward<First>(first));
        } else if constexpr (is_for_each_decision_v<return_type>) {
          if constexpr (future_within_args<sizeof...(A)>()) {
            (void)std::invoke(f, view{}, std::forward<First>(first));
            return rest::for_each_forward(std::forward<F>(f),
                                          std::forward<A>(a)...);
          } else {
            return std::invoke(f, view{}, std::forward<First>(first));
          }
        } else if constexpr (future_within_args<sizeof...(A)>()) {
          if constexpr (std::is_void_v<return_type>) {
            std::invoke(f, view{}, std::forward<First>(first));
          } else {
            (void)std::invoke(f, view{}, std::forward<First>(first));
          }
          return rest::for_each_forward(std::forward<F>(f),
                                        std::forward<A>(a)...);
        } else {
          return std::invoke(f, view{}, std::forward<First>(first));
        }
      } else {
        if constexpr (future_within_args<sizeof...(A)>()) {
          return rest::for_each_forward(std::forward<F>(f),
                                        std::forward<A>(a)...);
        } else {
          return;
        }
      }
    }
  }
};

template <class Driver, class Main, class Nodes, class Gen, class Branch>
struct pipe_flags_driver<Driver, pipe_state<Main, Nodes, Gen, Branch>> {
  using chunk = pipe_flags_chunk<Driver, pipe_state<Main, Nodes, Gen, Branch>>;

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

  template <std::size_t N>
  static consteval bool observes_within_fn() {
    if constexpr (N == 0) {
      return false;
    } else if constexpr (chunk::template observes_within_fn<N>()) {
      return true;
    } else if constexpr (chunk::has_more && N > chunk::rounds) {
      return pipe_flags_driver<
          typename chunk::next_driver,
          typename chunk::next_state>::template observes_within_fn<
          N - chunk::rounds>();
    } else {
      return false;
    }
  }

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
  static_assert(Nodes::size >= 1,
                "meta_pipe: run<> 需要至少一个主线节点");
  // fork 时 bit 63 留给支链，主线只能用 bit 0..62。
  static_assert(Nodes::size + (forked ? 1 : 0) <= 64,
                "meta_pipe: 存活位图只有 64 位（fork 时 bit 63 给支链）");

  using policy = std::conditional_t<
      std::is_void_v<DriverPolicy>,
      default_pipe_driver_policy<Main, Nodes, Gen, Branch>, DriverPolicy>;

  using main_init = typename Main::template meta_set<
      typename Main::flags::template set<
          u64, pipe_mask_shift,
          initial_alive_mask<Nodes::size> | u64{forked ? pipe_branch_bit : 0}>>;
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

template <class Nodes, class Gen, class Branch = void>
struct meta_pipe_builder {
  static constexpr bool empty = (Nodes::size == 0);
  static constexpr bool forked = !std::is_void_v<Branch>;

  template <class Rec, class Con = default_node_con<Nodes::size == 0>>
  using all_to = meta_pipe_builder<
      typename prepend_node<pipe_node<typename unwrap_con<Con>::type, Rec>,
                            Nodes>::type,
      Gen, Branch>;

  // 分叉：从当前写到的位置再挂一条支链（单节点），与主线吃同一个源元素。
  // 支链不再接节点；run<> 把两边产物合成 pipe_fork<主线, 支链>。
  template <class Rec, class Con = default_node_con<false>>
  using fork = meta_pipe_builder<
      Nodes, Gen, pipe_node<typename unwrap_con<Con>::type, Rec>>;

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

  // fork：主链一个节点 + 支链一个节点，两者吃同一个源元素。
  template <class Rec,
            class Con = meta_stream_details::default_node_con<false>>
  using fork = meta_stream_details::meta_pipe_builder<
      meta_stream_details::pipe_nodes<>, Gen,
      meta_stream_details::pipe_node<
          typename meta_stream_details::unwrap_con<Con>::type, Rec>>;

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
