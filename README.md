# meta_looper

C++23 纯头文件的**编译期循环 / 状态机**框架：一轮循环由"元生成 + 元接收"驱动，用四个监控窗口
（`pre_gen_exec` / `post_gen_exec` / `pre_rec_exec` / `post_rec_exec`）把用户的条件函数嵌进去，
阶段推进、index 记账、计时器递减与压制全部由 looper 自己的 flags 完成。

## 文件

| 文件 | 作用 |
|---|---|
| `meta_states_looper.hpp` | 引擎：门位（gates）、计时器、四个监控窗口、阶段推进与轮尾记账 |
| `meta_states.hpp` | 公开层：控制位 / 窗口号 / 字段布局 / 计时器 API、`state_fn`、`make_condition`、`run` |
| `meta_ios.hpp` | 流基础控制与用户算法组合，公开 `meta_ios::stream_states_co<Fn>` 与 `meta_ios::meta_pipe` |
| `meta_stream.hpp` | 管道：`meta_pipe` / `all_to` / `run`（实现；由 `meta_ios.hpp` 统一公开） |
| `meta_iostream.hpp` | 基于元对象协议的输入流、输出流及对齐迭代器 |
| `type_safe_aligned_iterator.hpp` | 类型安全的对齐字节缓冲迭代器（`meta_pipe` 的实际使用者） |
| `meta_objects.hpp` | 元对象协议：`meta_object` / `meta_states_object` / `meta_ret_object` / `meta_empty_o` / `flags_operator` |
| `meta_uint128.hpp` | 通用 128 位结构化整数：位运算、移位、模加减；不包含 looper 布局 |
| `meta_invoke_protocols.hpp` | `meta_invoke` / `invoke_if` 等调用协议 |
| `meta_utilities.hpp` | `exp_list`、`exp_size`、`length_*`、`value_is` 等基础件 |

`meta_stream` 的部分在本仓库里只保留管道（`meta_stream.hpp`）：它不再把
`(To, From)` 塞进一个 meta object。管道在扁平的节点槽和模板参数之间传递状态，
每轮对节点做一次 fold，不经过 MTO/MSO 的 payload。

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

实现位于内部命名空间 `meta_ios_details`，不修改底层元对象或 looper。
当前基础控制占用 `user` 字段低 4 位记录无生成器状态，并使用计时器槽 0
处理首轮推进/接收抑制；用户算法应保留这些字段，高 64 位仍可用于用户状态。

当 Generator 或 Receiver 本身就是 `meta_states_object` 时，元对象调用协议交给
条件的对象是它的 `flags`，基础流控制会直接放行（把 MSO 当作 gen/rec 位使用，
例如 `type_mask`）；此时是否停止由用户的 `Fn` 自己决定。

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
`main_con` 的存活位图由引擎维护，用户 Fn 返回后会重写高 64 位，保留低 64 位。
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
`(Condition, Receiver, Generator)` / `(Main, Nodes, Gen)` 交给 driver 开下一段。
预算是“每段轮数”：driver 的起始 `Remaining` 是 `B-1`，chunk 的 `Remaining`
递减到 0（含第 0 轮）正好 B 轮。`for_each` 的段边界也不在 chunk 里调用下一段，
而是由 driver 顺序调用，这样函数体实例化链也被打断。

- 深度从 `O(轮数)` 降到 `O(预算 + 轮数/预算)`；默认 900 深度下实测
  `index_sequence_istream<2000>` 的 looper/pipe 的 `::type` 与 `for_each` 都能过
  （见 `examples/deep_smoke.cpp`）。
- pipe 的预算按节点数自适应：`clamp(256 / (节点数 + 4), 1, 64)`，因为每轮还有
  一层 fold。
- looper 的预算可以用 `-DMETA_STATES_LOOPER_BUDGET=...` 覆盖，不影响 pipe；编译器自身的深度上限无法
  在语言内查询，所以这是固定预算，不是“到上限自动切”。
- driver 本身仍是 `轮数/预算` 层递归，runtime 的 `for_each` 也仍是一轮一层；
  真正无限长还需要 `-ftemplate-depth` 或非标准的状态化元编程。
- `exp_list::pop_back` 通过轻量列表上的索引选择前 N-1 项，不再逐元素递归累积；
  `examples/exp_list_smoke.cpp` 在模板深度 128 下覆盖 320 项列表及基础流兼容性。超大列表仍有
  编译资源开销，深流测试可用 `meta_iterator` 收尾以隔离容器开销。

### skip / yield / stop 决定

`stream_states_co` / `node_con` 的用户 Fn 除了返回 flags，还可以返回三种决定：

- `skip_t`（或 `skip()`）：本轮不收/不发，**循环继续**（不碰 `G_CONTINUE`）；
- `yield_t`（或 `yield()`）：本轮收/发，循环继续；
- `stop_t`（或 `stop()`）：结束循环。

TMP 里 `apply` 是类型别名，所以写 `using apply = yield_t;` / `skip_t` / `stop_t`；
`skip()` / `yield()` / `stop()` 是同一组标记的 constexpr 函数形式，方便在
`decltype(...)` 或 constexpr 辅助代码里用。pipe 的 skip/yield 会同时处理
`PIPE_EMIT`，不会误伤 `G_CONTINUE`。完整例子见
`examples/decision_smoke.cpp`（pipe filter、pipe stop、looper skip/yield）。

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
g++ -std=c++23 -O0 -ftemplate-depth=128 -I. -o exp_list examples/exp_list_smoke.cpp && ./exp_list
g++ -std=c++23 -Wall -Wextra -pedantic-errors -I. -fsyntax-only examples/uint128_smoke.cpp examples/flags128_smoke.cpp examples/looper128_smoke.cpp
```

## 文档

- [meta_states_looper.md](meta_states_looper.md) —— 引擎设计说明与用法
