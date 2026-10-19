# meta_looper

C++23 纯头文件的**编译期循环 / 状态机**框架：一轮循环由"元生成 + 元接收"驱动，用四个监控窗口
（`pre_gen_exec` / `post_gen_exec` / `pre_rec_exec` / `post_rec_exec`）把用户的条件函数嵌进去，
阶段推进、index 记账、计时器递减与压制全部由 looper 自己的 flags 完成。

## 文件

| 文件 | 作用 |
|---|---|
| `meta_states_looper.hpp` | 引擎：门位（gates）、计时器、四个监控窗口、阶段推进与轮尾记账 |
| `meta_states.hpp` | 公开层：控制位 / 窗口号 / 字段布局 / 计时器 API、`state_fn`、`make_condition`、`run` |
| `meta_objects.hpp` | 元对象协议：`meta_object` / `meta_states_object` / `meta_ret_object` / `meta_empty_o` / `flags_operator` |
| `meta_uint128.hpp` | 通用 128 位结构化整数：位运算、移位、模加减；不包含 looper 布局 |
| `meta_invoke_protocols.hpp` | `meta_invoke` / `invoke_if` 等调用协议 |
| `meta_utilities.hpp` | `exp_list`、`exp_size`、`length_*`、`value_is` 等基础件 |

流（`meta_iostream` / `meta_ios` / `meta_stream`）不在这个仓库里，留在 meta_stream 项目。

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
- 计时器：`set_timer` / `tick_timers` / `effective` —— 压制只影响 looper 的**判定**，不改用户的 flags，
  计时归零后自动恢复
- 字段布局：`position`(u16, 轮尾 +1) / `user`(u8) / `timer0`(u16) / `timer1`(u16) / `gates`(u8)

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
g++ -std=c++23 -Wall -Wextra -pedantic-errors -I. -fsyntax-only examples/uint128_smoke.cpp examples/flags128_smoke.cpp examples/looper128_smoke.cpp
```

## 文档

- [meta_states_looper.md](meta_states_looper.md) —— 引擎设计说明与用法
