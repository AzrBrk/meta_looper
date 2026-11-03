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
| `meta_function_binder.hpp` | `function_binder`：参数栈 = `type_safe_aligned_range`，无堆分配；`bind` / `bind_as` / 成员函数绑定 |
| `meta_shared_series.hpp` | 独立共享参数栈原型：`link_shared`、按编译期区间取参的 binder；新旧 series 均可用 `series_initialize` 直接初始化空槽位。参见 [实验与测量](examples/shared_series.md) |
| `meta_function_series.hpp` | `function_series`：多段 binder（连续 aligned_range 缓冲 + `meta_pipe` 偏移表）+ 返回值 aligned_range 缓冲；`link_f` / `series_bind` / `series_cat` / `_continue` / `drive` / `for_each` / `for_each_forward` / `run_loop`；`series[I]` 运行期子链视图 |
| `winapi_series.hpp` | 可组合 WinAPI series：module / register / create / show / message loop，`window_series` / `window_loop_series` |
| `winapi_ui.hpp` | 组件化 UI：默认绑定组件、标准控件、`place` / `with_child` / `run`；参见 [组件用法](winapi_ui.md) |
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
`meta_function_binder.hpp` / `meta_function_series.hpp` 是建在 `type_safe_aligned_range`
之上的上层组件：只依赖对齐布局、`meta_utilities` 的签名推导，以及 `drive()` 用到的
`meta_states`；不反向依赖 `meta_ios` 门面。

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
- 计时器：`set_suppression_timer` / `user_fn::set_suppression`（自动选槽）/ `tick_timers` / `effective`
  —— 压制只影响 looper 的**判定**，不改用户的 flags，计时归零后自动恢复
- 字段布局：`position`(u16, 轮尾 +1) / `user`(u8) / `timer0`(u16) / `timer1`(u16) / `gates`(u8)

### 调度器字段保护

用户条件函数的结果不会直接覆盖调度器字段。引擎在调用前提供当前窗口号，
调用后通过掩码合并：`(用户结果 & ~保护掩码) | (调度器快照 & 保护掩码)`；
随后才由引擎推进窗口、记账及递减计时器。关闭的窗口仍然完全惰性，不调用用户函数。

| 调度层 | 保护的段 |
|---|---|
| looper / pipe 节点的条件 flags | bits 0..15 的 `position`；bits 20..23 的 `stage_index` |
| `stream_states_co` / `node_con` / `wait_for_end_con` 的用户 Fn | 上述字段，以及 bits 16..19 的基础流状态（生成器延迟、无生成器、接收初始化失败及保留位） |
| pipe 的 `main_con` | bits 0..15 的 `position`；bits 64..127 的节点存活位图 |

初始化时的 position 保留，但每个实际执行的窗口会在调用前校正 stage 镜像，
不再允许用种子或用户返回值跳转窗口。用户结果对保护段的写入被忽略，不产生编译错误。
普通 looper 的 bits 16、17、19 及高 64 位可供用户使用，bit 18 保留为接收器初始化失败诊断；
stream 条件保留整个低 4 位。实际接收后仍未初始化时，底层关闭 `G_CONTINUE` 并写入失败位，
不调用窗口④的用户条件，用户不能通过该回调重新打开流。
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
- MSO 是观察者：通过 `meta_object_invoke` 始终收到完整被观察对象，包括 MSO，
  不自动初始化或解包；读取对方状态须显式使用 `Object::flags`。
  普通非 MSO 接收器仍依协议投影 payload/ret/flags，`meta_produce_t<MSO>`
  仍返回 flags。pipe main 使用 `meta_invoke` 接收完整输出对象，与观察者语义一致。
  自定义窗口 control 的第二参数也改为完整 condition，应读取 `Condition::flags`。
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
接收后的初始化失败保护则由底层 `user_flags_control_o` 的共享控制逻辑负责。
当前基础控制占用 `user` 字段低 4 位：`0b0001` 为生成器推进延迟，`0b0010`
为无生成器标志，`0b0100` 为底层保留的接收器初始化失败诊断，`0b1000` 保留不用。
首轮已有产物且允许推进时，在 `pre_gen_exec` 置延迟位并关闭
`G_ADVANCE`，下一轮同窗口清位并重新打开；不会占用计时器，也不把原本禁止推进的
pipe 中间节点或支链变成可推进节点。无生成器时 Receiver 的首轮接收抑制仍使用
计时器槽 0。用户 Fn 在基础控制之后运行，仍可修改门位；高 64 位仍可用于用户状态。

`stream_states_co` 在 `pre_rec_exec` 发现无生成器且未初始化接收器无法无参初始化时，
会提前关闭 `G_CONTINUE` 并置 `0b0100`；底层发现实际接收后仍未初始化时执行同样保护，
对普通自定义条件也生效。不再向用户 flags 注入临时执行标记。
门位关闭、计时器抑制或上游未发值导致未执行接收，不算失败。
即使流没有回调，也可通过最终结果 `::type::states` 的 user 低四位读取失败标志；
正常流结束不会设置它。

当 Generator 或 Receiver 本身就是 `meta_states_object` 时，元对象调用协议交给
条件的是完整 MSO，基础流控制会直接放行（把 MSO 当作 gen/rec 位使用，
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

本次分层对照以 `ccba889` 为旧头文件快照，使用完全相同的 compile_matrix 源，
Clang 18.1.3、默认模板深度、上述 syntax-only 选项，每项交替三次取中位数：

| rounds / nodes / stride / initialized | 时间：旧 → 新（秒） | 峰值 RSS：旧 → 新（KiB） |
|---|---|---|
| 7 / 1 / 1 / 1 | 0.77 → 0.75 | 162308 → 162364 |
| 65 / 4 / 2 / 1 | 2.11 → 2.04 | 292340 → 292612 |
| 65 / 8 / 3 / 0 | 2.14 → 2.17 | 315068 → 315348 |
| 65 / 4 / 100 / 0 | 2.04 → 2.02 | 299088 → 299128 |

这些小幅波动不证明性能优化收益；本次重点是可组合性、语义基线与依赖方向，
没有据此改写核心调度器。新增组合算法的成本需另用 composition_smoke 与真实
上层负载持续观察，不与旧 API 的此表混算。

回归验证：Clang 18 的 21 项非压力 smoke、GCC 14 的 20 项均通过；
GCC 14 的 `flags128_smoke` 保留既有解析问题，未计入通过数。
两种编译器均通过独立头文件/包含顺序检查，以及上述四组、空源
`(0,1,1,0)` 和 64 节点 `(1,64,1,1)` 共六组参数矩阵。
本次未运行 `deep_smoke`。

## Pipe（meta_pipe）

`meta_pipe` 自己就是 looper：状态是一组扁平 slot，每轮做一次 fold；中间段不再
塞进 MTO/MSO，每段是一个 `(rec, con)` 节点，`con` 只控制自己这段的流程。

```cpp
#include "meta_ios.hpp"

using offsets = meta_ios::meta_pipe<meta_iostream::meta_istream_list<int, double, char>>
                    ::all_to<meta_iostream::meta_aligned_iterator<>>
                    ::run<meta_iostream::meta_ostream<>>
                    ::type::to_t;   // exp_list of aligned seek_to objects
```

`meta_aligned_iterator` 描述的是**内存布局**（每格按 alignof 对齐，`get/emplace`
返回引用、要求缓冲区里已经有一个本机表示的 T）。读"已知的网络数据布局"用
`meta_wire_iterator`：布局默认 packed，端序是**协议**参数，机器端序只影响读写时
要不要 `byteswap`，不影响任何偏移。

```cpp
using wire    = meta_iostream::meta_wire_iterator<>;                     // packed + 大端
using wire_le = meta_iostream::meta_wire_iterator<1, std::endian::little>;

meta_states::run<meta_ios::stream_states_co<>, wire,
                 meta_iostream::meta_istream_list<std::uint16_t, std::uint32_t>>
    ::for_each([&]<class Stream>(Stream s) {
        using field = typename Stream::to_t::type;   // 当前字段类型
        auto v = s.object().read(buf);               // 按协议端序解码，按值返回
    });
```

- 偏移递推与 `meta_aligned_iterator` 完全相同（`advance_t` 只是加法）：端序不改变布局。
- `read` / `write` 按值 + `std::bit_cast` 字节拷贝，不构造对象、不要求缓冲区对齐。
- 端序与 `std::endian::native` 不同时编译期 `std::byteswap`；`float/double` 先
  `bit_cast` 到同宽整型再翻，枚举按底层类型翻，字节串/数组不翻。
- wire 布局没有尾部 padding，`field_size` 那套对齐收尾只适用于内存布局。
- **弱对齐 CPU**：`meta_wire_iterator` 的 `read`/`write` 只做字节拷贝（`std::bit_cast`），
  从不构成未对齐引用，所以在 strict-align 机器上也安全。而 `meta_aligned_iterator` /
  `type_safe_aligned_range` 在 `Alignment < alignof(T)`（典型是 packed=1）时会产生未对齐
  访问；库用 `META_IOSTREAM_ALLOW_UNALIGNED` 判断目标是否允许（x86/x86-64/arm64/
  ARMv7+UNALIGNED/ppc/s390 默认允许），不允许时直接 `static_assert` 报错，而不是运行时
  fault。确知目标能撑就 `-DMETA_IOSTREAM_ALLOW_UNALIGNED=1`。

真实链路上的验证见 `examples/wire_socket_smoke.cpp`：Boost.Asio 回环 TCP，服务器发
一帧 32 字节的 packed/大端协议帧（`magic u32 / version u8 / kind u8 / flags u16 /
device u32 / timestamp u64 / value double / length u32`），客户端用
`meta_wire_iterator` 在 looper 语境里解码、改一个字段再编码回传，服务器再解一遍核对。
同一个 `Ty...` 下 `field_size<1, ...>()` 是 32（packed），`field_size<0, ...>()` 是 40
（alignof 布局），测试里两个都有断言。

### Benchmark

`bench/wire_decode_bench.cpp`（无第三方依赖，`chrono` + `DoNotOptimize`）：32 字节帧 ×
65536，7 种路线各跑 25 次取最小值。`g++ -std=c++23 -O2 -I. -o bench_wire
bench/wire_decode_bench.cpp`。

| 方案 | ns/帧 | vs 手写 | .text |
|---|---|---|---|
| D 手写单程（memcpy+bswap → native struct） | 3.606 | 1.00× | 128 B |
| A `meta_wire_iterator` 只解码 | 3.610 | 1.00× | 192 B |
| B 单循环两游标（wire + `slot_at<>`） | 3.606 | 1.00× | 128 B |
| C **fork**（wire 主线 + aligned 支线） | 3.606 | 1.00× | 128 B |
| E 两趟（先 packed struct 再转 native） | 5.864 | 1.63× | 160 B |
| F `#pragma pack(1)` 原地读（未对齐/UB） | 3.606 | 1.00× | 128 B |
| G 只求和（地板） | 0.444 | 0.12× | — |

（小端主机、协议大端；协议本身小端时同理，E 仍然 1.69×。）

结论：`fork` 与手写单程**完全同速同体积**，抽象零成本；两趟方案才是真代价；
`packed` 原地读并没有比安全版更快（所以没必要冒 UB 的险）。注意这是热缓存、
32 B/帧的访存受限场景，G 与 A/C 的 8 倍差距就是解码本身的开销。

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

### fork：一条源、两条支链

```cpp
using p = meta_pipe<source>::all_to<meta_wire_iterator<>>   // 主线：wire 读
                    ::fork<meta_aligned_iterator<>>          // 支链：aligned 写
                    ::run<>;
```

- 两条线吃**同一个源元素**（同一个 index 帧）。主线节点推进 gen，`folded::gen_out`
  原样交给支链，支链自己的 `G_ADVANCE` 关，所以不会消费第二次；
- **`fork` 之后写的 `all_to` 全部接在支链上**（主线到此结束），所以支链可以是一整条链；
- 两条线的末端接收器保存在 `pipe_fork<主线接收器, 支链接收器>` 中，`for_each` 回调里用
  `s.main()` / `s.branch()` 取各自的 `meta_objects::meta_produce_t<receiver>`；
  `::main_t` / `::branch_t` 也是对应产出类型：普通和 timer 对象取 `::type`，
  ret 对象取 `::ret`，MSO 取 `::flags`，未初始化对象取 `meta_uninitialized`。
  未 fork 时 `main()` 仍按协议取主线产出，`branch_t` 为 `void` 且不可调用 `branch()`；
  `::forked` / `::branch_nodes` 可做编译期判断。`to` / `to_t` 等原有载荷接口不变；
- 存活位图整字 64 位按编译期偏移切两段：主线 `bit 0..M-1`，支链 `bit M..M+B-1`，
  所以 **主线 + 支链总节点数 ≤ 64**；`main` 只看主线位图，支链死活不影响 `G_CONTINUE`
  （主线活着就继续），只体现在位图和产物里；
- 典型用途：同一份线上字节，一条线按 packed + 大端解码，另一条直接写原生对齐布局
  （`examples/pipe_fork_smoke.cpp`）。
`main_con` 的存活位图和 position 由引擎维护，用户 Fn 返回后恢复这两段，其余低位保留。
初始 main 关闭 `G_CONTINUE` 时不执行任何轮次。

looper 的单步由 `meta_states_looper_step` 提供；pipe 保留独立的节点单步实现，
采用相同的四窗口语义，并增加 emit / flush 规则。中间节点
默认 `G_ADVANCE` 关，因为链上只有 receiver，没有独立的 generator。回调里用
`.index()` 取轮次；`.left()` 是 payload 长度，对 pipe 节点没有意义。

旧写法 `all_to<os, protocols::wait_for_end_to_t>` 现在写成
`all_to<os, con<wait_for_end_con<>>>`：一样是“活着只收不发、结束时只发最终值”。
如果这段就是节点 0（要推进源），用 `con<wait_for_end_advance_con<>>`。

`type_safe_aligned_iterator` 的 type_mask 现在用满 `meta_states_object` 的 128 位
flags：每步 `flags = (flags << 1) | 命中位`，走完 N 个类型后按 N 位反转一次，
不再需要单独的计数字段，类型上限从 64 提到 128（`aligned_iterator_storage` 里有
`static_assert`；超过时直接拒绝，不会给出错误的 `safe()`）。槽代理的 copy assignment 已删除：`*it1 = *it2` 会编译失败，要写值请用
`*it = value`。这也使正向与反向迭代器的隐式 copy assignment 被删除；
它们仍支持 copy construction。

布局的对齐边界是第一个模板参数（与 `meta_aligned_iterator<Alignment>` 一致）：
`0`（默认）每格按 `alignof(Ty)` 对齐、末尾按最大类型对齐收尾；给 2 的幂则所有
槽位统一用这个边界，`1` 就是 packed（线上/文件格式，没有 padding）。

```cpp
static_assert(field_size<0, char, int, double>() == 16);  // 内存布局
static_assert(field_size<1, char, int, double>() == 13);  // packed
type_safe_aligned_range<1, char, int, double> packed{buf};  // 直接写 array::data()
```

`initialize(range, ...)` / `begin` / `end` / `rbegin` / `rend` 都跟着 range 的
Alignment 走，不需要另外传参。

槽位类型可以写引用：`type_safe_aligned_range<0, int&, double const&>` 会被
`stored_type_t` 归一化成 `std::reference_wrapper<int>` /
`std::reference_wrapper<double const>`，布局、偏移表、掩码、流一律按 wrapper 计算。
这只是一层**类型**归一化，值层面不做隐式包装：`initialize(range, ...)` 里仍然要
写 `std::ref(x)` / `std::cref(x)`，读的时候用
`static_cast<std::reference_wrapper<T>&>(*it).get()`，或者直接
`os << *it`（wrapper 会隐式转回 `T&`）。反向不支持：`const&` 槽位不能用 `>>`
写回，含 const 引用的列表整体 `>>` 也会编译失败。

（`initialize` 要和 `meta_objects::initialize` 消歧，调用时请写
`type_safe_aligned::initialize(...)`。）

#### 运行期 `r[i]` / `at<T>(i)` 与编译期 `get<I>`

`type_safe_aligned_range` 同时是 range 和 tuple-like，三个入口分工明确：

```cpp
type_safe_aligned_range<1, u32, u8, u8, u16, u32, u64, double, u32> r{buf};

// 运行期下标：均匀的受检代理（按值返回，两个机器字），越界抛 index_exceed
auto p = r[k];                        // aligned_iterator_storage<Ty...>
double v = static_cast<double const&>(p);   // 槽位类型不符（或为空）时抛
r[7] = u32{99};                       // 代理也能直接写
size_t n = r.size();                  // 槽位数（编译期常量）

// 类型已知、下标仍在运行期：受检的精确类型引用
u64& timestamp = r.at<u64>(k);        // k 必须为 5，否则抛异常
r.at<u32>(7) = 99;                   // u32 有多个槽位，保留偏移查表

// 编译期下标：该槽位的精确类型引用，零检查（base + 常量偏移 + launder）
get<3>(r) = u16{0x1234};              // 就是 u16&
double d = r.get<6>();                // 成员形式

// 结构化绑定（tuple-like 协议：tuple_size / tuple_element）
auto& [magic, version, kind, flags, device, ts, value, length] = r;
```

`r[i]` 没有指定目标类型，不能随运行期下标改变静态返回类型，所以返回独立代理。
`at<T>(i)` 则由调用方指定 T，返回 `T&`：按现有类型位图匹配（允许加 const，不允许
去 const；引用槽位按 `std::reference_wrapper` 存储）。唯一匹配时先验证
`i == 静态槽位号`，再用常量绝对偏移访问，不加载运行期偏移表；多个匹配时沿用
`static_cast<T&>(r[i])` 的受检查表路径。T 在布局中完全不匹配时编译期拒绝。
异常类型与代理相同，优先级固定为 `index_exceed`、`null_pointer`、`type_not_safe`。
所有引用访问都要求相应对象已经构造，`at<T>` 不负责开始对象生命周期。

`const` 是视图语义：`data() const` 本来就返回可写指针（与 `std::span` 一致），
所以 const range 上 `at<T>()` / `get<I>()` / 结构化绑定拿到的仍然可以是可写引用，
`tuple_element_t<I, const range>` 也仍是 `T`（不带 const）。

开销（见[异构内存 benchmark](#异构内存访问stdany--stdvariant--type_safe_aligned_range)）：
`get<I>` 与 native struct 同速（0.98×），`r[k]` 是 1.28×（越界检查 + 从静态
偏移表取偏移），`begin()` 后走 k 步 `++` 是 1.37×。位置在编译期已知时用
`get<I>`；位置在运行期时 `r[k]` 与"迭代器带下标构造"编译出完全相同的代码。
`slot_at<I>(r)` 仍然保留：需要**游标**本身（`address_of` / 在未构造的槽位上
`emplace` / `destroy`）时用它，`get<I>` 只给引用。

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

- `driver_fn<Policy>` 有 `round` / `more` / `decide` 三个窗口；默认
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
- 自定义 policy 需提供 `seed_remaining` 和 `round` / `more` / `decide` 窗口（继承 `default_driver_policy<Budget>` 即可）。looper 用 `meta_states::looper_with_policy` /
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

## 异构内存访问：`std::any` / `std::variant` / `type_safe_aligned_range`

`bench/hetero_access_bench.cpp`。同一条记录（8 个异构字段
`u32,u8,u8,u16,u32,u64,double,u32`）用五种存法：

| 存法 | 形态 | B/记录 |
|---|---|---|
| native | `std::vector<native_rec>`（自然对齐） | 40 |
| any | `std::vector<std::array<std::any,8>>` | 128 |
| variant | `std::vector<std::array<std::variant<u8,u16,u32,u64,double>,8>>` | 128 |
| `range<1>` | 一块 `std::byte` + `type_safe_aligned_range<1, ...>`（packed） | 32 |
| `range<0>` | 同上但 `Alignment = 0`（自然对齐，与 `native_rec` 同布局） | 40 |

```sh
g++ -std=c++23 -O2 -falign-functions=64 -falign-loops=32 -I. \
    -o bench_hetero bench/hetero_access_bench.cpp
./bench_hetero --typed-only  # 只运行 G7
```

（65536 记录 / 25 次取最小，单位 ns/记录。）

**测量前提**：对齐开关不是可选的。同一份机器码换个地址会落在不同的 cache line /
BTB 槽上，实测能差 22%（不带对齐开关时，两个编译结果逐条相同的函数分别测出
1.59 / 1.29 ns）。加 `-falign-functions=64` 后同码不同址的差异降到 3% 以内，
所以下面只相信 >5% 的差值。

**G1 字段位置是编译期常量**（正常解码循环的形状）：

| 方案 | ns/rec | vs 地板 | ns/op |
|---|---|---|---|
| native struct | 2.878 | 1.00× | 0.360 |
| `std::any` + `any_cast<T>` | 3.209 | 1.12× | 0.401 |
| `std::variant` + `std::get<T>` | 3.090 | 1.07× | 0.386 |
| `range<1>` + `slot_at<I>` | 2.846 | 0.99× | 0.356 |
| `range<0>` + `slot_at<I>` | 2.823 | 0.98× | 0.353 |
| `range<1>` + `get<I>(r)` | 2.844 | 0.99× | 0.356 |
| `range<0>` + `get<I>(r)` | 2.823 | 0.98× | 0.353 |

**G2 位置是运行期值**（每记录读 1 个字段，k = i & 7）——这就是"运行时迭代器"：

| 方案 | ns/rec | vs 地板 |
|---|---|---|
| native `switch` | 1.332 | 1.00× |
| `range<1>` / `range<0>` `switch (k) → get<I>` | 1.302 | **0.98×** |
| `std::variant` + `std::get` | 1.501 | 1.13× |
| `std::any` + `any_cast` | 2.005 | 1.51× |
| `range<1>` `r[k]`（受检代理） | 1.669 | 1.25× |
| `range<0>` `r[k]`（受检代理） | 1.672 | 1.26× |
| `range<1>` 迭代器带下标构造 | 1.709 | 1.28× |
| `range<0>` 迭代器带下标构造 | 1.672 | 1.26× |
| `range<1>` `begin()` 后走 k 步 `++` | 1.784 | 1.34× |
| `range<0>` `begin()` 后走 k 步 `++` | 1.788 | 1.34× |

`r[k]` 与"迭代器带下标构造"是同一份代码（125 条指令逐条相同），1.25× 的代价来自
越界检查 + 从静态偏移表取偏移再相加；走 k 步 `++` 只多 ~6%。

**源码检查不等于机器码成本**：下面列出接口承担的检查，编译器可能合并或消除它们。

| | 检查了什么 | 代价 |
|---|---|---|
| `std::get<T>(v)` | 存下来的 tag == `index_of<T>`（一次 cmp 立即数）；值在 variant 里的位移是常量 | 1 次比较 |
| `r[k]` 代理 | 索引范围、空指针、类型位图；定位时查询静态偏移表 | 优化后不一定全部保留 |
| `r.at<T>(k)` | 同样的受检语义；T 唯一匹配时比较静态槽位号，再取常量偏移 | 唯一匹配不查偏移表；多匹配沿用代理路径 |
| `get<I>(r)` | 什么都不查（`base + 常量偏移`） | 0 |

GCC 14.2 的 G2 反汇编中，range 的类型位图检查已经消失，留下的关键路径是
`index -> offsets[index] -> 对象地址 -> 对象加载`；variant 的 switch 分支则直接用
常量字段地址。不能把差值直接归因于三次检查，更不能仅由时间反推出分支预测命中率。
已定位代理不再查偏移表；在双方指向同一批对象的对照中，它可以达到 variant 的读取水平。

**G7 类型已知、下标在运行期的受检访问**：对照 `static_cast<T&>(r[k])` 和
`r.at<T>(k)`，并保留 variant 的受检读取作为参考。索引来自运行期数组，数据在计时前
生成；每轮通过编译器屏障防止重复扫描被外提，交替测量并报告中位数和校验结果。
同时测量唯一匹配的 u64 和在三个槽位间变化的 u32，后者用于监测回退路径退化。

唯一匹配优化针对的实际调用链也包括 `function_binder::at<T>(index)`：
原来先通过 `operator[]` 加载偏移，再转换并检查 T；现在由 range 融合检查与定位。
运行期下标、空指针和类型检查仍然保留，不是用不受检的 `get<I>` 替换运行期接口。
收益取决于编译器、内联情况和工作集，不能保证所有布局都提升 30%。

AMD EPYC 9V74、固定 CPU 0 的一次 G7 实测如下（ns/访问，15 次中位数，每次扫描
32 遍；GCC 14.2 使用上述开关，Clang 19.1 使用 `-O2 -falign-functions=64`）：

| 编译器 | 记录数 | 唯一 u64：代理 | 唯一 u64：`at<T>` | 耗时降低 |
|---|---:|---:|---:|---:|
| GCC 14.2 | 4096 | 0.818 | 0.544 | 33.6% |
| GCC 14.2 | 65536 | 0.843 | 0.576 | 31.6% |
| Clang 19.1 | 4096 | 1.531 | 0.565 | 63.1% |
| Clang 19.1 | 65536 | 1.595 | 0.626 | 60.8% |

多匹配 u32 的两条路径基本持平。两种编译器的唯一匹配热路径都只剩槽位相等判断、
空指针检查和常量偏移加载；越界判断移入失败路径，异常优先级不变。
失败时统一构造一次异常，避免多个 throw 站点阻碍内联；Clang 的原代理转换未内联，
新入口已内联，因此它的收益还包含消除逐次函数调用。GCC 的基线和新入口均已内联。

**G3 完全没有位置信息**（逐槽位动态判定类型，通用 visitor 形状）：

| 方案 | ns/rec | vs 地板 |
|---|---|---|
| native struct | 2.879 | 1.00× |
| `range<1>` / `range<0>` `type_mask_series` 位图 + 运行时迭代器 | 8.130 | 2.82× |
| `std::variant` + `std::visit` | 9.482 | 3.29× |
| `std::any` + `typeid` 链 | 57.710 | **20.0×** |

**G4 24 字节 blob**（超过 `std::any` 的 SBO）：

| 方案 | build | read |
|---|---|---|
| `range<1>`（`initialize`/`get<I>`） | 0.603 | 0.975 |
| `range<0>`（同上，40 B/记录） | 0.700 | 0.996 |
| `std::variant`（`emplace` / `get`） | 1.224 | 1.083 |
| `std::any`（`emplace` / `any_cast`） | 30.9 | 1.292 |
| `std::any`（临时对象 + 赋值） | 34.3 | — |

**G5 未对齐到底值多少钱**（同一个 u64，只把地址平移，步长 64 B 使每格独占一行）：

| 地址 | ns/load | vs 对齐 |
|---|---|---|
| `offset 0`（自然对齐） | 0.557 | 1.00× |
| `offset 4`（跨 8 字节边界，仍在同一行内） | 0.546 | 0.98× |
| `offset 60`（每次访问都跨 cache line） | 0.584 | 1.05× |

**G6 空间密度**（1M 条记录，packed 32 MiB vs variant 128 MiB，工作集远超 L3）。
GB/s 用"每记录字节数 / 每记录纳秒数"算，它表示"这个布局实际要从内存搬多少字节"：

全记录扫描（每记录 8 次访问，计算量大）：

| 方案 | ns/rec | vs 地板 | B/记录 | GB/s |
|---|---|---|---|---|
| `range<1>` packed | 3.080 | 1.00× | 32 | 10.4 |
| native struct | 3.171 | 1.03× | 40 | 12.6 |
| `range<0>` aligned | 3.181 | 1.03× | 40 | 12.6 |
| `std::variant` | 5.589 | **1.81×** | 128 | 22.9 |
| `std::any` | 5.602 | **1.82×** | 128 | 22.9 |

薄扫描（每记录只读 1 个 `u32`，循环变成纯搬运；这里给的是 5 次运行的区间，
因为 packed 的 32 MiB 有一部分能进 L3，会时而命中时而落内存）：

| 方案 | ns/rec | vs 地板 | B/记录 | GB/s |
|---|---|---|---|---|
| `range<1>` packed | 0.95–1.17 | 1.00× | 32 | 27–34 |
| native struct | 1.11–1.25 | 1.0–1.3× | 40 | 32–36 |
| `range<0>` aligned | 1.38–1.51 | 1.2–1.6× | 40 | 27–29 |
| `std::any` | 3.01–3.15 | **2.6–3.3×** | 128 | 41–43 |
| `std::variant` | 3.34–3.46 | **2.9–3.6×** | 128 | 37–38 |

**空间账**：`std::variant` 的每槽大小 = `sizeof(最大备选)` 再补到 `alignof` 上，
而且**还要额外存一个 tag**——而 range 里"位置就是类型"，tag 完全不必要，每格只按
自己的 `sizeof` 排。这条记录 8 个字段共 32 B、最大备选 8 B → variant 每槽 16 B
（8 payload + 1 tag + 7 padding）× 8 = 128 B，正好 **4×**。

字段越碎、最大类型越大，差距越大：`u8` 放进 `variant<u8,u64>` 就是 1 B → 16 B；
20 个小整数字段 + 1 个 `u64` 的协议，variant 要 21 × 16 = 336 B，packed range 约 26 B
（**约 13×**）。

**密度什么时候变成时间**：全记录扫描里 packed 还没摸到带宽（3.08 ns 是算力地板，
它的 32 B 只要 1.4 ns 的带宽），所以只赢 1.8×；薄扫描里两边都贴着流式带宽（GB/s
一列全在 27~43，说明 ns 几乎正比于字节数），于是拿到 **2.6~3.6×**，方向就是理论上
的 4×（差的那一点是因为每个布局各自停在不同的有效带宽上，packed 还会部分命中 L3）。
也就是说：**派发受限时密度看不出来，搬运受限时它直接变成吐吞**。
`native struct` 与 `range<0>` 两行同样是 40 B 却差 20% 上下——它们的循环指令完全
相同（只差 `add $0x28` 还是 `add $0x20`），这是落址噪声，所以那两行读作"1.0~1.6×"。

结论：

1. **`Alignment = 0` 和 `Alignment = 1` 在这台 x86 上没有任何可测差别**——G1~G4
   里两列的每一行都在噪声内（≤2%），而 `Alignment = 0` 还要多花 25% 内存
   （40 B vs 32 B/记录）。所以"未对齐存储会拖慢 CPU"这条直觉在这里不成立。
2. G5 解释了原因：**行内未对齐在 x86 上完全免费**（0.439 vs 0.439，第二轮甚至连
   0.98× 都测出来），**真正有代价的是跨 cache line**，而且只有 5%~6%（顺序访问时
   预取器已经把下一行拉过来了）。而 32 字节记录里的 `u64@12` / `double@20`
   根本不会跨 64 字节行。
3. 那 `Alignment = 0` 还有什么用？**可移植性**，不是速度：弱对齐目标（ARMv7、
   老 MIPS）上 `layout_access_ok<1, ...>` 直接编译期拒绝 packed 布局，
   而 `Alignment = 0` 到处都能编译。所以选法是语义驱动而不是性能驱动：
   **线上/文件格式用 1（布局就是协议布局，反正后面还要逐字段 byteswap），
   内存布局用 0（每格对上 alignof，任何目标都能跑）**。
4. **定点访问时 `get<I>` 连 1% 都不用付**——`base + 常量偏移`，比 native struct
   还略快。`any`/`variant` 的 7~12% 就是每字段一次比较 + 一条不会走到的抛异常分支。
5. **"类型安全检查"本身不是慢的原因，回答的问题多才是**：`std::get<T>` 只比一次
   tag，`r[k]` 代理要比 tag 更强的三件事（区间、非空、类型位图），所以 G2 里
   1.25× 对 1.13×；但把类型放回编译期（`switch (k) → get<I>`）就是 0.98×，
   反超 `variant` 和 native。**能静态知道的就别交给类型擦除**。
6. **没有位置信息时差距才拉开**：`any` 20×，`range` 位图链 2.82×，
   `variant` 跳表 3.29×。位图链比 `visit` 跳表还快，因为 8 个槽位的类型分布
   静态已知，分支预测器全记得住。这也是代理唯一无法被 `get<I>` 替代的场景。
7. **空间上 range 是压倒性的**：32 B vs 128 B = 4×（G6）。`variant` 每槽的大小由
   最大备选决定，而 range 每格只按自己的 `sizeof` 排；`variant` 还得额外存一个
   tag，而 range 里“位置就是类型”，tag 完全不必要。薄扫描（搬运受限）里这个 4×
   直接变成 **2.6~3.6× 的吐吞差**；全记录扫描（算力受限）里只体现为 1.8×。
   字段越碎、最大类型越大，差距越大。
8. **`std::any` 的真正问题在构造而不是读取**：24 字节载荷要堆分配，
   build 慢 50×，read 只慢 1.1~1.3×。它也是 128 B/记录（SBO 存不下就再加一次堆分配）。
9. 编译成本：这个 TU（32 个变体）`-O2` 编译约 4.3 s、`.o` 约 95 KB——模板的开销在
   编译期，不在运行期。

三个写基准的坑（都在这里踩过）：

- 累加器必须**按字段位置拆开**（`acc[k]`）。单链 `s += ...` 时 8 次串行 FP 加法
  （每次 4 周期延迟）就是瓶颈，四种方案全被压成 5.4 ns/记录——测到的是加法延迟。
- 微基准必须对齐函数；否则同一个实现换个地址差 22%，会得出"这个版本快 20%"
  的错误结论。看到"快 10~20%"时先 diff 两边的反汇编。
- 结论要在多次运行间复现。`range<1>` 与 `range<0>` 的 `++` 那一行第一次差 11%，
  第二次完全一致——那是实例化落址的运气，不是对齐的功劳。同一次加行之后
  `native switch` 还出现过 1.84 的离群值，下一版就回到 1.33。

## 函数绑定与函数链：`function_binder` / `function_series`

对标 select_type 的 `exp_function_binder` / `exp_function_series`，但存储形态完全不同：

| | select_type 旧写法 | 本仓库 |
|---|---|---|
| 参数栈 | `std::tuple` + `exp_node` 链表，每槽一个 `shared_ptr` 节点 | 一块内联 `std::byte` 缓冲 + `type_safe_aligned_range` 布局 |
| 堆分配 | 构造时 N 次 `make_shared` | 0 |
| 运行期下标 | 沿链表递归 + `typeid` 检查 | 槽位号分派 + 编译期偏移表 |
| 返回值 | `std::tuple` | 同样按 `aligned_range` 布局的对齐缓冲 |
| 组合 | `exp_shared_node` 串 EFB | binder 也排在 aligned_range 缓冲里（`meta_pipe` 生成偏移表） |

### 绑定

```cpp
int add(int, int);
auto b = meta_bind::bind(add);
b.bind(2);
b.bind(3);
b();                    // 5

b(10, 20);              // operator()(args...) 重写前两个槽位再调用
b[0] = 7;               // 运行期下标：受检代理
b.at<int>(1) += 1;      // 精确类型引用
b.bind_at<0>(40);       // 编译期槽位号，跳过运行期分派
```

签名推导不出来（泛型 lambda / 重载 `operator()`）时写 `bind_as<R(L...)>(f)`；
成员函数写 `bind(obj, &T::fn)`（binder 里只存对象地址和成员指针，obj 必须活得比 binder 长）。
参数槽映射：`int`→按值；`int&`→存指针，调用时解引用（绑定左值或 `std::ref`）；
`int const&`→保存一份值；`int&&`→保存值、调用时 move。
只能移动的类型（`unique_ptr` 等）也可以作为参数/返回值：调用时按右值传、跨段喂参数时
move，代价是原槽位变成 moved-from（要再次调用就重新 bind）。每个 binder 最多 64 个参数
（存活位图是 64 位）。

参数在第一次 `bind` 时才 placement-new，所以不要求默认可构造；`bind` 越界、类型不符、
调用时没绑满都抛 `meta_bind::exception`。`Alignment` 模板参数与 range 一致：
`bind<1>(f)` 用 packed 参数栈，没有 padding。

### 函数链

```cpp
auto s = meta_bind::link_f(
    [](int a, double b) { return b - a; },
    [](double c) { return std::to_string(c); },
    []() { return 123; });

meta_bind::series_bind(s, 1, 1.33, 0.33);   // 按段顺序灌参数
s._continue();                              // 依次执行
s[1]._continue();                           // 运行期子链视图：从第 1 段跑到结尾
s.select(1)._continue();                    // 旧式游标：只设 series_index_ 再 continue
s.template at<1>()._continue();             // 编译期起点，展开成直接调用

auto [first, second, third] = s._return();  // 返回值都在 range 里
s._return().get<1>();                       // 编译期下标，精确类型
s._return()[2];                             // 运行期下标，受检代理
s.final_return();                           // 最后一段返回值
```

`series[I]` 返回的是**运行期子链视图**（不是 `*this`，也不是取第 I 个 binder）：
视图持有起始段号，`_continue()` / `_bind_continue()` 用 `stage_offsets` +
static constexpr thunk 表从 I 走到结尾，不需要为前 I 段生成运行期守卫；
`series[I].bind(...)` / `execute()` 操作视图当前段。`series[size()]` 是 end 视图（no-op）。
要旧库那种“只设游标”的入口用 `select(I)`；要编译期展开的起点用 `at<I>()`。
取某个 binder 本身用 `stage<I>()`（连续缓冲版另有 `stage_address(i)`）。

上一段返回值自动喂下一段：`s.bind(...); s._bind_continue();`。
`series_cat(a, b, ...)` 按顺序拼接多条 series：参数栈和返回值缓冲一起延长，跨边界的返回值
由 `_bind_continue` / `drive` 自动喂给下一条的第一段。拼接本身走标准 looper 管线：

```cpp
using generator = meta_iostream::meta_istream<
    exp_list<First::binders_list, Second::binders_list, ...>>;
// meta_jostream 把每个 exp_list<Binders...> 摊平；
// 最后一轮 pack::to_t 就是 exp_list<AllBinders...>；
using combined = typename decltype(stage)::to_t::template to<function_series>;
```

`stream_states_co<last_round_condition<N>>` 只让最后一轮回调，N 条 series 都作为
`for_each` 额外参数；回调里 `combined::from_series` 把每个 stage 各 move 一次进合并缓冲。
**不经过 `std::tuple`、`std::apply`，也不手写 concat_lists**（6 段链实测 `sizeof` 184 B）。
返回值缓冲在 series 构造时按默认值构造，执行时写回；`void` 段占位 `nullptr`。
返回值用 `field_size` / `aligned_offset_stream` 排版，不再走 `std::tuple` 的布局，
所以 `_return()` 本身就是一个 `type_safe_aligned_range` 视图。

### `for_each` / `for_each_forward`：回调、返回值和决定

`for_each` 直接复用 looper 的 `for_each` 语义：每段执行完回调一次，额外参数原样转发，
回调返回值按 looper 的规则处理（`stop_t` 停止后续段、`skip_t` / `yield_t` 只影响观察，
否则返回最后一个观察值）：

```cpp
int calls = 0;
auto last = s.for_each([](auto stage, int* count) {
  constexpr std::size_t index = stage.index();   // consteval
  ++*count;
  return static_cast<int>(index) + 100;          // 最后返回 102
}, &calls);

auto stopped = s.for_each([](auto stage) {
  if constexpr (stage.index() == 1) return meta_states::stop_t{};
  else return 0;
});  // 段 2 不执行，返回 stop_t
```

`for_each_forward(f, first, rest...)` 每个 stage 拿一个额外实参（和 looper 的
`for_each_forward` 一致）；`for_each` 只执行各段，不自动把上一段返回值喂给下一段
（要链式喂值用 `_bind_continue` / `drive`）。`stage.index()` 是 consteval，所以可以在
回调里直接 `self.template _return<stage.index()>()`（把 series 作为额外参数传进去）。

### 用 looper 状态机跑链

```cpp
s.reset();
s.bind(nullptr);
s.drive();        // 每段一轮，driver 推进/分块
s.run_loop(5, [&](auto results) { ... });   // 重复 5 次，回调拿本次返回 range
```

`drive()` / `for_each()` 走的是标准 looper 管线，没有自造 receiver/条件：

```cpp
using loop = meta_states::run<
    meta_ios::stream_states_co<>,
    meta_iostream::meta_jostream<exp_list<>>,
    meta_iostream::index_sequence_istream<stage_count>>;
loop::for_each(...);   // drive() / for_each() 共用同一个 series_loop
```

`meta_jostream<>` 在这个仓库里要写成 `meta_jostream<exp_list<>>`（它需要一个收集列表的
初始类型）。`index_sequence_istream<stage_count>` 产生恰好 N 轮，`stream_states_co<>`
自己处理流结束，不需要自定义 stop 条件，也不需要 repeat + 手工关 `G_CONTINUE`。
每轮回调里的段编号是 `consteval`，所以段执行仍是编译期下标，没有运行期 switch。
`_continue()` 仍是最快的展开路径；对 3 段平凡函数，两者在这个基准里相同（见下表）。

返回值 range 是视图：`run_loop` 回调里拿到的 `results` 指向 series 的缓冲，
下一次迭代会覆盖，需要保留就拷出来。

### WinAPI 实测（`examples/function_series_winapi.cpp`）

Windows 上直接绑定 `GetModuleHandleA` / `GetCurrentProcessId` / `CreateWindowExA`（12 个参数）
/ `DestroyWindow`，再把 `GetModuleHandleA -> 适配 lambda -> DestroyWindow` 串成链：

```cpp
auto s = meta_bind::link_f(
    GetModuleHandleA,
    [](HMODULE m) { return CreateWindowExA(..., (HINSTANCE)m, nullptr); },
    DestroyWindow);
s.bind(nullptr);
s._bind_continue();
auto [module, window, destroyed] = s._return();
```

`drive()` 和 `run_loop(5, ...)` 各再跑一遍：5 次创建/销毁全部成功。STATIC 是系统预注册类，
不需要消息循环，也不弹窗。非 Windows 构建下该例子直接输出 skipped 并 PASS。

### WinAPI 窗口流程：可组合 series（`winapi_series.hpp`）

零散的 WinAPI 调用被拆成可以命名的段，再用 `series_cat` 拼成一条 series：

```cpp
auto create = winapi_series::window_series(config);   // module -> register
                                                       // -> create -> show
create.bind(nullptr);          // GetModuleHandleA 的参数
create._bind_continue();
HWND hwnd = create.final_return();

auto loop = winapi_series::window_loop_series(config); // + message loop
loop.bind(nullptr);
loop._bind_continue();
int exit_code = loop.final_return();
```

也可以按需组合，和 `window_loop_series` 完全等价：

```cpp
auto s = meta_bind::series_cat(
    winapi_series::module_handle_series(),     // LPCSTR -> HMODULE
    winapi_series::register_class_series(cfg), // HMODULE -> HMODULE
    winapi_series::create_window_series(cfg),  // HMODULE -> HWND
    winapi_series::show_window_series(cfg),    // HWND -> HWND
    winapi_series::message_loop_series(cfg));  // HWND -> int
```

- 4 段 `window_series` 的 `sizeof` 是 **416 B**，5 段 `window_loop_series` 是 **536 B**
  （GCC 16/Clang 23 实测；config 按值捕获，字符串用字面量）；没有堆分配、`std::function`、
  链表或每槽 `shared_ptr`。
- 每段仍是普通 binder：`series[I]` 运行期视图、`at<I>()` 编译期起点、`series_bind`、
  `_return()` 全部适用。
- `default_window_proc` 在 `WM_TIMER` / `WM_DESTROY` 时投递 `WM_QUIT`；示例用
  `SW_HIDE` + 20 ms timer，所以 smoke 不会卡在消息循环里。
- `window_config` 里的字符串必须是活到 series 之后的字面量/静态串；`window_series`
  不会复制字符串内容，只保存指针。

### 组件化 UI：带默认值的 binder（`winapi_ui.hpp`）

每个 UI 组件都是普通 `function_binder`，构造时把全部参数绑成默认值；用户只改自己关心的
槽位，然后 `link`：

```cpp
auto create = winapi_ui::create_window();          // 全部参数已有默认值
create.set(winapi_ui::create_arg::title, "My Window");
create[winapi_ui::create_arg::width] = 320;        // 精确类型的槽位

auto s = winapi_ui::link(
    winapi_ui::register_class(),                   // HMODULE()
    create,                                        // HMODULE -> HWND
    winapi_ui::show_window(),                      // HWND -> HWND
    winapi_ui::message_loop());                    // HWND -> int

s._bind_continue();                                // 组件链：上一段返回值喂下一段
int exit_code = s.final_return();

auto w = winapi_ui::window();                      // 默认四段组合
w.stage<1>().set(winapi_ui::create_arg::title, "window()");
w._bind_continue();
```

- `set(enum, value)` 走 binder 的 `bind_slot` 构造/赋值路径，所以 `std::string` 槽可以
  直接吃字符串字面量；`binder[enum] = value` 要求槽位类型精确匹配。
- 组件用 `front_feed` 标记“上一段返回值覆盖 slot 0”：`register -> create` 是 `HMODULE`，
  `create -> show -> message_loop` 是 `HWND`；其余参数默认值保留。
- 槽位 enum：`register_arg` / `create_arg` / `show_arg` / `loop_arg` / `destroy_arg`；
  选项 enum：`window_style` / `window_style_ex` / `class_style` / `show_command`。
- `message_loop` 默认 `timer_ms = 0`（不装 timer，正常窗口靠关闭消息退出）；
  smoke 里 `set(loop_arg::timer_ms, 20)` 让默认窗口过程自动 `PostQuitMessage`。
- 4 段组件 series `sizeof` 约 296 B（GCC）/ 272 B（Clang），3 段约 256 / 232 B；
  字符串由组件的 `std::string` 槽拥有，binder 本身没有链表/每槽堆分配。
- 组件链用 `_bind_continue()`（或 `drive()`）而不是 `_continue()`：前者会把上一段
  返回值喂给下一段，后者只按已绑参数执行。

### 尺寸与开销

`bench/function_binder_bench.cpp`，GCC 16.2 / Windows x64 / `-O2 -falign-functions=64
-falign-loops=32`，交错测量 25 组 × 1,048,576 次取最小值；重复运行间的波动约 3~5%。
被绑定函数是 `double mix(int,double,int)`。

sizeof（字节）：

| 对象 | 字节 |
|---|---|
| `binder<int>` | 24 |
| `binder<int,double,int>` | 48 |
| `binder<int,double,int>`，packed 参数栈 | 40 |
| `binder<int,double,string>` | 64 |
| `binder<>` | 24 |
| `series<3 stages>`（连续缓冲） | 112 |
| `series<3 stages>`（tuple 段对象，参考） | 104 |
| `window_series`（4 段 WinAPI） | 416 |
| `window_loop_series`（5 段 WinAPI） | 536 |
| `std::function + std::tuple` 基线 | 64 |
| 旧库存储形状（`std::function` + tuple + 每槽一个 `shared_ptr` 节点） | 112 |

（后两个基线用 libstdc++ 的 32 B `std::function`；MSVC 上 `std::function` 是 64 B，
旧库的 typed node 每槽还要 32 B，实际比 112 更大，这里已经是对旧写法有利的下界。）

复用 binder 时 `bind(3 个参数) + 调用`：

| 方案 | ns/次 | vs 手写 |
|---|---|---|
| 手写直调 | 0.676 | 1.00× |
| `binder`（`bind`） | 1.127 | 1.67× |
| `binder`（`bind_at<I>`） | 1.081 | 1.60× |
| `std::function + tuple` | 2.257 | 3.34× |
| 旧库指针节点形状 | 4.204 | 6.22× |

每次重建 binder 再 bind+call：`binder` 1.243 ns（1.84×），`std::function+tuple` 6.996 ns
（10.4×），旧库形状 90.5 ns（134×，构造时每槽一次 `make_shared`）。
`binder` 多出来的 ~0.45 ns 是 3 次运行期槽位分派 + 存活位检查 + 槽位读写；
`bind_at<I>` 把分派也去掉后只省 ~0.05 ns（分派链很小且全预测中）。

3 段 series（`int -> double -> double`）：

| 方案 | tuple 段对象（参考） | 连续缓冲（默认直接取址） | 连续缓冲 + `META_BIND_STRICT_LAUNDER=1` |
|---|---|---|---|
| 手写链 | 0.676 | 0.676 | 0.676 |
| `_continue()` | 0.676 | 0.676 | 1.830 |
| `_bind_continue()` | 1.547 | 1.562 | 5.194 |
| `execute()`（thunk 表） | 0.676* | 4.6–5.4 | 5.4–5.5 |
| `drive()`（looper） | 1.567 | 1.554 | 5.167 |

从下标 1 起跑剩下 2 段（不是完整 3 段链，只比较“起点视图”）：

| 方案 | ns/次 |
|---|---|
| `series[1]._continue()`（运行期视图，thunk 表） | 3.39 |
| `series.at<1>()._continue()`（编译期视图，直接展开） | 0.676 |

运行期视图 vs `std::any` 的公平对照（同样 2 段、从 1 开始）：

| 方案 | ns/次 | 说明 |
|---|---|---|
| `series[1]._continue()`（参数已在栈里） | 3.57 | binder 自己管 arg/result |
| `std::any`：callable + arg + result 都存 any | 11.32 | 每次 any_cast callable 和 arg，再 any 存/取 result |
| `series[1]._bind_continue()`（返回自动喂下一段） | 3.83 | 含 binder 的类型检查/绑定 |
| `std::any`：callable + result 都存 any | 11.06 | 链式喂返回值 |
| `std::function` 链（只存 callable，不存 arg/result） | 3.39 | 仅作参考，不是等价工作量 |

也就是说：**运行期视图 + thunk 表仍比等价工作量的 `std::any` 路径快约 3×**；
`std::any` 每段要做 2 次 `any_cast`，还要把 arg/result 装箱，这正是偏移表 + thunk
想省掉的东西。`std::function` 那一行没有参数栈和返回值存储，只用来定位调用本身的下限。

\* tuple 版的 `execute()` 走的是 compare 链，固定参数下被常量折叠，这个数字不能当成
“compare 链比 thunk 表快”的证据；它只说明编译期可见的阶段选择可以被完全优化掉。
`execute()` 的 ~4.6–5.4 ns 是运行期下标必须付出的一次间接调用（thunk），只适合运行期游标；
`drive()` 的 `states.index()` 是 consteval，不走表。

连续缓冲的 binder 访问默认直接按 `stage_offsets[I]` 做 `reinterpret_cast`：对象是
placement new 构造的、类型在编译期已知，强转本身不是类型双关；需要严格走 P0137 的
`std::launder` 对象模型路径时定义 `META_BIND_STRICT_LAUNDER=1`。GCC 16 会把
`std::launder` 当成优化屏障（binder 状态无法再标量替换），strict 模式下 3 段链慢约 3×；
clang 23 两种模式都是 0.676 ns。`-fno-strict-aliasing`、把 `Binder*` 存进 pointer table
都不能恢复 GCC 的 strict 性能，这是 launder 的处理问题，不是偏移表本身的开销。

series 的额外开销主要是每段的存活位检查、返回值写入缓冲和段游标；函数体越重，这部分越摊薄
（WinAPI 例子里 `CreateWindowExA` 的真实开销远大于它）。
吞吐对比只用于同机同编译选项的相对判断，不外推到别的机器。

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
g++ -std=c++23 -O0 -I. -o binder examples/function_binder_smoke.cpp && ./binder
g++ -std=c++23 -O0 -I. -o series examples/function_series_smoke.cpp && ./series
g++ -std=c++23 -O0 -I. -o series_winapi examples/function_series_winapi.cpp && ./series_winapi
g++ -std=c++23 -O0 -I. -o window_series examples/window_series_smoke.cpp && ./window_series
g++ -std=c++23 -O0 -I. -o winapi_ui examples/winapi_ui_smoke.cpp && ./winapi_ui
g++ -std=c++23 -Wall -Wextra -pedantic-errors -I. -fsyntax-only examples/uint128_smoke.cpp examples/flags128_smoke.cpp examples/looper128_smoke.cpp

# 尺寸 / 吞吐对照
g++ -std=c++23 -O2 -falign-functions=64 -falign-loops=32 -I. \
    -o bench_binder bench/function_binder_bench.cpp && ./bench_binder
```

## 文档

- [meta_states_looper.md](meta_states_looper.md) —— 引擎设计说明与用法
