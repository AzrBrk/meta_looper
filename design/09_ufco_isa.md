# 09 UFCO 与 32 位指令

## 目的

把 looper 的状态从 condition 里收回到 looper 自己持有的 MSO（`user_flags_control_o`，下称 UFCO），
并把手写的 16 位 MAIL 指令集升级为 32 位、可表达多动作的指令集，从而：

- 消掉「looper word + user word」双字；
- condition 与 looper 状态解绑（condition 只负责窗口策略）；
- 一条指令能表达多个动作（例如同窗口装两个计时器），不再靠握手多次发送。

## 角色划分

| 角色 | 身份 | 职责 |
|---|---|---|
| `UFCO` | looper 持有的 MSO | `flags` = looper 的 64 位状态；`function::apply` 解码指令、推进状态 |
| `Condition` | `stream_states_co` 角色，独立 thread 的 MSO | 只看 USER word，产出本窗口指令（含自己的 32 位标志） |
| `meta_states_looper_step<UFCO, Condition, Receiver, Generator>` | 四窗口编排 | 不持有状态，只调两者并执行动作 |

### condition 的 apply 只看 USER word

condition 的 `function::apply` **不接收 looper word**（L）。它的入参只有：

```cpp
// Condition::function::apply
template <class U, meta_isa32::u8 Window, class Obj>
struct apply { static constexpr u64 value = /* 新的 U */; };
//   U      = [指令 32][用户标志 32]
//   Window = 本窗口号（由 looper 提供，不是从 L 里读）
//   Obj    = 本窗口对象（generator / receiver / timer）
```

- L（POS / STAGE / TIMER / GATES）完全在 UFCO 的 `flags` 里，condition 碰不到；
- condition 需要运行时信息时，发 `QUERY` 指令，从回执里读；
- UFCO 的 `function::apply<L, U, Receiver>` 读 U 的指令槽并推进 L，返回新 L。
  回执不进 L，而由 looper 用 `L'` + query bitmap 算出后写回 U 的指令槽。

## UFCO 的 64 位状态布局（恢复旧布局）

| bits | 字段 | 宽 | 说明 |
|---|---|---|---|
| 0..15 | `POS` | 16 | 轮次，字段内回绕 |
| 16..19 | `STATE` | 4 | `delay` / `no_gen` / `init_failed` / `pending`（旧 4 个流状态位） |
| 20..23 | `STAGE` | 4 | 窗口镜像，one-hot（`0x1/0x2/0x4/0x8`） |
| 24..39 | `TIMER0` | 16 | `[12:0]` count，`[15:13]` target |
| 40..55 | `TIMER1` | 16 | 同上 |
| 56..63 | `GATES` | 8 | 本窗口动作（正向门位） |

「高八位」= `GATES`，looper 从这里读动作。

### GATES 字节（bits 56..63，恢复原始位序）

| 字节位 | 字位 | 名称 | 含义 |
|---|---|---|---|
| 0 | 56 | `GEN` | `G_CALL_GENERATOR` 推进 generator |
| 1 | 57 | `REC` | `G_CALL_RECEIVER` 接收器执行 |
| 2 | 58 | `FINISH` | `G_FINISH` 本轮到此为止 |
| 3 | 59 | `FWD` | `G_FORWARD` 方向 Gen→Rec |
| 4 | 60 | `CONSUME` | `G_CONSUME` 消费实参 |
| 5 | 61 | `INIT` | `G_INITIALIZE` 用初始化替代推进/接收 |
| 6 | 62 | `OBS` | `G_NOT_SKIP` 本轮可观察 |
| 7 | 63 | `CONT` | `G_CONTINUE` 总闸 |

默认全集 = `CONT|OBS|GEN|REC|FWD|CONSUME` = `0xDB`；停机掩码 = `CONT|REC|GEN` = `0x83`。
`REC`/`GEN` 置位时必须有 `CONT`。

### `G_FINISH` 与 `G_FORWARD` 是一次性控制位

- `G_FINISH`：哪个窗口置位就在哪结束本轮；后续窗口的动作被截断（`pre_gen` 置位还压掉
  自己的推进）；由结束窗口记账一次（POS+1、tick、stage 复位）；轮尾清位。
- `G_FORWARD`：在 `pre_rec`（窗口 2）读；清零且本轮继续时，下一轮 `REC -> GEN` 互换角色
  （`next.rec = 本轮 gen`、`next.gen = 本轮 rec`）；**同样在轮尾清位**，下一轮由条件重新
  发放。`skip` 不清方向位（方向与跳过正交）。
- 停机（`CONT` 清零）会锁存：后续窗口的 `CONT|REC|GEN` 被掩掉，不会被重新打开。

### 与旧实现的逐位对比（丢了什么、拿回什么）

| 控制位 | 旧 (main) | 两字模型 | 本方案 |
|---|---|---|---|
| `POS` | 16 | 32 | 16（恢复） |
| stage 镜像 | 4 one-hot | 2 window | 4 one-hot（恢复） |
| `generator_delay` | 1 | 无（改用 `pos==0` 算） | 1（恢复） |
| `no_generator` | 1 | USER bit45（搬走） | 1（回到 looper） |
| `receiver_init_failed` | 1 | 无 | 1（恢复） |
| `initialization_pending` | 1 | 无 | 1（恢复） |
| `TIMER0` count | 16 | 8 | 13（target 占 3） |
| `TIMER1` count | 16 | 8 | 13（target 占 3） |
| timer target | 类型（0 位） | 3+3 | 3+3 |
| `GATES` | 8 | 8 | 8 |

净结论：两字模型丢了 `delay / no_gen / init_failed / pending` 4 个状态位、stage 编码 2 位、
两个计时器各 8 位；本方案全部拿回。唯一必然折中是**计时器计数 16→13**：
target 从编译期类型变成运行时值后必须占 3 位；要满 16 位计数就得把 target 退回类型层面，
与「一条指令装两个计时器」冲突。

## 32 位指令字

用户字 `U`（u64）= `[指令 32][用户标志 32]`。低 32 位完全归 condition，looper 不碰。

指令布局：`[31:28] FAMILY(4)`，`[27:0] PAYLOAD`（按族解释）。解码是 `consteval`，
用普通 `if`/`switch`，不产生按指令数量的模板实例化。

| FAMILY | 名称 | PAYLOAD 布局 | 约束 |
|---|---|---|---|
| 0 | `NOP` | 全 0 | 门位取默认全集 |
| 1 | `GATES` | `[7:0]` gates | `REC/GEN` 需 `CONT` |
| 2 | `ARM` | `[8]` slot，`[11:9]` target，`[16:12]` count5 | 0 < count ≤ 31 |
| 3 | `ARM2` | `[10:8]` t0 target，`[15:11]` t0 count，`[18:16]` t1 target，`[23:19]` t1 count | 两个 count 都 ≥ 1 |
| 4 | `GATES_ARM` | `[7:0]` gates，`[8]` slot，`[11:9]` target，`[16:12]` count | gates 一致 + count ≥ 1 |
| 5 | `GATES_ARM2` | `[7:0]` gates，`[10:8]` t0t，`[15:11]` t0c，`[18:16]` t1t，`[23:19]` t1c | gates 一致 + 两个 count ≥ 1 |
| 6 | `QUERY` | `[7:0]` bitmap | bitmap ≠ 0 |
| 7 | `ACK` | `[7:0]` mirror，`[10:8]` reason，`[11]` t0 run，`[12]` t1 run | 仅 looper 写 |
| 8..15 | `EXT` | 未注册 | 使用即 `static_assert` |

### `call` 式隐式微操作

- **装表即三件事**：`ARM`/`ARM2`/`GATES_ARM*` 里 count ≥ 1 的槽，UFCO 隐式
  「写 count/target 进槽 + 当场清目标门 + 后续每轮 tick」。用户只写计数。
- **`G_INITIALIZE` 与 `G_CALL_*` 同现 = 隐式选择分支**：用初始化替代推进/接收。
- **清 `G_CONTINUE` = 隐式收尾**：本窗口后续动作不再执行。

### `QUERY` 的 bitmap 与回执打包

bitmap：`bit0 POS`、`bit1 GATES`、`bit2 TIMER0`、`bit3 TIMER1`、`bit4 STAGE`。
回执写回 `U` 的指令槽，家族为 `ACK`，payload 按 bitmap 顺序紧凑打包：

```
bit0 给了 → POS    (16)
bit1 给了 → GATES  (8)
bit2 给了 → TIMER0 (count8 + target3, 11)
bit3 给了 → TIMER1 (11)
bit4 给了 → STAGE  (8)
```

一次查询里要的字段总和 ≤ 28 位（PAYLOAD 宽度），超出即 `static_assert`。

## 握手（保留）

```
condition.apply(U, Obj) -> U'         // U' 指令槽是本窗口请求
decode(U') -> 动作 / 查询
  动作族: UFCO.apply(state, instr) -> state' ；回执 = ACK(mirror, reason, t0,t1 run)
  QUERY : state 不变；回执 = 按 bitmap 打包状态字段
回执写回 U' 指令槽 -> U''
condition 再看 U''，可改写指令；key 相同即收敛；Fuel 上限（默认 2）
```

## step 的四窗口流程

```
template <class UFCO, class Condition, class Receiver, class Generator>
struct meta_states_looper_step;

每窗口（Window 是 looper 自己知道的窗口号）:
  1. U1 = Condition::function::apply<U0, Window, Obj>    // 只看 USER word
  2. instr = U1 的高 32 位（低 32 位是条件私有标志，原样保留）
     动作族: L1 = UFCO::function::apply<L0, U1, Receiver>
             reply = ACK(instr 镜像, reason, t0 run, t1 run)
     QUERY : L1 = L0
             reply = 按 bitmap 从 L0 打包 POS/GATES/TIMER/STAGE
  3. U2 = U1 的指令槽换成 reply                        // condition 下一轮看到结果
  4. gates = L1 >> 56                                 // 从 UFCO 高八位读动作
  5. 执行: GEN/INIT → 推进/初始化 generator
           REC/INIT → 接收/初始化 receiver
           OBS      → 本轮可观察
           CONSUME  → 消费实参
           CONT 清  → 停机
  6. 收敛条件: 新指令 key 与上一条相同；否则 Fuel-1 再问一次
```

## 影响与迁移

- `meta_states_looper.hpp` 的 step 重写为 `<UFCO, Condition, Receiver, Generator>`；
- `meta_core64` 的 16 位 MAIL 路径退役；
- `meta_stream64` 的 condition 改写为 32 位指令输出（`stream_states_co` 保持 MSO 身份）；
- pipe / print / test 逐步迁移。

## 替代方案

1. 定长 bundle（无 FAMILY）：解码最简单，但字段组合死板，用户否掉了。
2. 保持 16 位 + 更多 opcode：装不下「门 + 两个计时器」，不可行。
