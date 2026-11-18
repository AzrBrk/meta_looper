# 交接：全面退役 u128（就地改到双字模型）

> 工作目录：`G:\cpp\meta_looper_main`（下一会话直接切到这里）。
> 远程仓库有 backup，中间破坏态没关系。用户最终会 review 全部 diff。

## 0. 任务目标

**全面退役 u128**：把 `flags_operator<u128>`（128 位 flags 载体）**和 `u128` 类型本体**都退掉，
因为 u128 是类类型、每次位运算都是一次 constexpr 函数调用 + 对象构造，比原生 u64 慢一个数量级（设计文档 D1）。

策略是**就地改**（不改文件名/公共 API 名字，内部换引擎），不是换引擎到 `meta_looper64`。
最终 `flags_operator` 只剩 `u64` 载体，looper/stream/pipe/binder/series/aligned/bank 全部落到「两个 u64 字」模型。

## 1. 先读这些（设计已定，不用重新讨论）

- `AGENTS.md` —— 硬规则（禁 tuple/index_sequence/conditional_t，改基础件前先读 meta_states_looper.md 等）。
- `design/03_state_words.md` —— 双字模型的目标布局。
- `design/08_decisions.md` —— D1~D19，每条决策的「为什么」（尤其 D1 u128→u64、D5 门位改正向、D6 POS u32、D7 timer target 用类型、D8/D9 MAIL/communicate、D14 只传 u64）。
- `design/06_migration.md` / `design/07_plan.md` —— 旧 API 映射 + 阶段计划（阶段号已过时，看下面的「已完成/下一步」）。

## 2. 双字模型（目标形态，核心记忆点）

```
looper word (u64): GATES(8) WINDOW(2) TARGET0(3) TARGET1(3) T0C(8) T1C(8) POS(32)
user word   (u64): MAIL(16) USER(48)
```

- 门位**全部正向**，在 looper word 的 GATES 字节（bit 0..7）：`cont/rec/gen/obs/finish/fwd/init/consume`（见 `meta_core64::gates`）。旧名 `G_NOT_SKIP` = 新 `obs`。
- 计时器 **count 进 T0C/T1C、target 下标进 TARGET0/TARGET1**（`apply` 装表时同窗清目标门，FINISH 永不压；装表接口仍是类型 `set_suppression_timer<TargetBit>`）。
- **条件（condition）是纯 Fn**，不再是 MSO：

```cpp
template <u8 Window, u64 L, u64 U, class Obj>
static constexpr u64 value;   // 返回 user word（一条指令）
```

- 复用 `meta_core64`（lw/uw/gates/isa/apply/communicate）+ `meta_loop64`（step/state/run）+ `meta_looper64`（drive/walker）+ `meta_stream64`（stream_cond64/ops/exactly_at）。这 6 个新模块**已经写好且 6 个 test 全绿**，是靶子。

## 3. 已完成并验证（不要再动）

| 文件 | 状态 |
|---|---|
| `meta_states_looper.hpp`（649 行） | ✅ 核心引擎重写到双字，复用 `meta_core64`+`meta_loop64::step`，保留 `driver_control`/`flags_chunk`/`flags_driver` 预算分块逻辑。`meta_states_looper_step<State>` 暴露 `continue_/observe_/consume_/next/condition/receiver/generator/stage`。 |
| `meta_states.hpp`（87 行） | ✅ 条件层改成 Fn 模型：`make_condition`/`states_executor`/`ret_origin`/`run`/`pack_stream`/`user_fn::index_of`。 |
| `meta_objects.hpp` | ✅ 只改了 `states_executor` 签名（旧 `apply<flags,obj>` → 新 `value<Window,L,U,Obj>`）。u128 载体和 u128 MSO **还在**，等 P4 删。 |
| `meta_stream.hpp`（560 行）+ `test/pipe64_test.cpp` | ✅ 6.1 重做完成：pipe 落到双字模型（liveness bitmap 作 state 第三路 u64，emit/flush 在 USER 46/47）。builder + `pipe_node_step`(跑 `meta_loop64::step`) + `pipe_fold` + 位图驱动 + emit/flush 语义全通，8 用例 PASS。公开入口 `meta_pipe<gen>::all_to<rec,con>::run<rec,con,main_con>`，`for_each` 已支持值平面（返回末值 / `stop_t`）。 |
| `meta_stream_control.hpp` | ✅ 6.2 完成：`stream_states_co<Fn>` 改成 `meta_stream64::stream_cond64<Fn>` 的别名（条件=Fn，不再 MSO）。保留字节（bits 120..127）随 u128 一起消失；timer 判定改为按窗口从 `meta_timer_object_t` 读。 |
| `meta_stream_algorithms.hpp` | ✅ 6.2 完成：`filter`/`take` 的 control 从 u128 换成单 u64；`filter_condition_fn`/`take_condition_fn` 改成 Fn（`value<Window,L,U,Obj>`）。`transform`/`fold`/`collect` 无 u128，原样保留。 |
| `meta_stream.hpp` 补丁 | ✅ 6.2 顺手修：`node_step_cond` 的 `!HasValue` 分支补清 OBS（原来只清 REC），否则 filter 吞掉的轮次仍被下游 collect 观察，`f_calls` 会是 5 而不是 3。 |
| `meta_states.hpp` 计时器/工具 | ✅ 6.3 完成 + 本次会话修正 target 进字：`timer_target<Bit>`（类型，供 `set_suppression_timer` 装表）、`timer_count<Slot>(looper)`、`tick_timers(looper)`（= `meta_loop64::tick`）、`effective(looper)`（无模板参数，target 从字里读，返回有效 gates 字节，FINISH 永不压制）、`set_suppression_timer<TargetBit>(word,count)`（返回 TIMER 指令）、`exactly_at_co<I>`、`args_at<I>(args...)`。**本次修正**：D7 原「target 用类型」导致引擎 `apply` 不知道 target、压制延后、压不住第一轮 gen——改成 target 下标进字（WINDOW 缩到 2 位，空出 6 位给 TARGET0/TARGET1），`apply` 装表时**当场**清目标门；`effective` 降为无模板。`test/meta_states_timer_test.cpp` 同步更新（装表后 `lw::gates_v(L_arm)==defaults&~rec` 断言同窗压制）。11 个 test 全绿。 |
| 基础设施（对齐/序列/银行 + 缺失导出） | ✅ 6.3.5 完成（本次会话，先把 example 依赖的基础件迁完，不急着迁 example）：<br>1. **`meta_states.hpp`** 补回 `stop_t`/`skip_t`/`yield_t`/`stop()`/`yield()`/`skip()` 的 `using meta_states_control::*` 导出——§3 重写时漏了，`function_series` 等一票 example 依赖它。<br>2. **`type_safe_aligned_iterator.hpp`**：`field_size` 改走 `meta_pipe<...>::run<meta_aligned_iterator<A>>::type::to`（取末 seek_to 的 advance_t）；`largest_aligned_type` 换成 `max_alignof_v` 纯 fold；128 位匹配位图从 u128 MSO 改成 `mask_details::type_mask`（两 u64 `low/high`）+ 直接 pack fold（`type_mask_series`）；`initialize` 的 condition 从 `stream_states_co<>` 换成 `node_con<unchanged,true>`（`meta_aligned_iterator` 靠接收初始化，`stream_cond64` 的 window-2 init-failure 规则会误停）。新增 `test/tsar_migration_test.cpp`（field_size/bitmap/initialize/destroy/get，含引用槽）。<br>3. **`meta_function_series.hpp`**：`last_round_condition<N>` 旧 MSO → 新 Fn（window 2 用 `with_obs`/`stop`）；`stages_loop` 的 `meta_aligned_iterator<>` 未初始化接收器改 `node_con<unchanged,true>`。<br>4. **`meta_static_function_series.hpp`**：`argument_condition<Index>` 旧 MSO → 新 Fn（等价 `exactly_at<Index>`）；`dispatch` 原来的 `meta_pipe::fork`（**新 pipe 6.1 没实现 fork**）改成单槽逐轮遍历（`slot_receiver` + `index_sequence_istream<Count>`，callback 用 `view.object()`），去掉 fork 依赖。<br>5. **`meta_segment_bank.hpp`**：删 `import_u128`/`export_u128`（两 bank 打包桥，死代码，只剩 bench 参考在用）；删 `#include "meta_uint128.hpp"`。<br>6. **`meta_objects.hpp`**：删 `bank_u128_flags`（死代码）。<br>验收：11 个 test（含 3 个新增）全绿；`examples/*.cpp` 现 29 编译过 / 32 失败（失败的几乎全是旧 MSO/u128 写法的 example，属 §6.4）。 |
| 验证 | 端到端：`run<make_condition<states_executor<stop_after_3>>, meta_empty_o>::for_each` 恰好 3 次回调，`static_assert` 通过。新增 `test/stream_algorithms64_test.cpp`（filter/take/transform/fold/collect + 观察计数）PASS；`test/pipe64_test.cpp` 及 6 个既有 test 全绿。 |

**关键语义已确认没跑偏**：`observe = pre_rec 的 OBS 门 && post_rec 的 CONT 门`（和旧 `_observe_ = _pre_rec_observe_ && _continue_` 一致）。
注意 **off-by-one**：清 CONT 的那一轮**不**被 observe（比如要跑 N 个被观察轮，应在 `POS >= N` 时 stop）。

## 4. 当前破坏态（预期中间态，继续从这修）

- ~~pipe / 流层 / 计时器 / 基础设施~~ 全部完成（见 §3）。
- **pipe 已补 chunk 驱动 + for_each_forward + fork**：`pipe_budget`(64) + `pipe_chunk` + `pipe_driver`（惰性 consteval 递归），2000 轮 pipe 编译+运行通过；`observes` 惰性 bool 修了 constexpr 深度；`for_each_forward` 跨 chunk 用 `forward_tail`；`fork` 完成。
- **`no_generator` + 计时器 target 进字 + flush 观测** 已修（见坑 #17/#18/#21）。
- 剩 2 个失败 example：
  - `examples/pipe_smoke.cpp`：`filter_int`/`kill_at_one` 旧 MSO 条件没迁（机械迁 Fn 即可），`held`（wait_for_end + meta_rostream 嵌套列表）断言。
  - `examples/print.hpp` + `print_smoke.cpp`：u128 `ready/delivered` 状态机重写（`print.hpp:38` 还在调旧的 `set_suppression_timer<...>` 五参数老签名）。
- `flags_operator<u128>` + `u128` 类型本体：**还没删**（`print.hpp` 等仍引用 u128，§6.4 迁完才能删）。
- `main.cpp`（根目录，用户 scratch）：本会话重写成测试「用户自定义函数 + 抑制计时器」（`ignore_for` + `ops::timer`），已编译过。`std::tuple<>` 是 bench 用途，规则 1 允许，别改。

## 5. 编译/验证命令

```bash
# 单个文件语法检查（-I. 必须，boost 路径见 compile_flags.txt）
g++ -std=c++23 -I. -Wall -Wextra -pedantic-errors -fsyntax-only <file.cpp>

# 最小端到端（新引擎跑通的最小验证，可参考）
# 条件 = meta_stream64::exactly_at<2> 或自写 stop_after_N
```

- **D11：重构期不跑全量 smoke、不跑 deep_smoke**。用 `test/` 下的独立用例验收。等全部迁完再一次性全量 smoke。
- `compile_flags.txt`：`-I. -IG:/boost_1_84_0/boost_1_84_0 -D_WIN32_WINNT=0x0601`。

## 6. 下一步（严格按此顺序，每步编译验证）

### 6.1 重做 pipe —— ✅ 已完成（见 §3；下一步从 §6.2 开始）

从**公开 builder 往下写**，别先写底层：

1. **builder**：`meta_pipe<gen>::all_to<rec, con>::run<rec, con, main_con>` 链式构建，产出 `pipe_state` + `meta_pipe_runner`（公开入口：`for_each`/`type`）。旧的 builder 逻辑在旧 `meta_stream.hpp` 的 `builder_run_nodes`/`meta_pipe_builder`（已被我覆盖，可用 `git show HEAD:meta_stream.hpp` 看原版参考）。
2. **pipe_node_step**：一个节点跑 `meta_loop64::step`，条件 = 该节点的 con Fn（用户 Fn 用 `meta_stream64::stream_cond64` 包一层流规则）。
3. **pipe_fold**：从 input-most 往 output 折叠，线程化 upstream produce；死节点不跑、不发。
4. **位图驱动**：liveness bitmap 是**独立的 u64**（state 第三路字），初始全存活，节点死了清对应位，main 在 bitmap==0 时 stop。驱动可用 `meta_looper64` 的 data-driven 或复刻 `flags_chunk` 预算分块（深度安全）。
5. **emit/flush 语义**：emit/flush 放 **USER 位 46/47**（`pipe_emit_bit=u64{1}<<46`、`pipe_flush_bit=u64{1}<<47`）；过滤=清 emit，wait_for_end=flush。这部分语义很细，参考旧 `meta_stream.hpp` 的 `pipe_node_step_impl`/`pipe_fold_node`（`git show HEAD:meta_stream.hpp`）。

**pipe 关键设计决策（已定，review 时重点看）**：liveness bitmap 64 位装不进 USER 48 位 → 作为 state 里独立的一路 u64（第三字）携带，64 节点语义不动。

### 6.2 流层剩余 —— ✅ 已完成（见 §3；下一步从 §6.3 开始）

- `meta_stream_control.hpp`（`stream_states_co` 等，3 处 u128）→ 迁到双字，条件改成 Fn。
- `meta_stream_algorithms.hpp`（filter/take，13 处 u128）→ 同上。

**§6.2 关键改动（review 时重点看）**：
- `stream_states_co<Fn>` = `meta_stream64::stream_cond64<Fn>`（条件从 MSO 变 Fn，保留字节消失）。
- `take` 的 control 变单 u64（旧 `.low/.high` 合并）；`take_condition_fn` **只在 `pre_rec_exec` 清 CONT**（不是 post_rec），否则清 CONT 那一轮不被 observe（off-by-one），`take<2>` 会只回调 1 次。
- `filter_condition_fn` 在 `post_rec_exec` 按 control 重挂/清 `PIPE_EMIT`（keep 重挂、drop 清）；这是 filter 真正吞值的机制。
- 顺手修 `node_step_cond`：`!HasValue` 分支补清 OBS。

### 6.3 补 `meta_states.hpp` 计时器/工具 —— ✅ 已完成（见 §3；下一步从 §6.4 开始）

`set_suppression_timer`/`effective`/`tick_timers`/`timer_count`/`timer_target`/`args_at`/`exactly_at_co` —— timer target 下标进字（`TARGET0/TARGET1` 各 3 位），`apply` 装表同窗清目标门；`effective(looper)` 无模板参数。`ops::timer` 第三参是**位下标**不是位值（`set_suppression_timer<gates::rec>` 内部 `countr_zero`）。

### 6.4 迁 61 个 example

executor 从 `template<class flags, class obj> struct apply`（读 flags_operator<u128>）改成 `template<u64 L, u64 U, class Obj> static constexpr u64 value`（返回 user word）。条件从 MSO 改成 `make_condition<states_executor<my_exec>>`（新 Fn 模型）。

### 6.5 收口（P4）

- 删 `flags_operator<u128>` 载体 + `u128` MSO + `normalize_flags` 的 u128 分支（`meta_objects.hpp`）。
- 删 `meta_uint128.hpp`（`u128` 类型本体）。~~原本还卡在 `type_safe_aligned_iterator.hpp` 的 128 位匹配位图、`meta_segment_bank.hpp` 的两 bank 打包——这俩已在 6.3.5 迁掉（位图改两 u64 `type_mask`，bank 的 `import_u128`/`export_u128`/`bank_u128_flags` 是死代码直接删），所以这步现在只剩删 `flags_operator<u128>` + `u128` MSO + `meta_uint128.hpp`。~~ 注意 `bench/legacy_control_reference.hpp`（bench 参考）还在用 `import_u128`，删 `meta_uint128.hpp` 前要把它一起处理或接受 bench 破。`examples/print.hpp` 等 32 个失败 example 也直接引用 `u128`，所以 **§6.4 迁完 example 之前不能删 `meta_uint128.hpp`**。
- 同步 `meta_states_looper.md` §19（还写着旧 128 位 driver 布局）。
- 一次性全量 smoke。

## 7. 已踩过的坑（下次别再踩）

1. **`states_executor` 命名冲突**：`meta_objects::states_executor` 和 `meta_states::states_executor` 同时存在会歧义。已把 `meta_objects::states_executor` 改成新签名（`value<Window,L,U,Obj>`），`meta_states.hpp` 里 `using meta_objects::states_executor;` 复用即可，**别再定义新的**。
2. **`template <template <u8> class Fn>` 不匹配**：用 `template <template <auto> class Fn>`（`stop_after_3` 是 `template<u8> struct`）。
3. **observe off-by-one**：清 CONT 的那一轮不 observe。
4. **`flags_operator<0x1234>`（int 字面量）会选中未定义的 primary**（`template<auto>`）→ 必须显式 `u64{...}` 或 `u128{...}`。这只影响已废弃的 u128 路径，迁完 example 后自然消失。
5. `meta_loop64::step` 的 `next` 是 `state<L4, R3, rec1, gen1, Fn>`（`meta_loop64::state`），**不携带** pipe 的 bitmap——pipe 必须用自己的 state 类型 + 自己的 step，不能直接复用 `meta_states_looper_step`。
6. **pipe 的 receiver 靠接收来初始化**，但 `stream_cond64` 的 window-2 规则把「未初始化且不能零参初始化」的 receiver 当 init 失败。**旧解**是 `node_con_t` 在 window-2 对 `uninitialized_meta_object_t` 补回 `CONT|REC`——这条 workaround 已删，正确解是补回 `no_generator` 检查（见坑 #17）：只有「没有生成器」时 init 失败才算失败，有生成器就靠接收初始化。
7. **"received" 信号别用类型恒等**（`rec1 != Rec` 在 `meta_istream_list<int,int>` 这种重复类型下误判成没收到）。pipe 用 `HasValue && st::cont` 当 received，且 `has_value`（上游本回是否 emit）必须线程化到下游——否则「过滤=清 emit」会泄漏被吞掉的值。
8. **`!HasValue` 要连 OBS 一起清**：`node_step_cond` 里上游吞值（filter 清 emit）那一轮，下游节点原来只清 REC、不清 OBS，导致吞掉的轮次仍被观察（filter→collect 会回调 5 次而非 3 次）。已在 6.2 补上 `with_obs(w,false)`。
9. **`take_con` 只在 `pre_rec_exec` 清 CONT**：在 post_rec 清会让「收到第 N 个值的那一轮」不 observe（off-by-one），`take<N>` 少回调一次。正确做法是 pre_rec 看到 `control==0` 再 `stop()`，这样第 N 轮仍 observe、第 N+1 轮才停。
10. **`exp_utilities::take` 撞名**：`meta_ios::take`（N 收）和 `exp_utilities::take<N,TL>`（列表切片）同名。写测试/示例时别 `using namespace exp_utilities`，用 `using exp_utilities::exp_list;` 这种选择性引入（`composition_smoke.cpp` 就这么干的）。
11. **`meta_loop64::step` 的 `halted1` 没锁 window-0 的 CONT**：`stream_cond64` 在 window-0（pre_gen）对耗尽计时器（`meta_timer_object` count==0）清 `CONT|REC|GEN`，但 `halted1` 原来只看 `g1`（window-1），window-1 重算默认门位把 CONT 又开回来 → 生成器永远停在 count==0 不前进也不停 → 模板深度爆 900。已修：`halted1 = (g0 & cont)==0 || (g1 & cont)==0`（window-0 的 halt 也要锁）。`timer_object_smoke.cpp` 由深度爆炸变为 PASS。注意：`meta_timer_object` 的 `stop_forward_next_if_break_f_is_true` 已初始化特化 `requires(!is_same_v<timer, meta_break_signal>)` 是**故意**让 count==0 下溢复活的（`timer_object_smoke` 第 42 行 `revived::timer::value` 断言了这一点），别去"修"它——正确做法是流在 advance 之前判死，即上面这个 `halted1` 锁。
12. **`meta_states::run` 缺 `stop_t/skip_t/yield_t` 导出**：§3 重写时漏了 `using meta_states_control::*`，example 里 `meta_states::stop_t` 全挂，已补回（见 §3 基础设施行）。
13. **example 迁移口诀**：旧 `apply<Window,Flags,Object>` → 新 `value<Window,L,U,Obj>`（返回 user word）；`Flags::get<u16,position_shift>` → `lw::pos_v(L)`；条件返回 `skip_t/yield_t/stop_t` → `ops::skip/yield/stop`；`Flags::value & ~G_CONTINUE` → `ops::stop(w)`；未初始化接收器（`meta_aligned_iterator`）的 looper 条件用 `node_con<unchanged,true>`（不是 `stream_states_co<>`）。
14. **observe 必须在 window-2（pre_rec）设 OBS，不是 window-1（post_gen）**：新模型门位是脉冲（D12），window-1 设的 OBS 到 window-2 就被 condition 重建掉了，`observed = g2 & obs` 读的是 window-2。旧 pipe 的 `select_rounds` 用 `post_gen_exec` 是靠旧模型 flags 持久才生效的；迁到新模型后 pipe 的 `select_rounds` 必须改成 `pre_rec_exec`（looper 本来就是 pre_rec）。
15. **`meta_pipe` 的 `for_each_forward` 已补**（6.1 漏了）：`meta_pipe_runner::for_each_forward` + `pipe_forward_walker` + `pipe_observes_within<State,Cont,N>`（消参 lookahead，照搬 chunked driver 的 `observes_within_fn`：N==0 false / observe true / N==1 false / 递归 N-1）。pipe 无 G_CONSUME 门，每轮都消一个实参。`chunk_review_smoke` 由此通过。
16. **`meta_pipe` 的 `fork` 已补**（6.1 漏了）：`pipe_fork<M,B>` + `fork_parts` + `branch_size_of`/`branch_rec`；`pipe_state` 加 `Branch` 参数；位图分主/支两段（主 0..M-1、支 M..M+B-1）；`pipe_round` 主 fold 后把 `folded::gen_out`（主线的源元素）喂给 `pipe_branch_fold` 跑支线；`pipe_view` 加 `forked`/`main_t`/`branch_t`/`branch_nodes` + `.main()/.branch()`；builder 加 `fork` + `builder_append`（fork 后 all_to 接支线）；`meta_pipe_runner` 加 `start` 标记（main 初始就关 CONT 时一轮都不跑，接收器保持 uninitialized）。`pipe_fork_smoke` 由此通过（其 MSO main `stopped_main` 迁成新 `cont<Bitmap,Out>=false`）。
17. **`stream_cond64` 的 window-2 init-failure 规则丢了 `no_generator` 检查**：原规则是「没有生成器 && 接收器未初始化 && 不能零参初始化 → 关 looper」，迁移时退化成了「接收器未初始化 && 不能零参初始化 → 关」，导致 receive-to-init 接收器（`meta_aligned_iterator`/`meta_wire_iterator`）在有生成器时被误停。已修：USER 位 45 存 `no_generator_bit`，pre_gen 见 `meta_empty_object_t` 置位，pre_rec 按 `no_generator && !no_arg_init` 才 drop `stop_mask`。
18. **计时器 target 必须进字，不能只进类型**：D7 原「target 用类型」让引擎 `apply` 拿不到 target，压制只能延后到 condition 自己在后面窗口调 `effective`，压不住第一轮的 gen。已改成 target 下标进字（`TARGET0/TARGET1` 各 3 位，借 WINDOW 缩到 2 位空出的 6 位），`apply` 装表同窗清目标门。`ops::timer` 第三参是**位下标**（0..7）不是位值：`ops::timer(U,count,gates::rec)` 传的是位值 2 = 下标 2 = `gen`（不是 rec）；要压 rec 用 `set_suppression_timer<gates::rec>`（内部 `countr_zero`）。
19. **`pipe_final_view` 的 `from` 曾用 `State::gen`（源）而非传导的 produce**：`composition_smoke` 的 `meta_produce_t<from>::value==1` 失败。`pipe_state` 加 `prev` 成员，`pipe_round::next` 把 `folded::prev` 线程进状态，`pipe_final_view::view` 用 `State::prev`；初始 `prev` 用 `output_prev<Nodes,Gen>`（单节点=源，多节点=倒数第二节点的 rec）。`pipe_view::from_t` 保持 `get_type<from>`（looper `meta_pack_stream` 同款：`from_t=get_type`、`cache=meta_produce_t`），别改成 `meta_produce_t`。
20. **`observes` 必须是 `static constexpr bool`（惰性预计算），不能是 consteval 函数**：稀疏观测（wait_for_end 只有冲刷轮观察）下 `observes_fn()` 递归 32×64=2048 深，爆 constexpr 深度 512。改成 `static constexpr bool observes = round::observe_ ? true : rest::observes`（`if constexpr` 短路），`for_each` O(1) 读。
21. **flush 观测**：`node_step_cond` 的 `UpstreamDied` 曾每窗口 `stop`（清 REC），冲刷值收不到；改成 window-3 才 `stop`。且 `observed = st::obs && (st::cont || UpstreamDied)`（`meta_loop64::step` 暴露裸 `obs`），区分「引擎 UpstreamDied 致死（冲刷轮仍观测）」和「用户 stop 致死（off-by-one 不观测）」；`pipe_round::observe_` 去掉 `&& main_cont`（循环续跑是独立决策，不门控回调）。
22. **`for_each_forward` 跨 chunk 必须在 chunk 里做（`forward_tail`），不能在 driver 里做**：driver 层不知道 chunk 消了多少实参，`next::for_each_forward(f,a...)` 只丢一个会错位。改成 chunk 内 `forward_tail`：`spent` 时跨到 `pipe_driver<next_state>::for_each_forward`，否则 `rest::for_each_forward`；driver 的 `for_each_forward` 退化成纯委托。

## 8. 快速自检：新引擎最小例

```cpp
#include "meta_states.hpp"
using namespace meta_states; using namespace meta_objects;
template <u8 Window> struct stop_after_3 : ret_origin<Window> {};
template <> struct stop_after_3<post_rec_exec> {
  template <u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    u64 w = meta_stream64::ops::yield(U);
    if (meta_core64::lw::pos_v(L) >= 3) w = meta_stream64::ops::stop(w);
    return w;
  }();
};
using cond = make_condition<states_executor<stop_after_3>>;
// run<cond, meta_empty_o>::for_each(...) 应恰好回调 3 次
```
