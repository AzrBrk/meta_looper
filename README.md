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
  struct apply { static constexpr u64 value = flags::value; };
};

using cond = make_condition<states_executor<my_exec>>;
// run<cond, Receiver[, Generator]>::for_each([](auto stream){ ... });
```

- 门位：`G_CONTINUE`（总闸）、`G_NOT_SKIP`（回调）、`G_ADVANCE`、`G_RECEIVE`、`G_FINISH`（本轮到此为止）、
  `G_FORWARD`、`G_PACK`、`G_FEED_GEN`
- 计时器：`set_timer` / `tick_timers` / `effective` —— 压制只影响 looper 的**判定**，不改用户的 flags，
  计时归零后自动恢复
- 字段布局：`position`(u16, 轮尾 +1) / `user`(u8) / `timer0`(u16) / `timer1`(u16) / `gates`(u8)

## 编译示例

```sh
g++ -std=c++23 -O0 -I. -o smoke examples/condition_smoke.cpp && ./smoke
```

## 文档

- [meta_states_looper.md](meta_states_looper.md) —— 引擎设计说明与用法
