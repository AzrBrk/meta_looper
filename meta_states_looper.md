# meta_states_looper 教学文档

> 适用文件：`meta_states.hpp` / `meta_states_looper.hpp`
>
> **重要说明：这个库仍在开发中，公开 API 仍可能调整。**
> 建议你关注仓库更新，或直接查看最新的 `meta_states.hpp` 获取当前对外入口。
> 同时，`meta_iostream.hpp` 中也提供了一些常用的元对象流，可与 `meta_states` 配套使用。

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

---

## 2. 对外入口在哪

建议直接使用 `meta_states.hpp`，它是公开 API 门面。

### 主要命名空间

```cpp
namespace meta_states
```

### 常用别名

- `meta_states::u8 / u16 / u64`
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

`meta_states` 使用 128 位 flags。原有控制布局保持在低 64 位，高 64 位供用户存放状态。

### 3.1 控制位（低 64 位中的最高 8 位，即 bits 56..63）

这些位是 looper 识别的控制位：

- `G_CONTINUE`：循环是否继续
- `G_NOT_SKIP`：这一步要不要回调用户函数
- `G_ADVANCE`：生成器要不要推进
- `G_RECEIVE`：接收器要不要接收生成器输出
- `G_FINISH`：本轮到此为止，并在结转时清掉
- `G_FORWARD`：方向是否保持 `Generator -> Receiver`
- `G_PACK`：回调看到打包视图 `pack_stream`
- `G_FEED_GEN`：回调看到生成器产出

其中：

- `G_PACK` 优先于 `G_FEED_GEN`
- `G_FINISH` 是一次性的，结转时会被消费掉
- 计时器只会压制 looper 识别的位，不会破坏用户原始 flags

### 3.2 flags 布局

`meta_states.hpp` 已经把布局导出给你：

- `position_shift`：`u16`，记步位置
- `user_shift`：`u8`，用户字段（高 4 位是阶段镜像，低 4 位留给用户）
- `timer0_shift`：`u16`，计时器槽 0
- `timer1_shift`：`u16`，计时器槽 1
- `gates_shift`：`u8`，控制位字节

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

### 5.1 最简单的写法

你可以继承 `states_base`：

```cpp
struct my_exec : meta_states::states_base {
    template <class this_flags, class obj>
    struct pre_gen {
        static constexpr meta_states::u64 value = /* ... */;
    };
};
```

`states_base` 会为你补齐其余三个窗口：

- `post_gen`
- `pre_rec`
- `post_rec`

默认行为都是“原样返回 flags”。

### 5.2 executor 的四个成员

分别对应：

- `pre_gen<this_flags, obj>`
- `post_gen<this_flags, obj>`
- `pre_rec<this_flags, obj>`
- `post_rec<this_flags, obj>`

每个成员都应该提供：

```cpp
static constexpr meta_states::u64 value = ...;
```

也就是说，它可以是：

- 一个带 `::value` 的结构体
- 或者一个返回可计算 flags 的表达式封装

### 5.3 阶段分派怎么发生

`state_fn<Executor>` 会读取 `this_flags` 里的阶段镜像，并把调用分派到对应成员。

所以你的 executor 不需要自己判断当前在哪个窗口，框架已经处理好了。

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

其中 `some_flags` 应该是一个 `u64` 常量。

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

每一步都会把当前 stage 产物传给 `f`。

`f` 的第一个参数一般是当前状态类型对象，后面跟着你传入的额外参数。

### 9.2 `for_each_forward`

```cpp
run<cond, recv, gen>::for_each_forward(f, first, rest...)
```

它会把第一个参数逐步向后传递，每一轮消费一个参数。

适合“输入序列逐个推进”的场景。

---

## 10. G_PACK 与 meta_pack_stream

当 `G_PACK` 置位时，回调看到的不是单一对象，而是一个“打包视图”：

```cpp
meta_states::pack_stream<Receiver, Generator, States>
```

它提供：

- `from`：Generator 类型
- `to`：Receiver 类型
- `from_t`
- `to_t`
- `cache`
- `states`
- `index()`
- `value()`
- `left()`
- `object()`

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
- 到期后自动恢复

### 11.1 计时器格式

每个槽存的是：

- 计数 + 目标位索引

具体布局由：

- `timer_count_shift = 6`
- `timer_target_mask = 63`

共同决定。

### 11.2 相关 API

- `set_timer<Flags, Slot, Target, Count>`
- `override_timer<Flags, Slot, Target, Count>`
- `clear_timer<Flags, Slot>`
- `tick_timers<Flags>`
- `timer_value<Flags, Slot>`
- `timer_count<Flags, Slot>`
- `timer_target<Flags, Slot>`
- `effective<Flags>`

### 11.3 区别

- `set_timer`：如果槽位正在计时，不覆盖
- `override_timer`：强制覆盖
- `clear_timer`：清空某槽
- `tick_timers`：让两个槽都递减一次
- `effective`：计算“被计时器压制后的有效 flags”

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

---

## 13. 一个推荐的使用模板

下面是一个更接近实际使用的写法。

```cpp
#include "meta_states.hpp"

using namespace meta_states;

struct my_exec : states_base {
    template <class this_flags, class obj>
    struct pre_gen {
        static constexpr auto value = this_flags::value | G_ADVANCE;
    };

    template <class this_flags, class obj>
    struct pre_rec {
        static constexpr auto value = this_flags::value | G_RECEIVE;
    };
};

using cond = make_condition<my_exec>;
```

然后你可以把它交给 `run`。

---

## 14. 进阶：手工拼 flags

如果你已经理解布局，也可以直接操作 `flags_operator`：

```cpp
using f0 = flags_operator<condition_seed>;
using f1 = typename f0::template set<u8, user_shift, pre_rec_exec>;
using f2 = typename f1::template set<u64, 0, G_FINISH>;
```

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
- `meta_ristream<TL>`：从后往前读
- `meta_transform_istream<TL, F>`：读取时先对 `front` 做变换
- `meta_index_istream<start>`：永不结束，持续吐递增下标
- `index_sequence_istream<Len>`：吐 `0..Len-1`，到头结束
- `meta_repeat_istream<T>`：永不结束，重复吐同一个类型

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

它们都是普通元对象，所以可以直接和 `meta_states::run`、`meta_states::looper` 配合。

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

- `user_shift` 的低 4 位
- 或者别的你自己管理的 payload 中

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

- `set_timer`
- `override_timer`
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

## 18. 一句话总结

`meta_states_looper` 是一个“由 flags 驱动的编译期四窗口循环器”，你通过 `Executor` 定义每个窗口的规则，通过 `Receiver / Generator` 定义数据流动，通过 `meta_states.hpp` 暴露的 API 完成可组合、可暂停、可计时、可打包的状态循环。