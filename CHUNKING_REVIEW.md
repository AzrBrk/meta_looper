# 审核请求：分块执行（chunk driver）+ skip / yield / stop 决定

基线：`7c22cf3 Rewrite meta_pipe as a flat slot pipeline with per-node cons`。
当前改动**未提交**，工作区里就是待审的版本。

---

## 0. TL;DR

这次是两个相互独立、但共用同一套状态机的改动：

1. **分块执行**：`meta_states_looper` 和 `meta_pipe` 不再一条递归链跑到底，
   每段最多 B 轮，到预算把状态交给 driver 续下一段。目标是突破
   `-ftemplate-depth`（默认 900）。
2. **skip / yield / stop 决定**：窗口函数除了返回 flags，还可以返回三个标记；
   归一化后只是改门位，`skip`/`yield` 不碰 `G_CONTINUE`，`stop` 才结束。

公共 API 的**形状**没变（`run` / `looper` / `meta_states_for` / `meta_pipe` /
`all_to` / `run` / `for_each` / `for_each_forward` / `::type` 都还在）。
内部 `meta_states_looper_impl` 被删掉了，`meta_states_looper` 现在是类模板包装。

实测（GCC 16.2，默认 `-ftemplate-depth=900`）：
`index_sequence_istream<2000>` 的 looper/pipe 的 `::type` 与 `for_each` 都通过；
改动前 430 轮左右就撞 for_each 深度。18 个 example 全过。

### 0.1 上一轮审核后的修复

1. **P2：回调返回类型与调用值类别不一致** —— 已修。
   两套 chunk 实现里 6 处 `std::invoke_result_t<F, ...>` 全部改成
   `std::invoke_result_t<F&, ...>`，与实际的 `std::invoke(f, ...)`（左值）对齐。
   审核方的 `examples/chunk_review_smoke.cpp` 里“临时对象 + `operator() &`”
   反例已从 `#ifdef META_REVIEW_TEST_TEMPORARY_LVALUE_CALLBACK` 改为**默认正向测试**，
   `for_each` / `for_each_forward` 都通过。
2. **预算语义** —— 已改成真正的 B 轮。
   driver 的起始 `Remaining` 从 `B` 改为 `B-1`；chunk 的 `Remaining` 递减到 0，
   满额正好 B 轮。`chunk::rounds = Remaining+1`、`future_within_args` 的
   `rest::rounds` 与之一致；README / `meta_states_looper.md` / 头注释已同步。
3. **Clang 记录更正** —— 2000 轮压力用例在 Clang 18 + GCC 13 libstdc++ 下
   已通过严格**语法检查**（`CLANG_DEEP_EXIT=0`），但未做链接和运行；
   不能据此声称完整通过。trailing whitespace 是 CRLF 的误报，
   用 `core.whitespace=cr-at-eol` 检查已通过。

---

## 1. 为什么要分块（实测数据）

| 引擎 | 只算 `::type` | `::for_each`（void） |
|---|---|---|
| `meta_states::run<stream_states_co<>, meta_iterator, index_sequence<N>>` | 800 过 / 900 失败 | 430 过 / 450 失败 |
| 单节点 `meta_pipe<index_sequence<N>>::run<meta_iterator>` | 850 / 900 失败 | 430 / 450 失败 |

`for_each` 需要“最后一个真正被调用回调”的精确返回类型，所以要探测后面还有没有
回调；类型链和函数体链各走一遍，深度约翻倍。分块把两遍都切段：

```text
depth ≈ O(B + R / B)
```

looper 的 `B = META_STATES_LOOPER_BUDGET`（默认 64）；pipe 的 B 按节点数自适应
`clamp(256 / (N + 4), 1, 64)`，因为每轮还有一层 fold。

---

## 2. 改动地图

| 文件 | 责任 |
|---|---|
| `meta_states_looper.hpp` | 单步 `meta_states_looper_step`；`looper_state` / `done` / `more`；`meta_states_looper_chunk` / `_driver`；`meta_states_control` 标记与 `normalize_decision` |
| `meta_ios.hpp` | `stream_fn` 增加 `SkipMask` / `YieldMask` 模板参数，并在用户 Fn 返回后归一化决定 |
| `meta_stream.hpp` | `pipe_state` / `pipe_chunk` / `pipe_driver`；`pipe_runner_engine`；`pipe_stream_co` 的带 `PIPE_EMIT` mask；公开导出 |
| `meta_states.hpp` | 导出 `skip_t` / `yield_t` / `stop_t` 与 `skip()` / `yield()` / `stop()` |
| `examples/deep_smoke.cpp` | 2000 轮深流：looper/pipe 的 type/for_each、2 节点、wait、filter |
| `examples/decision_smoke.cpp` | pipe filter/stop、looper skip-yield/stop、函数形式等价 |
| `examples/chunk_boundary_smoke.cpp` | 把预算压到 2，专测段边界上的 `for_each_forward` 返回值 |

---

## 3. 分块设计（请重点看这一节）

### 3.1 looper 的单步抽取

原 `apply` 的四窗口逻辑整段搬进 `meta_states_looper_step`，只去掉递归，暴露：

```text
condition = final_condition_o
receiver  = next_receiver_o        // 已经处理过 _swap_
generator = next_generator_o
stage     = stage_t                 // meta_states_feed<...>
continue_ = _continue_
observe_  = _observe_
```

`meta_states_looper_chunk` 的 done 结果用 `done<step::stage>`。需要确认的等价性：

- 旧版 `_continue_ == false` 时，`track_apply_t` 是 false impl，`type` =
  `meta_states_feed<final_condition_o::flags, next_receiver_o, next_generator_o>`。
- `_swap_ = !_forward_ && _continue_`，continue 为 false 时 `_swap_` 一定 false，
  所以 `next_receiver_o == result_stage_o`；而 `stage_t` 正是
  `meta_states_feed<final_condition_flags_t, result_stage_o, generator_stage_o>`。
- 所以 done 的 feed 与旧 false terminal 的 `type` 应该逐位相同；**请用
  G_FINISH / pre_gen finish / forward / swap 的组合验证这一点**。

### 3.2 looper chunk / driver

```text
state   = (Condition, Receiver, Generator)
chunk   = 最多 Remaining 轮；continue 为 false -> done<feed>
          Remaining == 0 且还要继续 -> more<state'>
          ！则 -> chunk<state', Remaining-1>
driver  = 展开 more，直到 done；::type 取 done 的 feed
```

- 预算 B：`META_STATES_LOOPER_BUDGET`，默认 64；预算就是每段轮数。
- **轮数**：driver 起始 chunk 的 `Remaining = B-1`；`Remaining` 递减到 0，
  含第 0 轮，所以一段满额正好 **B** 轮；`chunk::rounds = Remaining + 1`，
  driver chunk 的 `rounds = B`。这里请检查有没有重复/漏掉一轮；
  `for_each_forward` 的 `future_within_args` 直接依赖 `rounds`。
- `more` 的状态用 `step::receiver` / `step::generator`（已经 swap 过），
  `condition` 用 settle 之后的 `final_condition_o`。
- driver 的 `observes_t` 是 lazy 的（嵌套类型，只在 for_each 时实例化）；
  type-only 路径不应触发任何跨段探测。

### 3.3 pipe chunk / driver

- `pipe_round` 原样保留；done 结果 = `pipe_done<pipe_round_view<Round>>`，
  其中 view 用 `Round::out` / `Round::prev` / `Round::main_out::flags` /
  `Round::index`。
- 需要确认它与旧 `meta_pipe_impl<false>` 的最终 view 等价（continue false 时
  main_out 的 position 已 +1，而 view 的 index 是 round.index = position-1；
  output rec/prev 与 `output_node<Nodes'>::rec` / `output_prev<Nodes',Gen>` 一致）。
- 预算 `pipe_chunk_budget<Nodes::size> = clamp(256/(N+4),1,64)`；
  `rounds = Remaining+1` 同理。
- `pipe_runner_engine<start,...>`：start=false 走 `pipe_initial_view`（index 0），
  不跑任何轮；start=true 走 `pipe_driver`。
- 中间节点死亡 / 输出最末节点死亡 / main mask 的语义没有改，只是状态在段边界
  由 `main_out` / `nodes` / `gen_out` 携带。请重点打 middle death + 段边界。

### 3.4 for_each（返回“最后一个观察值”）

这一块是分块里最容易出错的：

- **chunk 自己只跑本段**：段边界的 `more` tail 的 `for_each` 只处理本段最后一轮，
  绝不调用下一段。这样函数体的实例化链也在段边界断开（不只是类型层）。
- **driver 顺序调用**：
  ```text
  if has_more:
      if next.observes: 先执行本段（副作用），丢弃结果，return next::for_each
      else:             return 本段结果（若 observes）
  else:                 return 本段结果（若 observes）
  ```
- `observes` 是 F 无关的（只由 `_observe_` / `Round::has_value` 决定），
  所以 driver 的 `observes_t` 可以在不知道回调类型的情况下预算。
- void / 非 void 都走同一套：非 void 的返回值就是“最后一个观察到的回调”的
  精确类型；本段的结果在 next 观察时丢弃。
- **请用“全观察 / 全跳过 / 只在段边界附近跳过 / 最后一段无观察”组合验证**
  返回值类型、值和副作用顺序。

### 3.5 for_each_forward（按位置的参数）

- 段内：`observes_within_fn<N>`（本段从当前轮起 N 轮）。
- 段边界：`more` tail 用 `driver::observes_within_fn<sizeof...(A)>`。
- 跨段的递归 tail：`future_within_args<N>`：
  ```text
  rest::observes_within_fn<N>
  || (rest::has_more && N > rest::rounds
      && driver<rest::next_state>::observes_within_fn<N - rest::rounds>)
  ```
  这是为了修“段内最后几次跳过、但 continuation 还有观察”时把当前返回值提前
  返回的问题。
- `examples/chunk_boundary_smoke.cpp` 把预算压到 2：5 轮里第 2、3 轮 skip，
  `for_each_forward(f,0,1,2,3,4)` 必须返回 4（而不是段内的 1）。这个测试已过。
- **请重点攻击**：`N` 与 `rest::rounds` 的 off-by-one、最后一段 done 但 N 很大、
  `rest::has_more == false`、`N == 0`、参数数量少于轮数。

### 3.6 深度与实例化

- type-only：chunk 的 `type` 链 B 轮 + driver 的 `R/B` 段。
- for_each：chunk 的 body B + driver 的 `R/B`（driver 顺序调用，不嵌套在 chunk 里）。
- driver 的 `observes_t` / `observes_within_fn` 只在 for_each 路径实例化；
  type-only 不应看到它们。
- 仍然存在的上限：driver 本身是 `R/B` 层；runtime 的 for_each 仍是一轮一层调用；
  真正无限长需要 `-ftemplate-depth` 或非标准状态化元编程。

---

## 4. skip / yield / stop 设计

### 4.1 标记与归一化

```text
skip  : 清 G_RECEIVE | G_NOT_SKIP（pipe 再加 PIPE_EMIT），保留 G_CONTINUE
yield : 置 G_RECEIVE | G_NOT_SKIP（pipe 再加 PIPE_EMIT），保留 G_CONTINUE
stop  : 清 G_CONTINUE
```

`meta_states_looper_details::normalize_decision<Raw, Flags, SkipMask, YieldMask>`：
非标记原样返回，标记以 `Flags` 为底做 `opDeact` / `opSet`。

### 4.2 挂点

只在 `meta_ios_details::stream_fn::apply::value` 里归一化：

- 先过 stream control 得到 `controlled_flags`；只有 `G_CONTINUE` 还在时才调用
  用户 Fn；
- 用户 Fn 返回 `raw`；`normalize_decision<raw, controlled_flags, SkipMask, YieldMask>`；
- 返回 `normalized::value`。

`stream_states_co` 用默认 mask（`G_RECEIVE|G_NOT_SKIP`）；`pipe_stream_co` 显式
传带 `PIPE_EMIT` 的 mask。这样 looper 的 skip/yield 不会误碰 pipe 的 emit，
pipe 的也不会漏掉 emit。

公开导出：`meta_states::skip_t/yield_t/stop_t`、`meta_states::skip()/yield()/stop()`，
`meta_ios` 同样。

### 4.3 语义边界

- `skip`/`yield` 只改门位，**不碰 `G_CONTINUE`**；所以 skip 不结束递归。
- `stop` 才清 `G_CONTINUE`。
- 决定是 flags 级的：`stream_states_co` 自己的压制计时器仍然生效
  （`effective` 可能看不见某一位），决定不会强制压过计时器。
- 目前只支持 `stream_states_co` / `node_con` 这条 `stream_fn` 路径；
  自定义 MSO condition 直接返回标记还不支持（MSO 协议要的是 flags）。
- TMP 的 `apply` 是类型别名：`using apply = skip_t;`（或 `decltype(skip())`）。
  函数式 Fn（`static constexpr auto apply() { return skip(); }`）还没适配。
- `yield(value)`（带 payload）**未实现**；见“待定”。

### 4.4 观察到的 stop 行为

- looper 在 `post_rec_exec` stop：这一轮 `_continue_` 为 false，所以本轮**不回调**，
  但 result 已经接收，最终 `to_t` 是这一轮的值。
- pipe 在 `pre_gen_exec` stop：本轮不发射，最终是上一轮的值。
- `decision_smoke` 明确断言了这两种行为；如果认为“stop 那轮应该先回调再停”，
  需要改语义并改测试。

---

## 5. 请重点攻击的点（按优先级）

### P0

1. **单步抽取等价性**：continue=false 的 feed 与旧 false terminal 是否逐位一致；
   `_swap_`、`G_FINISH`、pre_gen finish、post_rec finish、forward 方向。
2. **段边界轮数**：B 轮、`Remaining == 0` 的 more、done 的 feed；
   有没有重复跑一轮/漏一轮；`rounds` 与 `future_within_args` 是否一致。
3. **for_each 语义**：最后一个观察回调的类型/值；void 与非 void；全跳过；
   段边界附近跳过；最后一段无观察；副作用顺序；不能多调/少调。
4. **决定归一化**：skip/yield 是否真的不动 `G_CONTINUE`；stop 是否清；
   pipe 的 emit mask；四个窗口都试；非标记返回值是否原样通过。
5. **start=false**：初始 main 关总闸时 type/index 与旧行为一致，for_each 为空。

### P1

6. **for_each_forward 跨段**：`future_within_args` 的 off-by-one；`N` 与
   `rest::rounds`；done 的段；参数不够；pipe 的同一逻辑（预算自适应，建议
   临时把 `pipe_chunk_budget` 压小测一次）。
7. **driver observes 的 lazy 性**：type-only 是否真的没有实例化跨段探测；
   如果发现 type 路径也触发了 `observes_t`，深度会回退。
8. **pipe 的 middle death / tail death / wait** 跨段：main mask、position、
   未初始化 receiver 的 idle/flush。
9. **API 兼容**：`meta_states_looper_impl` 被删；`meta_states_looper` 从别名
   变成类模板；`stream_fn` 多了两个模板参数。仓库内的引用已清，外部用户可能受影响。
10. **`meta_states_looper_t` / `meta_states_for`** 的约束与默认参数是否还一致。

### P2

11. 命名：`meta_states::yield` / `skip` / `stop` 是否会与用户代码冲突；
    `meta_states_control` 顶层命名空间是否合适。
12. `wait_for_end_con` + 返回决定的 Fn：mask 含 `PIPE_EMIT`，用户在 wait 里
    yield 会真的发射；这是不是期望行为。
13. 文档：README / `meta_states_looper.md` 的 chunk/decision 段落是否与实现一致。
14. 可移植性：`std::clamp` constexpr、`u128` 作为 NTTP、`consteval`；
    Clang 18 + GCC 13 libstdc++ 请跑一遍（本机 Clang 18 配 GCC 16 libstdc++ 会因
    `__builtin_popcountg` 失败，没法验证）。
15. 测试成本：`deep_smoke` 编译约 316s；是否要降到 1000 轮，或单独作为一个
    manual/stress target。

---

## 6. 测试与复现

所有 example：

```powershell
g++ -std=c++23 -Wall -Wextra -pedantic-errors -Werror -I. -o out.exe examples\<name>.cpp
```

当前 18 个 example 全部通过（含审核方的 `chunk_review_smoke`、新增的
`pop_back_smoke` / `exp_select_from_smoke`）：

```text
args_at_smoke, chunk_boundary_smoke, chunk_review_smoke, condition_smoke,
decision_smoke, deep_smoke, exp_select_from_smoke, flags128_smoke,
looper128_smoke, object_invoke_smoke, pipe_boundary_smoke, pipe_edge_smoke,
pipe_smoke, pop_back_smoke, stream_control_smoke, stream_end_smoke,
type_safe_aligned_smoke, uint128_smoke
```

关键断言：

- `deep_smoke`：`index_sequence_istream<2000>` 的 looper/pipe `::type`、
  `for_each` 计数、最后一个回调返回值、2 节点对齐、wait（1 次回调）、
  filter（1000 次回调，跨段 skip）。
- `decision_smoke`：pipe filter（5 次回调，最终 8）、pipe stop（4 次，最终 3）、
  looper skip/yield（5 次，最终 8）、looper stop（4 次，最终 4）、函数/类型等价。
- `chunk_boundary_smoke`：预算 2，段边界 skip + continuation，forward 返回 4。
- `pop_back_smoke`：`take<N, TL>` 的 0 / 截断 / tuple / void / 函数 / 引用用法；
  2000 项 `exp_list::pop_back`、`back`、类型等价，GCC 16 完整编译约 4.5s。
- `exp_select_from_smoke`：`From/Index` 越界、巨大下标、非 type list、`void`、
  `std::tuple` 与 2000 项列表，失败一律 `no_exist_type`。

`deep_smoke` 编译约 290-316s（2000 轮）；其余都是秒级。

---

## 7. 待定 / 已知缺口

1. **`yield(value)` 的语义**：还没做。两种方向——
   - 只给观察者：stage 包成 `yield_view<V, View>`，流状态不变；
   - 注入流：节点 rec 变成 V，下游真的收到 V（V 必须满足 meta object / produce 约束，
     looper 的 Receiver 位尤其严格）。
   需要你定，定了再补。
2. **函数式 Fn**：现在 TMP 的 `apply` 是类型；`return states.skip()` 这种写法
   需要一个“函数式 Fn → 类型”的适配层。要不要加？
3. **`for_each_forward` 跨段**：looper 已有预算 2 的测试；pipe 的 budget 是自适应的，
   没有直接压小测过。若你认为需要，我可以加一个 `META_PIPE_CHUNK_BUDGET` 覆盖。
4. **stop 那轮的回调**：looper 在 post_rec stop 时本轮不回调（`_continue_` 已 false），
   但 result 已接收。是否要改成“先回调再停”？
5. **决定 vs 计时器**：`skip`/`yield` 是 flags 级，压制的位仍然可能被计时器隐藏；
   如果你希望决定有更高优先级，需要另做。
6. **`meta_states_looper_impl` 删除** 的兼容性；仓库内无引用。
7. **`stream_states_co` 直接当 pipe 节点 con** 时，skip/yield 不会带 `PIPE_EMIT`；
   这是 by design（应该用 `node_con`），但值得在文档里点明。
8. **`exp_list::pop_back` 已优化**：复用新公开的 `take<N, TL>`，用
   `std::tuple_element_t` 一次展开取前 N-1 个元素；2000 项完整编译约 7s
   （旧实现 ~900 层就撞深度）。深流测试用 `meta_iterator` 只是习惯，不再是限制。

---

## 8. 环境

- GCC 16.2.0 (MinGW-W64 UCRT)，`-std=c++23 -Wall -Wextra -pedantic-errors -Werror`。
- 仓库文件 CRLF；新增/修改文件保持 CRLF。
- 本机 Clang 18 配 GCC 16 libstdc++ 无法验证；审核环境是 Clang 18 + GCC 13。
- 基线 `7c22cf3`；当前改动未提交。

如果只能挑三件事看，我建议：**单步抽取等价性（P0-1）**、
**for_each 的跨段返回值（P0-3）**、**段边界轮数 / for_each_forward 的
`rounds`（P0-2 + P1-6）**。这三处错了会静默给出错误结果，而不是编译失败。
