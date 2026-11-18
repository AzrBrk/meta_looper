# 04 ISA：MAIL 的指令集

## 目的

定义 user word 中 `MAIL`（16 位）的编码：user 发指令，looper 解码并回执。

## 结论

### 帧格式

```
 0..2   FAMILY   3    opcode
 3..15  ARG     13    操作数 / 回执数据
```

### 门位（`GATES` 指令的 8 位掩码，全部正向）

| 位 | 名称 | 含义 | 旧位 |
|---|---|---|---|
| A0 | `CONT` | 继续循环 | `G_CONTINUE` |
| A1 | `REC` | 接收器执行 | `G_CALL_RECEIVER` |
| A2 | `GEN` | 生成器推进 | `G_CALL_GENERATOR` |
| A3 | `OBS` | 本轮可观察（`for_each` 回调） | **`G_NOT_SKIP` 取反** |
| A4 | `FINISH` | 本轮提前收尾 | `G_FINISH` |
| A5 | `FWD` | 交换方向 | `G_FORWARD` |
| A6 | `INIT` | 走初始化分支 | `G_INITIALIZE` |
| A7 | `CONSUME` | 消费一个实参 | `G_CONSUME` |

`OBS` 与 `REC` **正交**：`REC` 置位、`OBS` 清零 = 静默接收（`exactly_at` 的语义）。
`skip()` = 清 `REC|OBS`；`yield()` = 置 `REC|OBS`，与旧 `SkipMask` 语义一致。

### 指令表

| opcode | 名称 | ARG 布局 | 约束 |
|---|---|---|---|
| 0 | `NOP` | 全 0 | 门位取默认全集（`CONT|REC|GEN|OBS|FWD|CONSUME`） |
| 1 | `GATES` | `A0..A7` 门位 + `A8..A12` 置 0 | `REC/GEN` 需 `CONT` |
| 2 | `TIMER` | `A0..A7=count`，`A8..A10=target`，`A11..A12=0` | `count ≥ 1` |
| 3 | `GATES_ARM` | `A0..A7=mask`，`A8..A10=target`，`A11..A12=count` | `count ∈ [1,3]`（更长计数走 `TIMER`） |
| 4 | `ACK` | `A0..A7=被接受镜像`，`A8..A9=reason`，`A10=t0 活跃`，`A11=t1 活跃`，`A12=0` | 仅 looper 写 |
| 5–7 | `EXT` | 未注册 | 使用即 `static_assert` |

### `reason`

| 值 | 含义 |
|---|---|
| 0 | ok |
| 1 | `REC` 缺 `CONT` |
| 2 | `GEN` 缺 `CONT` |
| 3 | 计时器槽已满 |

### 解码器的形状（避免每条指令一个类型）

```cpp
consteval bool valid(u16 mail);    // 整个指令字，纯布尔掩码
consteval u64  decode(u16 mail);   // 打包成一个 u64：
                                   //  0..7 gates, 8..15 count, 16..23 target,
                                   //  24..31 family, 32 rebuild, 33 arm, 40..47 reason
constexpr u8 d_gates_v(u64);       // 全是移位/掩码访问
constexpr u8 d_count_v(u64);
constexpr u8 d_family_v(u64);
constexpr bool d_arm_v(u64);
```

没有 `decoded` 结构体、没有 `decode<>` 模板：**N 条指令既不会产生 N 个类型，也不会产生 N 个复合值**；
调用点用 `static_assert(isa::valid(family, arg))` 校验。

### 语义规则

1. **脉冲**：门位每轮/每窗口从指令整体重建，不在上一轮基础上增量修改；
   因此 `FINISH` 天然一次性，粘性状态消失。
2. **回执即指令**：`ACK` 覆盖同一 `MAIL` 槽；指令只发一次，覆盖不丢信息。
3. **组合用专用 opcode**：需要同时表达时新增指令（如 `GATES_ARM`），不扩宽信箱。
4. **越界即编译错误**：所有约束 `static_assert`，不做截断。
5. **一条/窗口**：单槽信箱每窗口最多一条请求；`communicate` 允许若干次往返（见 05）。

## 待定

- `EXT` 三个 opcode 是否开放给外部 trait（还是永久保留）。
- `NOP` 默认门位是否包含 `OBS`（当前方案：包含）。

## 关联

`03_state_words.md`（`MAIL` 位置）、`05_communicate.md`（往返次数）、`08_decisions.md` D5/D8/D13。
