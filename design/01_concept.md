# 01 框架定位

## 目的

给“这个库到底是什么”一个稳定说法，避免用 TMP 库的尺子衡量它。

## 结论

> **一个编译期执行引擎。** 它不“计算类型”，而是驱动对象演化的轮次；
> `流`、`管道`、`函数链` 都是这套引擎上的策略，不是引擎的一部分。

### 层次

```
引擎   meta_states_looper      轮次 / 窗口 / 门位 / 记账 / 续跑
策略   meta_ios / meta_stream  流的方向、开始结束、节点组合
用法   for / binder / series   退化为“无流”或“多段流”
```

判据（代码可查）：

- `stream_states_co<Fn>` 是 `meta_states_object`，即**一个 condition**；流结束、首轮延迟、
  `no_generator`、receiver 初始化失败，全部由它改写门位实现；引擎不认识“流”。
- 引擎默认 generator 是 `meta_empty_o`：没有源、没有方向、没有结束也能跑轮次。
- `for` 是 `unchanged` condition，`print` 是需要卡住生成器的策略，两者同一引擎。

引擎契约一句话：

> 给你若干观察点、一个控制寄存器、一条可中断且可续跑的轮次序列；每一轮谁被调用、
> 状态怎么变，由对象与指令决定。

### 三个平面

| 平面 | 承载 | 由谁展开 | 产物 |
|---|---|---|---|
| 类型平面 | Condition / Receiver / Generator | 模板实例化 | `::type` 最终载荷 |
| 控制平面 | 门位、计时器、窗口、预算 | 每轮解码与 settle | 下一轮谁被调用 |
| 值平面 | 回调调用与返回值 | `for_each` 的递归 | 返回值（可为 void） |

`for_each` 是**值平面的解释器**：把轮次投影成调用、把门位投影成跳过、把停止投影成提前返回。
因为返回的是“最后一个被观察到的调用”，返回类型成为**序列性质**——这就是 `observes` /
`future_within_args` 探测存在的原因。

### 能力证明：`args_at`

`args_at<I>(args...)` 一次调用、无运行期循环、无 `variant`，返回第 I 个实参，**引用与值类别
原样保留**（`int&` / `const int&` / `int&&` / `std::string&&` 均有断言）。机制：

1. `exactly_at_co<I>` 只在第 I 轮观察，第 I+1 轮停止；
2. `for_each_forward` + `G_CONSUME` 每轮消耗一个实参，同一泛型 lambda 被实例化成不同签名；
3. 回调 `-> decltype(auto)` 原样转发；
4. 只有第 I 轮贡献返回类型。

运行时循环做不到：body 只有一个签名、异构实参进不了同一循环、返回类型声明时已固定。

### 代价（诚实版，GCC 16.2 aarch64，`-fsyntax-only`）

| 形态 | 2000 轮编译时间 |
|---|---|
| 语言自带 `consteval for`（状态是数据） | ~0.14s |
| 纯递归 + 分块 + 每轮 4 个新类型 | ~0.21s |
| 本引擎 `run<stream_states_co<>, meta_iterator, index_sequence_istream<N>>` | ~106s |

差距来自**每轮协议**（窗口、门位、驱动、值平面探测），不是递归本身。

### 边界

- 该用：类型级状态推进、可中断续跑、门位/计时器、按序列决定返回类型。
- 不该用：纯类型计算、一次性 fold、只追求编译速度的场合。

## 待定

- 是否保留 `MODEL.md` 作为对外入口（现已并入本篇）。

## 关联

`02_chunk_protocol.md`（续跑协议）、`04_isa.md`（控制面编码）、`08_decisions.md`（D1/D2/D5）。
