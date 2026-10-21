# meta_states_looper 教学文档

> 适用文件：`meta_states.hpp` / `meta_states_looper.hpp`
>
> **重要说明：这个库仍在开发中，公开 API 仍可能调整。**
> 建议你关注仓库更新，或直接查看最新的 `meta_states.hpp` 获取当前对外入口。
> 同时，`meta_iostream.hpp` 中也提供了一些常用的元对象流，可与 `meta_states` 配套使用。
> 本文示例使用 C++23；流控制组合入口在 `meta_ios.hpp`。

---

## 1. 这个库是做什么的

`meta_states_looper` 是一个**编译期状态循环器**。

它把一次循环拆成四个窗口（stage / window）：

1. `pre_gen_exec`：在生成器执行前检查状态
2. `post_gen_exec`：生成器执行后检查状态
3. `pre_rec_exec`：接收器执行前检查状态
4. `post_rec_exec`：接收器执行后检查状态

你可以把它理解为：

- `Generator` 负责“产生输入”
- `Receiver` 负责“消费输入并更新状态”
- `Condition / Executor` 负责在四个窗口里发号施令
- `looper` 负责调度、记账、计时器衰减、阶段推进

这套设计的重点是：

- **状态在编译期流动**
- **每一轮循环的行为可由 flags 精确控制**
- **用户只需要写规则，不需要自己写递归调度器**

职责边界必须分清：`meta_objects` 的调用协议不认识流，通用 looper 只按
flags 调度，不根据 payload 猜测类型列表或自动判断流结束。
即使 Generator 是有限流，直通条件也不会让它隐式停止；无界流更需要显式停止规则。
`meta_ios::stream_states_co<Fn>` 才是负责初始化边界与结束检测的流条件组合层。

---

## 2. 对外入口在哪

建议直接使用 `meta_states.hpp`，它是公开 API 门面。

### 主要命名空间

```cpp
namespace meta_states
```

### 常用别名

- `meta_states::u8 / u16 / u64 / u128`
- `meta_states::flags_operator`
- `meta_states::meta_empty_o`
- `meta_states::meta_object`
- `meta_states::meta_states_object`

### 主要入口类型

- `meta_states::make_condition<Executor>`
- `meta_states::run<Condition, Receiver, Generator>`
- `meta_states::looper<Condition, Receiver, Generator>`
- `meta_states::looper_t<Condition, Receiver, Generator, Args...>`
- `meta_states::pack_stream<Receiver, Generator, States>`

---

## 3. 最重要的概念：flags

`meta_states` 使用结构化类型 `meta_objects::u128` 作为 flags，含 `low`、`high`
两个 `uint64_t` 成员，可作为非类型模板参数；它不是编译器原生 `__int128`。
控制布局保持在低 64 位，高 64 位（bits 64..127）供用户存放状态。

### 3.1 控制位（低 64 位中的最高 8 位，即 bits 56..63）

这些位是 looper 识别的控制位：

- `G_CONTINUE`：总闸；本轮须在窗口①判定为进入，并且窗口④后的有效判定仍为真，才回调并继续
- `G_NOT_SKIP`：在窗口③采样，决定本轮是否允许回调；不能指望窗口④修改它就改变本轮回调
- `G_ADVANCE`：生成器要不要推进
- `G_RECEIVE`：接收器要不要接收生成器输出
- `G_FINISH`：本轮到此为止，并在结转时清掉
- `G_FORWARD`：方向是否保持 `Generator -> Receiver`
- `G_PACK`：回调看到打包视图 `pack_stream`
- `G_FEED_GEN`：未打包时，回调看到生成器产出

其中：

- `G_PACK` 优先于 `G_FEED_GEN`
- `G_FINISH` 是一次性的，结转时会被消费掉
- 计时器只会压制 looper 识别的位，不会破坏用户原始 flags

窗口①的条件本身仍会被调用，即使它随后关闭 `G_CONTINUE`。
关总闸不等同于跳过后续所有条件窗口；要截断本轮窗口链，应使用 `G_FINISH`。

### 3.2 flags 布局

`meta_states.hpp` 已经把布局导出给你：

- `position_shift = 0`：bits 0..15，`u16`，轮尾递增的 position
- `user_shift = 16`：bits 16..23，`u8`，高 4 位阶段镜像，低 4 位可由上层协议使用
- `timer0_shift = 24`：bits 24..39，`u16`，计时器槽 0
- `timer1_shift = 40`：bits 40..55，`u16`，计时器槽 1
- `gates_shift = 56`：bits 56..63，`u8`，控制位字节

控制位从 bit 63 到 bit 56 依次为 `G_CONTINUE`、`G_NOT_SKIP`、`G_FEED_GEN`、
`G_PACK`、`G_FORWARD`、`G_FINISH`、`G_RECEIVE`、`G_ADVANCE`。
四个阶段值为 `0x10 / 0x20 / 0x40 / 0x80`，不是 `0 / 1 / 2 / 3`。
同一轮各执行窗口看到相同 position；正常轮尾或提前 FINISH 时只记账一次。
position 是 `u16` 字段，溢出会在字段内回绕，并非无限精度计数器。

通用 looper 保留 user 字段低 4 位，但 `meta_ios::stream_states_co` 会占用它们，
并使用 timer0 处理首轮抑制；使用该包装器时，这两处不要自行复用。

以及对应掩码：

- `position_mask`
- `user_mask`
- `timer0_mask`
- `timer1_mask`
- `gates_mask`

---

## 4. 默认条件种子

`meta_states::condition_seed` 是默认条件初始 flags。

它包含：

- `G_CONTINUE`
- `G_NOT_SKIP`
- `G_PACK`
- `G_FORWARD`
- `G_RECEIVE`
- `G_ADVANCE`

并且会把阶段镜像写成第一窗口：`pre_gen_exec`。

如果你直接用 `make_condition<Executor>`，默认就从这里开始。

---

## 5. 如何写一个 Executor

Executor 的职责是：

- 在四个窗口里，根据当前 `flags` 和当前对象，返回新的控制 flags

### 5.1 直接提供 apply

当前协议是 `apply<Stage, Flags, Object>`，不是四个具名成员。
`Stage` 是 `std::integral_constant<meta_states::u8, 窗口号>`；
`Flags` 是当前 flags 类型，`Object` 是该窗口观察的对象。
下面是可独立编译的直通执行器示例：

```cpp
#include "meta_states.hpp"

struct passthrough_exec {
    template <class Stage, class Flags, class Object>
    using apply = Flags;
};

using passthrough_condition = meta_states::make_condition<passthrough_exec>;
using passthrough_result = meta_states::state_fn<passthrough_exec>::apply<
    meta_states::flags_operator<meta_states::condition_seed>,
    meta_states::meta_empty_o>;
static_assert(passthrough_result::value == meta_states::condition_seed);

int main() {}
```

### 5.2 按窗口号选择模板

也可以提供 `template <auto Stage> struct Fn`，再用
`meta_objects::states_executor<Fn>` 适配（当前未导出到 `meta_states`）。未定制的窗口可继承
`meta_states::ret_origin<Stage>`，其 `apply<Flags, Object>` 原样返回 flags。
下面是可独立编译的适配示例；第 13 节给出完整的有限流运行示例。

```cpp
#include "meta_states.hpp"

template <auto Stage>
struct window_exec : meta_states::ret_origin<Stage> {};

template <>
struct window_exec<meta_states::pre_rec_exec> {
    template <class Flags, class Object>
    using apply = typename Flags::template opSet<meta_states::G_FINISH>;
};

using executor = meta_objects::states_executor<window_exec>;
using condition = meta_states::make_condition<executor>;
using result = meta_invoke<executor,
    std::integral_constant<meta_states::u8, meta_states::pre_rec_exec>,
    meta_states::flags_operator<meta_states::condition_seed>,
    meta_states::meta_empty_o>;
static_assert(result::template test<meta_states::G_FINISH>);

int main() {}
```

### 5.3 阶段分派怎么发生

`state_fn<Executor>` 从 `Flags` 的 user 字段高半字节读取窗口号，调用
`meta_invoke<Executor, std::integral_constant<u8, Stage>, Flags, Object>`。
返回结果必须有 `::value`；返回完整 flags 时应使用 `auto` 或 `u128`，
不能缩窄成 `u64`，否则会丢失高 64 位用户状态。

---

## 6. 如何创建条件对象

使用 `make_condition`：

```cpp
using cond = meta_states::make_condition<my_exec>;
```

如果你想自定义初始 flags，也可以传第二个模板参数：

```cpp
using cond = meta_states::make_condition<my_exec, some_flags>;
```

第二个参数的类型是 `u128`；`u64` 常量（包括默认的 `condition_seed`）可以隐式转换。
例如 `meta_states::u128{meta_states::condition_seed, 7}` 将低半部设为种子，
高半部设为用户状态 7。组合完整 flags 时不要先缩窄成 `u64`，
清位优先用 `opDeact<Mask>`；直接取反掩码时先扩为 `u128`，避免误清高半部。

---

## 7. Receiver 和 Generator 是什么

### 7.1 Receiver

Receiver 是“状态接收器”。

它接收：

- 当前状态
- 生成器给出的输入（如果 `G_RECEIVE` 允许）

并返回新的状态对象。

### 7.2 Generator

Generator 是“状态生产器”。

它每轮可以产生：

- 新的输入给 Receiver
- 或者被跳过

如果你不需要 generator，可以直接用默认值：

```cpp
meta_states::meta_empty_o
```

### 7.3 条件观察与普通对象调用不是一回事

`meta_object_invoke<MSO, 非MSO对象>` 把**整个原始对象**交给条件，
包括尚未初始化、没有 `::type` 的对象，不会先自动初始化它。
因此条件不能无条件访问 `Object::type`，应先判断
`meta_objects::initialized_meta_object_t<Object>` 或
`meta_objects::uninitialized_meta_object_t<Object>`。
两个 MSO 相互调用时，传入的是被观察者的 `::flags`，而非整个 MSO。

普通非 MSO 调用方的对象调用协议仍保留自动初始化：被调用对象若可无参初始化，
先初始化再解包；否则返回等待初始化参数的元函数。Generator 的首次普通调用也可
建立初始 payload。这些规则不是“所有观察都自动初始化”。

---

## 8. run / looper / looper_t 的区别

### 8.1 `looper`

```cpp
template <class Condition, class Receiver, class Generator = meta_empty_o>
using looper = ...;
```

这是循环引擎本体。你通常不直接操作它的内部细节。

### 8.2 `run`

```cpp
template <class Condition, class Receiver, class Generator = meta_empty_o,
          class... Args>
using run = ...;
```

这是推荐入口。

你通常会写：

```cpp
meta_states::run<cond, recv, gen>::for_each(...)
```

### 8.3 `looper_t`

```cpp
template <class Condition, class Receiver, class Generator, class... Args>
using looper_t = ...;
```

它会直接给出本次运行结束后的最终类型。

适合做纯类型推导。

---

## 9. 回调接口：for_each 和 for_each_forward

循环本身是元编程，但它也提供“逐步回调”的接口。

### 9.1 `for_each`

```cpp
run<cond, recv, gen>::for_each(f, args...)
```

只在被观察的轮次调用 `f`，不是每个窗口调用一次，也不是每轮必调。
观察要求窗口①进入、窗口③有效的 `G_CONTINUE | G_NOT_SKIP` 都置位，
以及窗口④后的有效 `G_CONTINUE` 仍置位。

`f` 的第一个参数一般是当前状态类型对象，后面跟着你传入的额外参数。

### 9.2 `for_each_forward`

```cpp
run<cond, recv, gen>::for_each_forward(f, first, rest...)
```

它按轮次消费位置参数：本轮回调使用 `first`，下一轮使用下一个参数。
**跳过回调的轮次也消耗一个参数**，不会把该参数留给下一个命中轮。
参数耗尽会结束运行期回调展开，但不会替无界的编译期递归建立停止条件。

适合“输入序列逐个推进”的场景。

两个接口都用 `decltype(auto)` 返回最后一个**实际执行的回调**的结果，
此前回调结果被丢弃，因此可以保留最后结果的引用类型。
没有回调，或最后回调返回 `void` 时，整体返回 `void`。
`for_each_forward` 的“最后”还受实参数量限制。

---

## 10. G_PACK 与 meta_pack_stream

当 `G_PACK` 置位时，回调看到的不是单一对象，而是一个“打包视图”：

```cpp
meta_states::pack_stream<Receiver, Generator, States>
```

它提供：

- `from`：Generator 类型
- `to`：Receiver 类型
- `from_t / to_t`：`get_type<from / to>`；有 `::type` 就取 payload，否则为 `no_exist_type`，不是 ret 投影
- `cache`：`meta_objects::meta_produce_t<Generator>`，按对象协议取生成器产物
- `states`：结转后的 flags 类型（计时器已 tick，`G_FINISH` 已清除）
- `index()`：结转后 position 减 1，通常是本轮编号；受 `u16` position 回绕限制
- `value()`：接收器的 `meta_produce_t<to>` 的值，仅在该产物支持取值时适用
- `left()`：`exp_size<from>`，按 payload 的长度协议计算，不是强行把 payload 当列表
- `object()`：默认构造 `to_t`，不是取出保存的运行期实例，要求该类型可默认构造
- `target_type()`：`typeid(meta_produce_t<to>)` 的 `const std::type_info&`，接口为 `consteval`，不承诺可用于常量求值 RTTI 操作

视图由 Receiver、Generator 和 flags 现算，不持有运行期状态。
它也用于循环终态 `::type`；终态可能包含最后一次未观察的停止检查，
不要把终态视图总当成“最后一次回调参数”。

关闭 `G_PACK` 时，若 `G_FEED_GEN` 置位则选 `meta_produce_t<Generator>`；
否则当前实现选 `get_type<Receiver>`（已初始化时即 `Receiver::type`），
**不是**接收器的 `ret` 投影。这是实际实现行为，不应被注释中的“接收器产出”误导。
产物选择使用结转后的请求 flags，而非计时器压制后的 effective flags；
因此不要依赖计时器压制 `G_PACK / G_FEED_GEN` 来切换产物类型。

### 什么时候用

当你希望回调同时知道：

- 当前接收器是谁
- 当前生成器是谁
- 当前状态 flags 是什么

就可以开启 `G_PACK`。

---

## 11. 计时器机制

`meta_states` 提供两个计时器槽：

- `timer0`
- `timer1`

它们的目标是：

- 在若干轮内压制某个控制位
- 到期后解除压制；能否恢复为开，取决于请求 flags 中该位是否仍置位

### 11.1 计时器格式

每个槽存的是：

- 计数 + 目标位索引

具体布局由：

- `timer_count_shift = 6`
- `timer_target_mask = 63`

共同决定。

每槽是 16 位，低 6 位存目标位下标，高 10 位存计数，`timer_max_count = 1023`。
合法 Count 为 `1..1023`；清槽使用 `clear_timer`，不能用 Count 为 0 装表。
`timer_value` 是整个编码字段，不等于剩余轮数；剩余轮数用 `timer_count`。
空槽应通过 count 或 value 判断；空槽的 `timer_target` 本身没有有效目标含义。

### 11.2 相关 API

- `set_suppression_timer<Flags, Slot, Target, Count>`
- `override_suppression_timer<Flags, Slot, Target, Count>`
- `clear_timer<Flags, Slot>`
- `tick_timers<Flags>`
- `timer_value<Flags, Slot>`
- `timer_count<Flags, Slot>`
- `timer_target<Flags, Slot>`
- `effective<Flags>`

### 11.3 区别

- `set_suppression_timer`：如果槽位正在计时，不覆盖
- `override_suppression_timer`：强制覆盖
- `clear_timer`：清空某槽
- `tick_timers`：让两个槽都递减一次
- `effective`：计算“被计时器压制后的有效 flags”

请求 flags 原样保留，looper 在门控判定时用 `effective<Flags>` 暂时清掉目标位。
所以用户在计时期间再次置位，并不能绕过尚未到期的压制。
每轮结束 tick 一次，包括任意窗口 `G_FINISH` 的提前结转；不按窗口重复递减。
Count 为 1 且在轮首装表时覆盖当前整轮，到轮尾清槽；若中途装表，则只覆盖
从装表处到当前轮尾的判定，不会追溯已经发生的动作。
窗口④或 FINISH 的结转先 tick，随后的有效判定看到的是 tick 后计数，
所以不要用“整轮压制”推断它还能控制轮尾之后的所有判定。

### 11.4 约束

`Target` 必须是 `meta_states` 识别的控制位之一，也必须是单 bit。

也就是：

```cpp
looper_controlled_bit<Target>
```

必须成立。

---

## 12. 为什么有 G_FINISH

`G_FINISH` 的意思不是“循环停止”，而是：

- 本轮到此为止
- 后续窗口不要再执行
- 这轮结转时要消费掉它

它和 `G_CONTINUE` 不是一回事：

- `G_CONTINUE`：是否进入下一轮
- `G_FINISH`：当前轮是否提前收尾

因此你可以让一轮在中途结束，但外层循环还可以继续。

具体截断点是：

- 窗口①置位：不推进 Generator，也不接收，跳过窗口②③④。
- 窗口②置位：Generator 已执行，不接收，跳过窗口③④。
- 窗口③置位：在总闸、接收门等条件满足时，**自己的接收仍执行**，只跳过窗口④。
- 窗口④置位：本来就到轮尾，仍只记账一次。

实际进入的结束窗口负责 position 加 1、tick 和阶段复位；被截断的窗口不再重复结转。
`G_FINISH` 本身不是跳过回调的门，是否回调仍取决于观察条件。

---

## 13. 可运行的有限流示例

`meta_ios::stream_states_co<Fn>` 是公开的流条件组合入口。
省略 Fn 即 `stream_states_co<>`，默认用户执行器只做直通，
**基本流控制仍执行**，不是取消结束检测。
每个窗口先执行基本控制，仅当它保留 `G_CONTINUE` 时才调用用户 Fn。
Fn 的协议与第 5 节一致，也可传入 `meta_objects::states_executor<窗口模板>`。

包装器在窗口①识别没有 Generator 的情况；对于首轮已初始化的 Generator，
借 timer0 抑制一次推进，避免丢掉初始产物。窗口②检查生成器产物是否为结束哨兵。
无 Generator 时，窗口③处理 Receiver 的初始化能力与首轮接收抑制，
窗口④检查接收器产物的结束哨兵；需要输入却没有输入的未初始化 Receiver 会停止。
这些流知识都属于包装器，而非通用 looper。

以下程序消费 `char, double, int`，但只在第二轮执行一次回调。
编译期与运行期都检查回调次数，回调类型也由 `static_assert` 核实：

```cpp
#include "meta_ios.hpp"
#include <cassert>
#include <type_traits>

struct select_second {
    template <class Window, class Flags, class Object>
    using apply = typename Flags::template select<
        meta_states::user_fn::index_of<Flags>() == 1,
        meta_states::G_NOT_SKIP, 0>;
};

using selected = meta_states::run<
    meta_ios::stream_states_co<select_second>,
    meta_iostream::meta_iterator,
    meta_iostream::meta_istream_list<char, double, int>>;

constexpr bool check_callbacks() {
    unsigned calls = 0;
    auto last = selected::for_each([&]<class Stream>(Stream stream) {
        static_assert(stream.index() == 1);
        static_assert(std::is_same_v<
            meta_objects::meta_produce_t<typename Stream::to>, double>);
        ++calls;
        return stream.index();
    });
    return calls == 1 && last == 1;
}

static_assert(check_callbacks());

int main() {
    assert(check_callbacks());
}
```

`G_NOT_SKIP` 只控制回调，不妨碍其他轮的生成与接收。
如果换成 `meta_index_istream` 或 `meta_repeat_istream` 等无界流，
必须在用户 Fn 中显式关闭 `G_CONTINUE`，包装器不会凭空推断上限。

---

## 14. 进阶：手工拼 flags

如果你已经理解布局，也可以直接操作 `flags_operator`：

```cpp
#include "meta_states.hpp"

using namespace meta_states;

using f0 = flags_operator<u128{condition_seed, 7}>;
using tagged = f0::field_or<u8, user_shift, 3>;
using cleared_stage = tagged::field_and<u8, user_shift, 0x0F>;
using f1 = cleared_stage::field_or<u8, user_shift, pre_rec_exec>;
using f2 = f1::opSet<G_FINISH>;

static_assert(f2::get<u8, user_shift> == (pre_rec_exec | 3));
static_assert(f2::get<u64, 64> == 7);
static_assert(f2::test<G_FINISH>);
static_assert(f2::get<u16, position_shift> == 0);

int main() {}
```

通常阶段由 looper 维护，不需要手工推进。上例仅演示如何保留低半字节后
替换阶段镜像，并且保留高 64 位。`set<u64, 0, G_FINISH>` 会覆盖整个低 64 位，
不是“仅置 FINISH 位”；应使用 `opSet<G_FINISH>`。

常见操作有：

- `set<T, shift, value>`
- `deact<T, shift>`
- `inc<T, shift>`
- `dec<T, shift>`
- `field_or / field_and / field_xor`
- `opSet / opDeact / opToggle`

---

## 15. `meta_iostream.hpp` 中的常用元对象流

如果你在写 `meta_states` 的条件、接收器或生成器，`meta_iostream.hpp` 里有一些常用流可以直接拿来用。

### 15.1 读取流

- `meta_istream<TL>`：从类型列表前往后读
- `meta_ristream<TL>`：名字意在逆序读取，但当前实现复用了 `list_dec`（pop_front），ret 却取 `pop_last`；不能当成正确的逐元素逆序流使用
- `meta_transform_istream<TL, F>`：读取时先对 `front` 做变换
- `meta_index_istream<start>`：永不结束，持续吐递增下标
- `index_sequence_istream<Len>`：吐 `0..Len-1`，到头结束
- `meta_repeat_istream<T>`：永不结束，重复吐同一个类型

这些名称位于 `meta_iostream` 命名空间。有限性不是 looper 的内置概念：
使用 `meta_ios::stream_states_co<>` 或自定义条件才能在结束处停止。

`end_of_stream<End, EndType>` 由 payload 声明结束状态。
`end_of_ret<RetFn>` 在 payload 满足该协议且 `end == true` 时返回 `end_type`，
否则才调用 RetFn；这让流在结束处返回哨兵，而不是继续做无效的元素投影。
流包装器通过 `exp_utilities::eol<meta_objects::meta_produce_t<Object>>` 检查产物，
因此自定义 `EndType` 也必须满足这个结束判定，不能仅换任意类型便期待自动停止。
`meta_uninitialized` 表示尚无 payload，**不是结束哨兵**。

### 15.2 写入流

- `meta_ostream<TL>`：普通追加型输出流
- `meta_jostream<TL>`：如果输入本身是类型列表，则自动摊平合并
- `meta_rostream<TL>`：前插型输出流，结果顺序会反转
- `meta_filter_ostream<TL, pred>`：满足谓词才写入
- `meta_transform_ostream<TL, F>`：写入前先做变换
- `meta_forward_ostream<TL, F>`：按位置映射输入
- `meta_iterator`：可替换状态的 iterator 风格 ostream
- `meta_transform_iterator<F, init>`：带初始化能力的 iterator 风格对象
- `meta_aligned_iterator`：把类型映射到运行期对齐地址偏移

### 15.3 这些流和 meta_states 的关系

它们满足普通元对象协议，可作为 `meta_states::run`、`meta_states::looper` 的对象。
但协议兼容不等于自动具备流结束控制；推荐以 `meta_ios::stream_states_co<Fn>` 组合。
payload 可以是计数器等非列表状态；通用层不应假设存在 `front / pop_front`。

如果你的状态逻辑需要：

- 逐个消费类型
- 构造/拼接类型列表
- 过滤、映射、折叠
- 生成有界或无界序列

这些流会非常常用。

---

## 16. 常见坑

### 16.1 不要把用户自定义位写进 gates 字节里

gates 字节是 looper 的控制区。

如果你需要自己的状态，请放在：

- 高 64 位（bits 64..127），例如 `Flags::set<u64, 64, Value>`
- 自己管理的 payload 中
- 仅用通用 looper 时，可约定使用 `user_shift` 的低 4 位；流包装器已保留此处与 timer0

### 16.2 计时器目标只能是单 bit

不要把多个控制位混在一个 timer target 里。

### 16.3 `G_FINISH` 是一次性的

它会在结转时清除，不要拿它当长期状态位。

### 16.4 `G_PACK` 会改变回调看到的类型

如果你开启了 `G_PACK`，回调参数就不再是单纯的 Receiver / Generator 产物，而是打包视图。

### 16.5 注意版本更新

这个库仍在开发中，`meta_states.hpp` 才是最适合关注的对外入口；如果你依赖较多内部细节，建议定期对照最新头文件。

---

## 17. 速查表

### 入口

- `meta_states::make_condition<Executor>`
- `meta_states::run<Condition, Receiver, Generator>`
- `meta_states::looper_t<Condition, Receiver, Generator, Args...>`
- `meta_states::pack_stream<Receiver, Generator, States>`

### 控制位

- `G_CONTINUE`
- `G_NOT_SKIP`
- `G_ADVANCE`
- `G_RECEIVE`
- `G_FINISH`
- `G_FORWARD`
- `G_PACK`
- `G_FEED_GEN`

### 阶段

- `pre_gen_exec`
- `post_gen_exec`
- `pre_rec_exec`
- `post_rec_exec`

### 计时器

- `set_suppression_timer`
- `override_suppression_timer`
- `clear_timer`
- `tick_timers`
- `effective`

### 常用元对象流

- `meta_istream`
- `meta_ristream`
- `meta_transform_istream`
- `meta_ostream`
- `meta_jostream`
- `meta_rostream`
- `meta_filter_ostream`
- `meta_forward_ostream`
- `meta_index_istream`
- `index_sequence_istream`
- `meta_repeat_istream`
- `meta_iterator`
- `meta_transform_iterator`
- `meta_aligned_iterator`

---

## 18. meta_pipe（编译期管道）

`meta_ios::meta_pipe` 自己就是 looper：状态是扁平 slot，每轮做一次 fold。
中间段不再是“装着流的 meta object”，每段是一个 `(rec, con)` 节点：

```
meta_pipe_impl<main, recN, conN, ..., rec0, con0, gen>
```

```cpp
#include "meta_ios.hpp"
using namespace meta_iostream;

using offsets = meta_ios::meta_pipe<meta_istream_list<int, double, char>>
                    ::all_to<meta_aligned_iterator>
                    ::run<meta_ostream<>>
                    ::type::to_t;
```

- `all_to<Rec>`：追加一段；默认 con 是 `stream_states_co` 的 pipe 版本
  （节点 0 的 `G_ADVANCE` 开，其余关）。
- `all_to<Rec, con<C>>`：显式指定这段的 con（con 置后）。
- `run<Rec>` / `run<Rec, con<C>, main_con<M>>`：最后一段 + 全局 main；
  `::type` 是 `pipe_view`（`to` / `to_t` / `from` / `from_t` / `states` / `index()`）。
- `meta_pipe<>`：没有 gen，默认 `meta_empty_o`。

实现上的关键点：

- 每轮的 fold 从 gen 往输出方向走：节点 0 推进 gen，其余节点的输入是上一个
  节点的 `rec'`（它已经是 generator），所以中间节点的 `G_ADVANCE` 默认关。
- looper 的单步逻辑是 `meta_states_looper_step`（四窗口，不递归）；pipe 保留独立
  的节点单步实现，采用相同窗口语义，并增加 fold / emit / flush 规则。
- 每段 con 的高 64 位有 `PIPE_EMIT`（bit 64）和 `PIPE_FLUSH`（bit 65）：
  filter 用 EMIT 跳过元素；`wait_for_end_con<>` 活着只收不发，死亡那轮按 FLUSH
  冲刷最终累积值。`main` 的高 64 位是节点存活位图。
- 非输出最末节点死亡 → 本轮传完最终结果后 pipe 停；输出最末节点死亡 → 清位、
  以后不再调用，`main_con` 按位图决定是否继续。filter 不死亡，只 skip。
- 一个 `meta_states_object` 可以放在 gen/rec 位（它的 flags 就是产出）；
  用 MSO 当 rec 位的 `type_mask` 例子见 `type_safe_aligned_iterator.hpp`。

`type_safe_aligned_iterator.hpp` 是管道的实际使用者：对齐偏移用
`meta_pipe<...>::all_to<meta_aligned_iterator>::run<meta_ostream<>>` 收集，
总大小用一次 `run` 得到，类型安全位图用 MSO 当 rec 位跑一次流得到。

---

## 19. 分块续跑（chunk driver）

轮次不再是一条递归链跑到底：looper 每段最多 `META_STATES_LOOPER_BUDGET` 轮
（默认 64），pipe 使用独立的自适应预算。到预算就把状态交给 driver 续下一段。

预算就是“每段轮数”：driver 的起始 `Remaining` 是 `B-1`，chunk 的 `Remaining`
递减到 0（含第 0 轮）正好 B 轮。pipe 的 `pipe_chunk_budget` 同理。

- looper 的状态是 `(Condition, Receiver, Generator)`；
- pipe 的状态是 `(Main, Nodes, Gen)`；
- 一段的结果是 `done<feed/view>` 或 `more<state>`；driver 展开 `more` 直到 `done`。

```text
depth ≈ O(Budget + Rounds / Budget)
```

默认 900 深度下，`index_sequence_istream<2000>` 的 looper/pipe 的 `::type` 与
`for_each` 都能通过（`examples/deep_smoke.cpp`）。pipe 的预算按节点数自适应
（`clamp(256 / (节点数 + 4), 1, 64)`），因为每轮还有一层 fold。

两个实现细节：

- `for_each` 的段边界不在 chunk 内调用下一段，而是由 driver 顺序调用；
  这样函数体的实例化链也在段边界处断开，而不是只在类型层断开。
- `observes` / `observes_within_fn` 是惰性 any：当前状态已有回调时不再实例化
  后面的 `Rest::observes`，探测停在第一个回调状态；执行仍走到最后一个回调。
- `for_each_forward` 用“剩余参数数量”参与判断未来是否还有回调
  （`observes_within_fn<N>`），避免把已经执行的回调返回值丢掉；这个判断也跨段。

限制：driver 仍有一层 `Rounds/Budget` 的递归；runtime 的 `for_each` 仍是一轮一层
调用；真正无限长需要 `-ftemplate-depth` 或非标准的状态化元编程。looper 的预算可用
`-DMETA_STATES_LOOPER_BUDGET=...` 覆盖，不影响 pipe。

---

## 20. skip / yield / stop 决定

窗口函数（`stream_states_co` / `node_con` 里的用户 Fn）除了返回 flags，还可以返回
三个标记：

| 标记 | 效果 |
|---|---|
| `skip_t` / `skip()` | 清 `G_RECEIVE`、`G_NOT_SKIP`（pipe 还清 `PIPE_EMIT`），**保留 `G_CONTINUE`** |
| `yield_t` / `yield()` | 置 `G_RECEIVE`、`G_NOT_SKIP`（pipe 还置 `PIPE_EMIT`），保留 `G_CONTINUE` |
| `stop_t` / `stop()` | 清 `G_CONTINUE`，结束循环 |

归一化发生在 `stream_fn` 里：以 `controlled_flags` 为底做 `opDeact` / `opSet`，
所以 skip/yield 只是改门位，不会把“结束”和“跳过”混为一谈。TMP 的 `apply` 是类型
别名，写 `using apply = yield_t;`；`skip()` / `yield()` / `stop()` 是 constexpr
函数形式，方便 `decltype(...)`。例子见 `examples/decision_smoke.cpp`。

---

## 21. 一句话总结

`meta_states_looper` 是一个“由 flags 驱动的编译期四窗口循环器”，你通过 `Executor` 定义每个窗口的规则，通过 `Receiver / Generator` 定义数据流动，通过 `meta_states.hpp` 暴露的 API 完成可组合、可暂停、可计时、可打包的状态循环。
