# 07 阶段计划

## 目的

把重构拆成可验收的阶段，明确“不跑 smoke、用独立 test 验收”的执行方式。

## 结论

| 阶段 | 产物 | 验收 |
|---|---|---|
| 0 | **已完成**：`meta_core64.hpp`（布局 + ISA + `apply` + `communicate`）+ `test/core64_test.cpp` | 编解码往返、`ACK` 只写 16 位 `MAIL`、`apply` 复原 `WINDOW/POS`、`Fuel` 收敛与不收敛负例；已通过 |
| 1 | `test/word_test.cpp`：两字布局、掩码复原、`USER` 隔离、`NOP` 默认门位 | 同上 |
| 2 | `test/communicate_test.cpp`：`Fuel=1` 收敛、`Fuel=2` 修正一次、`Fuel=0` 触发 `static_assert`（负例 `#ifdef` 隔开） | 同上 |
| 3 | **基本完成**：`meta_looper64` 数据驱动跑到结束、产物 `::type`、`for_each` 与 `for_each_forward` 均返回最后观察值并支持 stop/skip/yield；跳过由 `OBS` 门位控制，`test/looper64_test.cpp` 覆盖全部 | 待做：payload 边界驱动的 chunk/resume（不用轮次预算） |
| 4 | **已完成（窗口级）**：`meta_stream64.hpp` 的 `stream_cond64<Fn>` + `ops::` 便利函数；首轮延迟用 `POS == 0` 表达，不再需要 delay 位 | `test/stream64_test.cpp`：延迟/空源/timer 结束/eol/初始化失败/指令保留与改写/USER 隔离 |
| 5 | 清账：looper 路径 `grep u128` 为零；`meta_states_looper.md` 同步；迁移 9 处调用点 | 文档与代码一致 |

### 测试清单（新建，不碰现有 examples）

```text
test/isa_test.cpp         编解码与约束
test/word_test.cpp        两字不变量
test/communicate_test.cpp 握手与收敛
test/chunk_test.cpp       续跑与边界
test/stream_test.cpp      stream_states_co 重写后的行为
test/mem_test.cpp         （可选）对象式 / payload 重建 / 分段 payload 的 RSS 对比
```

### 施工顺序

1. 阶段 0 与 1 可以并行（都只依赖字布局与 ISA）；
2. 阶段 2 依赖 0/1；
3. 阶段 3 依赖 2；
4. 阶段 4 依赖 3；
5. 阶段 5 最后做，含文档同步。

### 暂缓项

| 项 | 原因 |
|---|---|
| `pipe` 重构 | 先定 looper；pipe 依赖 driver 字与 policy |
| `for` 门面（`for_range` / `for_types`） | 只是 generator 选择 + body 适配，等协议稳定 |
| `MODEL.md` 对外化 | 内容已并入 `01_concept.md` |
| 全量 smoke | 重构期耗时；完成后一次性跑 |

## 待定

- `test/` 目录是否加入 `compile_flags.txt` / CI 脚本。
- 阶段 5 是否顺手把 9 处示例迁移与 `decision_smoke` 的既有失败一起处理。

## 关联

`04_isa.md`、`05_communicate.md`、`06_migration.md`。
