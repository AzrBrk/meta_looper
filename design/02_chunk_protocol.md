# 02 chunk 协议：进程续跑点

## 目的

定义“分块”到底分的是什么。结论：分的是**递归进程的续跑点**，不是类型列表的切片。

## 结论

### 为什么不是列表切片

列表切片的依据是“剩余元素”；递归进程的依据是“状态是否闭合”。反例就是 `index_sequence_istream`：

- 它的状态是 `index_counter<剩余轮数>` 这个**进程**，不是“剩下几个下标”；
- 若按列表切片，每段会重新从 0 开始；要保留全局位置，就必须把计数器作为状态携带——
  那就是“携带进程状态”，只是换了名字。

所以“否则 `index_sequence_istream` 就不成立”。

### 边界的五条不变量

边界只能落在**两轮之间**，不能落在四个窗口中间。

| # | 不变量 | 谁保证 |
|---|---|---|
| 1 | 状态闭合：载荷、门位、计时器、预算、位置都能被确切表示 | 状态元组 `(Cond, Recv, Gen, LooperWord, DriverWord)` |
| 2 | 窗口对齐：`WINDOW` 归零，不存在半个窗口 | looper |
| 3 | 值平面对齐：`for_each` 的返回链在此断开，无悬挂结果 | 值平面驱动 |
| 4 | 单调性：恢复后 `POS`、预算只前进，不重算 | driver policy |
| 5 | 可重入：恢复不依赖上一段的实例化上下文 | chunk 入口（新实例化帧） |

第 5 条是“深度被重置”的**唯一**来源：恢复必须由新的模板实例化发起，否则只是换个名字的递归。

### driver policy 的接口

```text
seed_remaining              每段起点预算
round / more / decide       段内事件（已实现）
boundary(state) -> state'   段边界：决定哪些跨段保留、哪些重置
resume(state)  -> state'    进入下一段：恢复预算/窗口/计时器，并保证不变量 4
```

`boundary` 是新增关键点：

- **计时器**必须跨段存活（计数在 looper word，target 在类型上）；
- **低频指令**（`TIMER`）允许上一段末尾发出、下一段生效，由 policy 决定是否延迟；
- **值平面**是否携带“最后观察值”由 policy 决定；不需要就不携带（对应 `for_each_void`）。

### 对字布局的约束

凡是跨段必须存活的东西，都必须在状态元组里，不能是局部变量：

| 需要跨段 | 存放位置 |
|---|---|
| 计时器计数 | looper word `T0C/T1C` |
| 计时器 target | 状态类型 |
| `POS` / `WINDOW` | looper word |
| 门位 | 不进跨段状态：每轮/每窗口脉冲重建 |
| 回执 `ACK` | user word `MAIL`，跨段保留原样即可 |

## 6. 载荷级 chunk（v2 提案，待确认）

chunk 的前提是 **payload 可切**，而不是 looper 可切：

```cpp
// 载荷自己声明怎么切；只有需要时才实例化（惰性）
using slice = list::chunk<u64, list>::apply;      // = range_select<start_of<u64>, end_of<u64>, list>
using next  = obj::meta_set<slice>;               // 把切片塞回对象
```

规则：

1. **chunked_able 才能切**：`concept chunked_able<T> = requires { typename T::template chunk<u64>; }`
   之类的判据；没有 `chunk` 模板的载荷（例如 `index_sequence_stream` 这种）**不切**，收益也不大。
2. **切片在 driver 做，looper 不感知**：driver 决定边界、调用载荷的 `chunk`、再用 `meta_set` 换载荷；
   looper 只看到“又一轮”。
3. **不要预测 chunk 的 apply 行为**：driver 不假设切片后状态怎么变，只负责切与换。
4. **状态仍然压成 u64**：`(L, U)` 两个 u64，不因为 chunk 引入新结构。
5. 类型标识要真的变小才有收益：`ranged_index_sequence<start, end>` 这类“区间类型”比
   “从头重复的 `index_sequence_istream<N>`”更值得切，因为前者每段是**不同的类型**，
   段一多，类型总数才会从 O(N) 降到 O(N/B)。

## 7. 已落地的 chunk 协议（`meta_chunk64.hpp`）

```cpp
struct chunked_able {};                       // 可选标记基类
template <class T>
concept chunked_able_c = requires { typename T::template chunk<u64{0}>; };

// payload 自己声明怎么切；States 对 driver 是不透明的
struct my_list : chunked_able {
  template <u64 States> using chunk = /* 该段的 payload */;
};

// driver 在边界上唯一的动作：
template <class Obj, u64 States>
using chunk_object = /* chunked ? Obj::meta_set<Obj::type::chunk<States>> : Obj */;
```

规则（与实现一致）：

1. **只有 `chunked_able` 才切**：没有 `chunk` 成员模板的 payload 原样返回，避免未初始化元对象被误切；
2. **只在边界调用**：`chunk<States>` 不进轮内；**边界由 payload 定义，不用轮次预算**
   （终止条件是数据驱动的，固定“每 N 轮”切一次会割裂一个逻辑步，尤其 states_looper）；
3. **结果用 `meta_set` 塞回原对象**：函数、投影、未初始化状态都由 `meta_set` 的既有语义保持；
4. **`States` 由 driver 递增**（`next_segment`），但 driver 不解释它；段大小、切片方式、是否用
   `exp_list_select` 都是 payload 的自由；
5. 状态仍是 `(L, U)` 两个 u64，chunk 不引入新的字。

测试：[test/chunk64_test.cpp](test/chunk64_test.cpp) 用 4 元素列表按段长 2 切，
验证 `chunk_object<obj,0>` / `<obj,1>` 的载荷，以及没有 `chunk` 的 payload 原样返回。

## 待定

- `boundary` / `resume` 是否要暴露给自定义 policy（还是只留默认实现）。
- 值平面在边界“丢弃最后返回值”的判定放在 policy 还是 chunk 入口。

## 关联

`03_state_words.md`（字布局）、`07_plan.md` 阶段 2–3、`08_decisions.md` D3。
