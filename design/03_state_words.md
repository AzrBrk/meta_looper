# 03 状态字：两个 u64

## 目的

确定重构后的状态表示：两个 64 位字，全部运算为原生 u64，u128 从 looper 路径退役。

## 结论

### looper word（looper 独占）

| bits | 字段 | 宽 | 说明 |
|---|---|---|---|
| 0–7 | `GATES` | 8 | 本轮有效门位（脉冲，由指令重建） |
| 8–9 | `WINDOW` | 2 | 当前窗口 0..3 |
| 10–12 | `TARGET0` | 3 | 计时器 0 目标位下标（0..7） |
| 13–15 | `TARGET1` | 3 | 计时器 1 目标位下标（0..7） |
| 16–23 | `T0C` | 8 | 计时器 0 计数 |
| 24–31 | `T1C` | 8 | 计时器 1 计数 |
| 32–63 | `POS` | 32 | 轮次，上限约 43 亿；u16 溢出问题消失 |

计时器 **target** 占 3 位（位下标），随 `count` 一起在装表时写进槽；`apply` 在装表窗口当场清目标门（FINISH 永不压制），所以第一轮的 `gen` 也能被压住。装表接口仍是类型 `set_suppression_timer<TargetBit>`，内部 `countr_zero` 转下标。

### user word（user MSO 的 flags）

| bits | 字段 | 宽 | 说明 |
|---|---|---|---|
| 0–15 | `MAIL` | 16 | `FAMILY 3 | ARG 13`，请求与回执同槽 |
| 16–63 | `USER` | 48 | user 独占，looper 不解码 |

### 掩码保护与复原（四条不变量）

1. looper word 的 64 位全部归 looper；解码器只允许写 `GATES / T0C / T1C`。
2. `WINDOW / POS` 每窗口从 looper 自身快照恢复（`restore_owned`），越权写入不累积。
3. 非法组合一律 `static_assert`（越界 `count`、`REC` 缺 `CONT`、未注册 `EXT`）。
4. `write_ack` 只写 user word 的 16 位 `MAIL`，`USER` 48 位原样保留。

### 为什么 `POS` 不进类型参数

用户明确要求：`POS` 占位放 looper word，位置信息从 looper word 读；`index_of` 作为 wrapper
改从 looper word 读取，调用点语义不变。u16 → u32 同时消除了溢出。

### u128 的退役范围

- looper 路径（`meta_states_looper` / `stream_states_co` / `meta_pack_stream`）不再引用 u128；
- `flags_operator<u128>` 保留（pipe 尚未重构），但 looper 不依赖；
- `flags_operator<u64>` 不提供 `.high`，避免掩盖 128 位误用。

## 待定

- `POS` 用 u32 是否够（43 亿轮），还是留 40 位（去掉 8 位保留）？
- `t0/t1` 的 target 编码是否需要支持“同一计时器换目标”的原子操作。

## 关联

`02_chunk_protocol.md`（跨段存活项）、`04_isa.md`（`MAIL` 编码）、`08_decisions.md` D6/D7/D10。
