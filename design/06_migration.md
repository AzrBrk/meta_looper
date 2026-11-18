# 06 迁移映射与现状

## 目的

把“旧 API 的每一处”映射到新模型，并记录当前工作区的半成品状态，避免误判。

## 结论

### API 映射

| 现在 | 新模型 |
|---|---|
| `G_CONTINUE` | `GATES` 的 `CONT` |
| `G_CALL_RECEIVER` | `GATES` 的 `REC` |
| `G_CALL_GENERATOR` | `GATES` 的 `GEN` |
| `G_NOT_SKIP` | `GATES` 的 `OBS`（**极性反转**） |
| `G_FINISH` / `G_FORWARD` / `G_INITIALIZE` / `G_CONSUME` | 同名门位 |
| `position_shift` / `position_mask` / u16 `position` | looper word `POS`（u32）；`index_of<L>()` |
| stage 镜像（user 字节高 4 位） | looper word `WINDOW` |
| flags 内 `timer0/1` | looper word `T0C/T1C` + 类型 target |
| `receiver_init_failed_mask` / `initialization_pending_mask` | `ACK.reason` |
| `stream_reserved_byte`（u128 高 64） | 删除 |
| `flags_operator<u128>`（looper 路径） | 不再引用（pipe 重构前保留类型本身） |

### 需要一起改的调用点（u64 载体落地后）

`condition_seed`、`G_*`、`counted::value.low` 等是 u64，`flags_operator<常量>` 会选中 u64 载体，
随后 `.high` 与 `set<u64, 64>` 会编译失败。必须显式写 `u128{...}`：

| 文件 | 行 | 备注 |
|---|---|---|
| `examples/args_at_smoke.cpp` | 22 | `set<u64, 64>`，必须改 |
| `examples/looper128_smoke.cpp` | 7 | `set<u64, 64>`，必须改 |
| `examples/condition_smoke.cpp` | 36, 89 | 防类型漂移 |
| `examples/execution_contract_smoke.cpp` | 72 | 同上 |
| `examples/pipe_boundary_smoke.cpp` | 24（及 16 的 `.low`） | 同上 |
| `examples/stream_control_smoke.cpp` | 206, 244 | 同上 |

### 当前工作区（未提交，实测）

| 文件 | 状态 |
|---|---|
| `meta_objects.hpp` | 已加 `flags_operator<u64>`、u64 MSO、`normalize_flags` 保留 u64、`is_flags_operator` u64 |
| `meta_states_looper.hpp` | driver 压成 u64；新增 `for_each_void` / `for_each_forward_void`；seed 与 policy 已转 u64 |
| `examples/driver_flags_smoke.cpp` | 按新 driver 布局重写并通过 |
| `examples/flags128_smoke.cpp` / `meta_states.hpp` | `flags_operator<0>` → `u128{0}` |
| `design/*` | 本套文档 |

**尚未做**：上表 9 处调用点未迁移；`stream_states_co` 未重写；looper 未切双字 + `communicate`；
`meta_states_looper.md` §19 仍为旧布局。

### 已知既有问题（与本轮改动无关）

- `examples/decision_smoke.cpp:41` `filter_calls()==5` 实际 10；在原始 HEAD（`git archive`）上同样失败。
- 重构期间不跑全量 smoke；验收走 `07_plan.md` 的独立 test。

## 待定

- `for_each_void` 在半成品状态下是否先保留（已加，但未在双字模型上验证）。

## 关联

`03_state_words.md`、`07_plan.md`、`08_decisions.md` D1/D5/D10。
