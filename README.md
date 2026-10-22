# meta_looper

C++23 纯头文件的**编译期循环 / 状态机**框架：一轮循环由"元生成 + 元接收"驱动，用四个监控窗口
（`pre_gen_exec` / `post_gen_exec` / `pre_rec_exec` / `post_rec_exec`）把用户的条件函数嵌进去，
阶段推进、index 记账、计时器递减与压制全部由 looper 自己的 flags 完成。

## 文件

| 文件 | 作用 |
|---|---|
| `meta_states_looper.hpp` | 引擎：门位（gates）、计时器、四个监控窗口、阶段推进与轮尾记账 |
| `meta_states.hpp` | 公开层：控制位 / 窗口号 / 字段布局 / 计时器 API、`state_fn`、`make_condition`、`run` |
| `meta_ios.hpp` | 最终 API 门面：单向聚合状态、组件、流控制、管道和组合算法 |
| `meta_stream_control.hpp` | 独立流控制层：初始化/结束检测及 `stream_states_co<Fn>`，不依赖管道 |
| `meta_stream.hpp` | 管道：`meta_pipe` / `all_to` / `run`（实现；由 `meta_ios.hpp` 统一公开） |
| `meta_stream_algorithms.hpp` | 管道组合：`transform` / `filter` / `take` / `fold` / `collect` |
| `meta_iostream.hpp` | 基于元对象协议的输入流、输出流及对齐迭代器 |
| `type_safe_aligned_iterator.hpp` | 类型安全的对齐字节缓冲迭代器（`meta_pipe` 的实际使用者） |
| `meta_objects.hpp` | 元对象协议：`meta_object` / `meta_states_object` / `meta_ret_object` / `meta_empty_o` / `flags_operator` |
| `meta_uint128.hpp` | 通用 128 位结构化整数：位运算、移位、模加减；不包含 looper 布局 |
| `meta_invoke_protocols.hpp` | `meta_invoke` / `invoke_if` 等调用协议 |
| `meta_utilities.hpp` | `exp_list`、`exp_select` / `exp_select_from`、`take`、`exp_size`、`length_*`、`value_is` 等基础件 |

`meta_stream` 的部分在本仓库里只保留管道（`meta_stream.hpp`）：它不再把
`(To, From)` 塞进一个 meta object。管道在扁平的节点槽和模板参数之间传递状态，
每轮对节点做一次 fold，不经过 MTO/MSO 的 payload。

层间依赖保持单向：元对象/调用协议 → 执行引擎 → `meta_states` →
流控制 → 管道 → 组合算法 → `meta_ios` 门面。`meta_iostream` 是旁路组件层，
只依赖元对象协议，可独立用于 looper 或 pipe。实现头文件不反向包含
`meta_ios.hpp`；原先直接包含 `meta_stream.hpp` 的入口仍可用。
looper 与 pipe 共享窗口、计时器、字段恢复等值级机制，不合并为万能调度器；
pipe 独有的空转、上游死亡及 emit/flush 保留在管道层。

## 用法

```cpp
#include "meta_states.hpp"

using namespace meta_states;
using namespace meta_objects;

// 窗口函数：按窗口号特化，没写的窗口继承 ret_origin 直通
template <u8 stage>
struct my_exec : ret_origin<stage> {};

template <>                      // 只写窗口①
struct my_exec<pre_gen_exec> {
  template <this_ref flags, class gen>
  struct apply { static constexpr auto value = flags::value; };
};

using cond = make_condition<states_executor<my_exec>>;
// run<cond, Receiver[, Generator]>::for_each([](auto stream){ ... });
```

- 门位：`G_CONTINUE`（总闸）、`G_NOT_SKIP`（回调）、`G_ADVANCE`、`G_RECEIVE`、`G_FINISH`（本轮到此为止）、
  `G_FORWARD`、`G_PACK`、`G_FEED_GEN`
- 计时器：`set_suppression_timer` / `tick_timers` / `effective` —— 压制只影响 looper 的**判定**，不改用户的 flags，
  计时归零后自动恢复
- 字段布局：`position`(u16, 轮尾 +1) / `user`(u8) / `timer0`(u16) / `timer1`(u16) / `gates`(u8)

### 调度器字段保护

用户条件函数的结果不会直接覆盖调度器字段。引擎在调用前提供当前窗口号，
调用后通过掩码合并：`(用户结果 & ~保护掩码) | (调度器快照 & 保护掩码)`；
随后才由引擎推进窗口、记账及递减计时器。关闭的窗口仍然完全惰性，不调用用户函数。

| 调度层 | 保护的段 |
|---|---|
| looper / pipe 节点的条件 flags | bits 0..15 的 `position`；bits 20..23 的 `stage_index` |
| `stream_states_co` / `node_con` / `wait_for_end_con` 的用户 Fn | 上述字段，以及 bits 16..19 的基础流模式（无生成器标记） |
| pipe 的 `main_con` | bits 0..15 的 `position`；bits 64..127 的节点存活位图 |

初始化时的 position 保留，但每个实际执行的窗口会在调用前校正 stage 镜像，
不再允许用种子或用户返回值跳转窗口。用户结果对保护段的写入被忽略，不产生编译错误。
普通 looper 的 bits 16..19、高 64 位仍可供用户使用；stream 条件的低 4 位属于基础控制。
门位（含 `G_FINISH`）、两槽计时器、skip/yield/stop 和 pipe 的 emit/flush 仍是公开控制，
不被回滚。尤其计时器仍可由用户装表、覆盖或清除。

driver 使用独立的 MSO flags，普通条件函数不能写入它。显式传入的自定义
`DriverPolicy` 是受信任的调度扩展，仍可按原协议修改 driver 的 phase、预算、计数器、
计时器和状态位；本机制不回滚 policy 的合法调度结果，也不限制作为数据的 MSO receiver。

### 执行契约

- 通用 looper 不认识流结束；`stream_states_co` / 节点流条件负责初始化和
  `end_of_list` 检测。已有产物的源首轮保留当前值；未初始化源通过调用初始化。
  无源场景由流控制分别处理有无初始化能力的 receiver。
- 一轮顺序：`pre_gen` →（推进）→ `post_gen` → `pre_rec` →（接收）→ `post_rec`。
  `G_CONTINUE` 关闭不等于跳过其余条件窗口；`G_FINISH` 才截短窗口链。
  在 `pre_gen` / `post_gen` 设置 FINISH 会阻止本轮接收；在 `pre_rec` 设置
  FINISH 不撤销接收，但跳过 `post_rec`。FINISH 在轮尾消费。
- looper 回调要求窗口①允许进入、窗口③采样的有效 `G_CONTINUE | G_NOT_SKIP`
  和窗口④后的有效 `G_CONTINUE`。不额外检查 FINISH；`post_rec` 关闭总闸
  可以保留已经接收的结果而不回调。pipe 回调不同：只观察最终节点实际发射，
  因此节点死亡当轮仍能发射最后一个接收值或 flush 结果。
- 窗口正常共用同一 position；轮尾只加一次并在 u16 内回绕。
  计时器只压制有效判断、不永久清门位；结算时 tick 一次，计数归零清整槽。
  字段内算术不能进位到邻段；关闭窗口/调用不能实例化无效用户元函数。
- MSO 互调投影 flags；普通元对象依协议投影 payload/ret。例外是 pipe main：
  使用 `meta_invoke` 接收完整输出对象（包括 MSO），不能改成解包 flags。
- 自定义 control / DriverPolicy 是显式扩展边界；保留其调用协议，不把数据 MSO
  的布局误当成条件或 driver 布局。字段保护范围见上表。

上述语义以 `execution_contract_smoke` 的顺序参考模型、
`driver_flags_smoke` 的算术/所有权对照及既有空源、flush、调用协议测试约束。

### 值级控制快路径

内部控制优先在 `u128`（必要时直接使用其 `low` / `high`）上计算：
计时器压制和递减、position 回绕、窗口推进、driver 预算与计数器更新、
stream 决定归一化、pipe main 记账均不再通过连续的 `flags_operator` 类型变换。
窗口把用户调用后的核心字段恢复和轮尾结算合并计算，最后才经
`meta_set<flags_operator<value>>` 写回 MSO，避免中间控制 MSO 和 flags 类型实例化。

默认 `user_flags_control` 直接走值函数；显式传入自定义 control 时仍走
`meta_object_invoke`，自定义 `DriverPolicy` 的接口不变。
公开的 flags 别名、元对象系统和实际承载数据的类型保持不变。
类型只有在用户元函数参数、数据产物和 MSO 协议边界才必须物化；
`invoke_if` / 关闭窗口仍保持惰性，不能用 `std::conditional_t` 提前实例化未选分支。
`G_FINISH` 仍不被保护或新增为回调抑制条件；计数器的进位不会泄漏到相邻字段。

本次对照使用优化前 `b16e3cd` 的相同测试源，仅替换头文件，Clang 18，
`-std=c++23 -O0 -fsyntax-only`；前后交替各测三次，取中位数（沙箱实测，不保证其它环境）：

| smoke | 编译时间：优化前 → 后 | 峰值 RSS：优化前 → 后 |
|---|---|---|
| `driver_flags_smoke` | 11.97s → 6.94s | 615MiB → 421MiB |
| `chunk_review_smoke` | 5.28s → 4.23s | 337MiB → 294MiB |
| `args_at_smoke` | 9.20s → 6.31s | 518MiB → 401MiB |

同一 `driver_flags_smoke` 源以 `-c -ftime-trace -ftime-trace-granularity=0`
统计：类模板实例化事件从 41041 降为 30703，其中 `flags_operator` 从 19998 降为 14648。
新增的值级等价性测试不计入上表，完整回归另行运行；未运行 2000 轮 deep stress。

## Stream Conditions

```cpp
#include "meta_ios.hpp"

struct select_second {
  template<class Window, class Flags, class Object>
  using apply = typename Flags::template select<
      meta_states::user_fn::index_of<Flags>() == 1,
      meta_states::G_NOT_SKIP, 0>;
};

using condition = meta_ios::stream_states_co<select_second>;
using loop = meta_states::run<condition, meta_iostream::meta_iterator,
                             meta_iostream::meta_istream_list<char, double, int>>;
```

`Fn::apply<Window, Flags, Object>` 只描述用户算法，结果提供完整 flags 的 `::value`。
每个窗口先运行基础流控制，再调用 `Fn`；基础控制已经判定终止时不调用 `Fn`。
初始化、首轮保留现有产出和流结束判断不需要写入用户算法。
`meta_ios::stream_states_co<>` 使用默认直通算法。

实现位于 `meta_stream_control.hpp` 的内部命名空间 `meta_ios_details`，
不修改底层元对象或 looper。
当前基础控制占用 `user` 字段低 4 位记录无生成器状态，并使用计时器槽 0
处理首轮推进/接收抑制；用户算法应保留这些字段，高 64 位仍可用于用户状态。

当 Generator 或 Receiver 本身就是 `meta_states_object` 时，元对象调用协议交给
条件的对象是它的 `flags`，基础流控制会直接放行（把 MSO 当作 gen/rec 位使用，
例如 `type_mask`）；此时是否停止由用户的 `Fn` 自己决定。

## 上层组合

包含 `meta_ios.hpp`；全部适配器位于 `meta_ios`，不要求用户拼保留位：

| 适配器 | 契约 |
|---|---|
| `transform<Fn>` | 每次接收输出 `meta_invoke<Fn, Input>`，Fn 是一元元函数 |
| `filter<Pred, Receiver=meta_iterator>` + `filter_con<Advance=false>` | 只对实际收到的输入调用一元 Pred；不通过时不更新内部 Receiver，也不发射，源仍推进 |
| `take<N, Receiver=meta_iterator>` + `take_con<Advance=false>` | 最多实际接收 N 次；计数属于 receiver 的独立 u128，不使用轮数或调度器 position |
| `fold<Initial, Fn>` | 以 `meta_invoke<Fn, State, Input>` 累积；每次接收仍输出当前累计结果 |
| `collect<List=exp_list<>>` | 保留到达顺序及 List 模板，将每个产物作为一个元素收集（不自动展开列表） |

显式条件放在源的第一个节点时须设 `Advance=true`，中间节点保持默认 false。
filter / take 的 Receiver 须为已初始化、按 payload 接收的非 MSO 元对象；
MSO 的“接收完整对象”协议不能由这些 payload 包装器模拟，约束会拒绝它，
此类需求应直接使用原生 MSO 节点/自定义条件。产物按 Receiver 协议投影给下游，
内部 `::type` 保存包装状态。filter 在接收边界惰性调用 Pred，通过才更新 Receiver，
并在 post_rec 按接收结果决定 emit；上游空转时不调用 Pred，也不消耗下游限额。
take 的内部 `::type` 保留限额和 Receiver 状态。take 到达限额在 post_rec 死亡，
同轮最后值仍会发出，无须再推进源；N=0 在首次 pre_rec 拒收，但此前的
上游推进/变换仍可能发生。过滤空转不消耗 take 限额；源先结束则保留已经接收的部分。
这些是 pipe 适配器，尤其 take 的最后值回调不要套用 looper 的总闸观察规则。

```cpp
struct pointer {
  template<class T> using apply = T*;
};
using result = meta_ios::meta_pipe<meta_iostream::meta_istream_list<int, char, double>>
    ::all_to<meta_ios::transform<pointer>>
    ::all_to<meta_ios::take<2>, meta_ios::con<meta_ios::take_con<>>>
    ::run<meta_ios::collect<>>
    ::type::to_t;  // exp_list<int*, char*>
```

只需要最终累积值时，将 fold 配合 `wait_for_end_con`（源节点使用
`wait_for_end_advance_con`）延迟到死亡时 flush。自定义 `node_con`、
元对象 receiver 和 DriverPolicy 仍是底层 escape hatch。

### 语义与编译成本双基线

`examples/composition_smoke.cpp` 用独立 constexpr 顺序模型验证过滤、变换、
限额、收集及每次回调；覆盖 0/1/7/65 个输入、不同过滤密度、已初始化与延迟
初始化源、异类型、无限源截断、fold 和最终 flush。既有 `args_at`、
`chunk_review`、`type_safe_aligned` smoke 继续覆盖转发/分块/真实布局负载。

`examples/compile_matrix.cpp` 是仅用既有 API 的可配置编译负载，适合相同源对照：

```sh
/usr/bin/time -f '%e seconds; %M KiB RSS' clang++ -std=c++23 -O0 -fsyntax-only \
  -I/path/to/header-snapshot -DMETA_BENCH_ROUNDS=65 -DMETA_BENCH_NODES=4 \
  -DMETA_BENCH_STRIDE=2 -DMETA_BENCH_INITIALIZED=1 examples/compile_matrix.cpp
```

建议矩阵 `(rounds,nodes,stride,initialized)`：
`(7,1,1,1)`、`(65,4,2,1)`、`(65,8,3,0)`、`(65,4,100,0)`；
节点数含最终节点，范围 1..64。initialized=0 使用异类型 integral_constant
列表的延迟初始化源，1 使用已有产物的索引源。负载同时实例化最终结果与回调链，
静态断言回调数；不把更改测试内容后的耗时当成优化收益。

分别使用 Clang/GCC 的相同编译选项、模板深度限制，顺序交替测两个头文件快照
各三次，取中位数，测量期间不并行跑其它编译；记录版本、峰值 RSS 和失败诊断。
Clang 可另用 `-c -ftime-trace -ftime-trace-granularity=0` 观察实例化事件；
GCC 可使用 `-ftime-report`。压力测试 `deep_smoke` 单独运行，不能将超深度
或内存失败混入普通 smoke。不新增依赖或测试框架。

## Pipe（meta_pipe）

`meta_pipe` 自己就是 looper：状态是一组扁平 slot，每轮做一次 fold；中间段不再
塞进 MTO/MSO，每段是一个 `(rec, con)` 节点，`con` 只控制自己这段的流程。

```cpp
#include "meta_ios.hpp"

using offsets = meta_ios::meta_pipe<meta_iostream::meta_istream_list<int, double, char>>
                    ::all_to<meta_iostream::meta_aligned_iterator>
                    ::run<meta_iostream::meta_ostream<>>
                    ::type::to_t;   // exp_list of aligned seek_to objects
```

内部形态（输出 → 输入，build 时按 gen → 输出写、内部反过来 prepend）：

```
meta_pipe_impl<main, recN, conN, ..., rec0, con0, gen>
```

- `all_to<Rec>`：追加一段，默认 con = `stream_states_co` 的 pipe 版本（节点 0
  的 `G_ADVANCE` 开，其余关）。
- `all_to<Rec, con<C>>`：显式指定这段的 con（con 永远放在 rec 后面）。
- `run<Rec>` / `run<Rec, con<C>, main_con<M>>`：最后一段 + 全局 main；
  `::type` 是 `pipe_view`，有 `to` / `to_t` / `from` / `from_t` / `states` / `index()`。
- `for_each` / `for_each_forward`：只在本轮最终节点真的发值的时候回调；
  也支持 `for_each<protocols::stream_to_t>(f)` 先投影成 `to_t`。
- `meta_pipe<>`：没有上游（gen = `meta_empty_o`）。

每段 con 的 flags 高 64 位有两个 pipe 保留位：

- `PIPE_EMIT`（bit 64）：本轮收到值后是否往下游发；filter 把它清掉。
- `PIPE_FLUSH`（bit 65）：节点死掉的那一轮是否把当前 `rec` 冲刷给下游；
  `wait_for_end_con<>` 就是"活着只收不发、死时发最终累积值"。

结束规则：非输出最末节点死亡 → 本轮照常把最终结果传下去，本轮结束后 pipe 停；
输出最末节点死亡 → 清存活位、以后不再调用它，`main_con` 按位图（它的高 64 位）
决定是否继续，全死即停。上游还活着但还没产生任何值时（例如 filter 跳过了前几个
元素），下游这一段只空转、不提前死亡，等上游真正产生值再接收。
这里的空转仅针对上游 receiver 尚未初始化：不调用节点的用户 Fn，只递增
position 并 tick 计时器。上游 receiver 已初始化后，即使本轮没有发值，仍运行
四窗口，但不接收旧值。

未初始化的 receiver 不会 flush；已初始化的 receiver 即使从未接收过值，
结束时仍会 flush 当前累积值，包括空累积值。这不要求 payload 是类型列表。
`main_con` 的存活位图和 position 由引擎维护，用户 Fn 返回后恢复这两段，其余低位保留。
初始 main 关闭 `G_CONTINUE` 时不执行任何轮次。

looper 的单步由 `meta_states_looper_step` 提供；pipe 保留独立的节点单步实现，
采用相同的四窗口语义，并增加 emit / flush 规则。中间节点
默认 `G_ADVANCE` 关，因为链上只有 receiver，没有独立的 generator。回调里用
`.index()` 取轮次；`.left()` 是 payload 长度，对 pipe 节点没有意义。

旧写法 `all_to<os, protocols::wait_for_end_to_t>` 现在写成
`all_to<os, con<wait_for_end_con<>>>`：一样是“活着只收不发、结束时只发最终值”。
如果这段就是节点 0（要推进源），用 `con<wait_for_end_advance_con<>>`。

`type_safe_aligned_iterator` 的 type_mask 是 64 位，类型数上限 64
（`aligned_iterator_storage` 里有 `static_assert`；超过时直接拒绝，不会给出错误的
`safe()`）。槽代理的 copy assignment 已删除：`*it1 = *it2` 会编译失败，要写值请用
`*it = value`。这也使正向与反向迭代器的隐式 copy assignment 被删除；
它们仍支持 copy construction。

### 分块续跑（突破模板深度）

`meta_states_looper` 和 `meta_pipe` 的轮次现在按 chunk 跑。looper 每段最多
`META_STATES_LOOPER_BUDGET` 轮（默认 64），pipe 使用独立的自适应预算。到预算就把
`(Condition, Receiver, Generator)` / `(Main, Nodes, Gen)` 交给下一段 driver。
预算就是“每段轮数”：driver flags 的起始 `remaining` 是 `B-1`，reduce 到 0 的
那一轮结束后进入 `more`，下一段重置并继续，正好每段 B 轮。`for_each` 的段边界也
不在 chunk 里调用下一段，而是由 driver 顺序调用，这样函数体实例化链也被打断。

- 深度从 `O(轮数)` 降到 `O(预算 + 轮数/预算)`。本轮 flags driver 重构后按
  用户要求没有跑 2000 轮 deep stress；100 轮 looper/pipe（跨默认预算边界）已通过。
  `examples/deep_smoke.cpp` 保留作需要时的 stress 入口。
- pipe 的预算按节点数自适应：`clamp(256 / (节点数 + 4), 1, 64)`，因为每轮还有
  一层 fold。
- looper 的预算可以用 `-DMETA_STATES_LOOPER_BUDGET=...` 覆盖，不影响 pipe；编译器自身的深度上限无法
  在语言内查询，所以这是固定预算，不是“到上限自动切”。
- driver 本身仍是 `轮数/预算` 层递归，runtime 的 `for_each` 也仍是一轮一层；
  真正无限长还需要 `-ftemplate-depth` 或非标准的状态化元编程。
- `observes` / `observes_within_fn` 的“未来有没有回调”是惰性的：当前状态已有回调时
  不再实例化后面的 `Rest::observes`，探测停在第一个回调状态。`for_each` 的执行和
  “最后一个回调”的返回值不变——停的是探测，不是执行。
- `exp_list::pop_back` 改成复用 `take`，不再依赖 `std::tuple`：支持该内建的
  GCC/Clang（`__has_builtin(__type_pack_element)`）走一次展开（O(1)/元素），
  其他编译器（例如 GCC 13）回退到纯类型列表二分（深度 O(log N)）。
  GCC 16 上 2000 项约 4.5s；没有内建时 2000 项约 75s，100 项以内仍是瞬时的。
  `void` / 函数 / 引用元素也安全；可用
  `-DMETA_UTILITIES_HAS_TYPE_PACK_ELEMENT=0` 强制走回退实现。
  `take<N, TL>` 也作为 `exp_utilities` 的公开工具。
- `exp_select_from<From, Index, TL>` 先 drop 前 `From` 个元素，再在剩余部分选
  `Index`；固定 `From` 时前缀 drop 可复用。越界、下标过大、非 type list 都返回
  `no_exist_type`，不产生硬编译错误。

### flags 驱动的 driver（driver MSO）

控制面不再靠 `chunk_tail<Continue, AtBudget, ...>` 一组偏特化，而是一个 driver MSO：

```cpp
meta_states_object<driver_seed_flags<Budget>::value, driver_fn<Policy>>
```

driver 的 128 位就是它的寄存器。低 64 位是数据/计数器，高 64 位是控制/状态：

| bits | 字段 | 宽度 |
|---|---|---|
| 0..15 | `pc` | u16 |
| 16..23 | `remaining` | u8 |
| 24..39 | `chunk_index` | u16 |
| 40..47 | `chunk_round` | u8 |
| 48..55 | `timer0` | u8 |
| 56..63 | `timer1` | u8 |
| 64..71 | `phase`（run/more/done） | u8 |
| 72 | `observes` | bit |
| 73 | `has_more` | bit |
| 74 | `boundary` | bit |
| 75 | `stopped` | bit |
| 76 | `paused` | bit |
| 77 | `yielded` | bit |
| 78 | `skip` | bit |
| 79 | `error` | bit |
| 80..95 | `policy_state` | u16 |
| 96..127 | `user` | u32 |

- `driver_fn<Policy>` 有 `round` / `more` / `done` / `decide` 四个窗口；默认
  `default_driver_policy<Budget>` 实现预算、phase 和 timer tick。
- `chunk` / `driver` 各只有一个类：读 `phase`/`remaining`，用 `if constexpr`
  走 run/more/done；段尾的 driver/state/feed/has_more 由 `flags_end` 一路传上来。
- 回调里读 driver flags（`G_PACK` 的 looper 视图和 pipe 视图都暴露
  `Stage::driver`，表示本轮开始前的 driver 状态）：

```cpp
loop::for_each([](auto stage) {
  using D = typename decltype(stage)::driver;
  constexpr auto remaining =
      D::flags::template get<u8, meta_states_looper_details::driver_control::remaining_shift>;
  constexpr auto phase =
      D::flags::template get<u8, meta_states_looper_details::driver_control::phase_shift>;
});
```

- timer0/timer1 每轮递减到 0；`driver_decide` 把 `skip_t` / `yield_t` /
  `stop_t` 写进 `skip` / `yielded` / `stopped` 位，loop 的 `stop` 还会把
  phase 置为 done。
- 自定义 policy 需提供 `seed_remaining` 和 `round` / `more` / `done` / `decide` 窗口（继承 `default_driver_policy<Budget>` 即可）。looper 用 `meta_states::looper_with_policy` /
  `meta_states::run_with_policy<..., Policy>`；pipe 的 `run<Rec, Con, Main, Policy>`
  也接受 policy（默认 `default_pipe_driver_policy`）。


`stream_states_co` / `node_con` 的用户 Fn 除了返回 flags，还可以返回三种决定：

- `skip_t`（或 `skip()`）：本轮不收/不发，**循环继续**（不碰 `G_CONTINUE`）；
- `yield_t`（或 `yield()`）：本轮收/发，循环继续；
- `stop_t`（或 `stop()`）：结束循环。

TMP 里 `apply` 是类型别名，所以写 `using apply = yield_t;` / `skip_t` / `stop_t`；
`skip()` / `yield()` / `stop()` 是同一组标记的 constexpr 函数形式，方便在
`decltype(...)` 或 constexpr 辅助代码里用。pipe 的 skip/yield 会同时处理
`PIPE_EMIT`，不会误伤 `G_CONTINUE`。完整例子见
`examples/decision_smoke.cpp`（pipe filter、pipe stop、looper skip/yield）。

`for_each` / `for_each_forward` 也会用 `invoke_result_t` 识别回调返回类型：

- `stop_t`：立即停止，不再调用后续回调（跨 chunk 也不会进入下一段），返回 `stop_t`；
- `skip_t` / `yield_t`：不作为观察值，继续遍历；如果它是最后一个回调，返回该标记，
  否则返回后面观察到的值。

完整例子见 `examples/for_each_decision_smoke.cpp`（含跨 chunk 的 stop）。

## 128-Bit Flags

`flags_operator<Value>` 和 `meta_states_object<Value, Fn>` 的 `Value` 使用
`meta_objects::u128`。这是由 `low` / `high` 两个公开 `uint64_t` 成员组成的
C++20 structural type，不依赖 `__int128`，也不包含控制字段定义。
`u128{low, high}` 构造完整值，单个整数参数仍然表示低 64 位。

looper 的原有布局保留在 bits 0..63，bits 64..127 留给用户状态：

```cpp
using seeded = flags_operator<condition_seed>::set<u64, 64, 42>;
using next = seeded::inc<u64, 64>;
static_assert(next::get<u64, 64> == 43);
static_assert(next::get<u64, 0> == condition_seed);
```

字段允许跨越 64 位边界，完整字段可使用 `set<u128, 0, u128{low, high}>`。
位操作的掩码也支持完整 128 位，如 `opSet<(u128{1} << 127)>`。
`inc` / `dec` 截断在字段内部，`add_at` / `sub_at` 允许跨字段进借位；
完整加减按模 2^128 计算。移位量达到 128 时结果为零。

迁移时，返回完整 flags 的 `value` 应声明为 `auto` 或 `u128`，不能继续声明为
`u64`。读取低 64 位用 `get<u64, 0>` 或显式 `static_cast<u64>(value)`。
清除低位掩码优先使用 `opDeact<Mask>`；直接对 `u64` 掩码取反再与完整值相与
只会保留低 64 位。现有低位门位常量和定时器目标编码不变。

## 编译示例

```sh
g++ -std=c++23 -O0 -I. -o smoke examples/condition_smoke.cpp && ./smoke
g++ -std=c++23 -O0 -I. -o pipe examples/pipe_smoke.cpp && ./pipe
g++ -std=c++23 -O0 -I. -o aligned examples/type_safe_aligned_smoke.cpp && ./aligned
g++ -std=c++23 -Wall -Wextra -pedantic-errors -I. -fsyntax-only examples/uint128_smoke.cpp examples/flags128_smoke.cpp examples/looper128_smoke.cpp
```

## 文档

- [meta_states_looper.md](meta_states_looper.md) —— 引擎设计说明与用法
