# meta_looper 开发约定

## 先找框架原语，再写新代码

这个仓库的底层是一套完整的元编程库。新增 binder / series / 流式组合之前，先确认现有原语
能不能直接表达；能复用就不要在 `detail` 里另造一套类型递归、列表拼接或小 executor。

### 类型列表

- `exp_list<T...>`：列表本体；`exp_list<T...>::to<TL>` 直接转成 `TL<T...>`（例如
  `exp_list<Binders...>::to<function_series>`）。
- `exp_select<I, TL>` / `exp_select_from`、`take`、`exp_size`、`length_*`、`value_is`：
  取元素、截断、长度、判断，不要手写 `select_type` 递归。
- looper 回调的第一个参数是 pack：`pack::to_t` 就是当前轮累积出来的类型列表；需要
  “流结束后得到一个列表类型”时优先用它，而不是自己 fold/concat。

### looper / 流

- `meta_states::run<Condition, Receiver, Generator>::for_each` /
  `for_each_forward`：标准执行入口；回调返回值、`skip_t` / `yield_t` / `stop_t`、
  额外参数都由它处理。
- `meta_ios::stream_states_co<Fn>`：流控制条件；只需要 `Fn` 处理用户门位/自定义条件，
  初始化、结束、计时器不要重写。
- `meta_iostream::meta_jostream<exp_list<>>`：按类型收集并摊平类型列表。
- `meta_iostream::meta_istream<TL>` / `index_sequence_istream<N>` /
  `meta_repeat_istream<T>`：有界/索引/重复 generator。恰好 N 轮用
  `index_sequence_istream<N>`，不要再写“repeat + 位置到 N 关 G_CONTINUE”的条件。
- `meta_pipe` / `aligned_offset_stream` / `seek_to` / `type_safe_aligned_range`：
  对齐布局和偏移表。

### 调用与调度

- `meta_invoke` / `invoke_if`、`meta_objects::flags_operator`、`select_if_t`、
  `always_fn`、`identity_fn` 等已有协议；不要另写一套 invoke/条件选择。
- `function_binder` / `function_series` 只做 binder 语义和连续缓冲管理；类型计算、
  流控制、遍历尽量交给上面的原语。

## 判定“造轮子”的自检

1. 这个类型/函数是否已经在 `meta_utilities.hpp`、`meta_iostream.hpp`、
   `meta_states*.hpp`、`type_safe_aligned*.hpp` 里出现过？
2. 是否只是“遍历类型列表 + 选择/拼接 + 在最后一轮回调”？
   如果是，优先用 `exp_list`、`pack::to_t`、`stream_states_co`、`meta_jostream`。
3. 新增 `detail` 工具时，注释说明为什么现有原语不能表达；不要为了少写一层
   `typename` 就复制一套递归。

## 验证

- GCC/Clang：`-std=c++23 -Wall -Wextra -pedantic-errors`。
- 示例：`examples/function_binder_smoke.cpp`、`examples/function_series_smoke.cpp`、
  `examples/window_series_smoke.cpp`。
- 性能：`bench/function_binder_bench.cpp`（`-O2 -falign-functions=64
  -falign-loops=32`）；不要用微基准的偶然常数折叠当结论。
