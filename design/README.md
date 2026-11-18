# design/ —— 重构设计与规划

目的：把已经讨论过的**决策、协议、布局、计划**全部落到文件里，任何一条都不再依赖记忆。

| 文档 | 内容 | 状态 |
|---|---|---|
| `01_concept.md` | 框架定位：引擎 / 策略 / 用法、三个平面、能力与代价 | 🟡 草案 |
| `02_chunk_protocol.md` | chunk 是“进程续跑点”而非列表切片；五条不变量；driver 的 boundary/resume | 🟡 草案 |
| `03_state_words.md` | 两个 u64 字：looper word 与 user word；掩码保护与复原 | 🟡 草案 |
| `04_isa.md` | FAMILY/ARG、8 个门位（含 `OBS`）、6 条指令、ACK | 🟡 草案 |
| `05_communicate.md` | 窗口内握手、`Fuel=2`、收敛规则、回执覆盖 | 🟡 草案 |
| `06_migration.md` | 旧 API → 新模型的映射；当前未提交改动清单 | 🟡 草案 |
| `07_plan.md` | 阶段 0–5、验收标准、测试清单、暂缓项 | 🟡 草案 |
| `08_decisions.md` | 决策记录，含被否方案与理由 | 🟡 草案 |

图例：✅ 定稿（可据此写代码） / 🟡 草案（待审） / ⬜ 未开始

## 当前工作区状态

未提交（`git status` 实测）：

| 文件 | 改动 | 说明 |
|---|---|---|
| `meta_objects.hpp` | +164 −0 | 新增 `flags_operator<u64>` 载体、`meta_states_object_impl<u64>`、`normalize_flags` 保留 u64、`is_flags_operator` u64 特化；u64 不提供 `.high` |
| `meta_states_looper.hpp` | 改动较大 | driver 状态压成 u64（`remaining` u8 + `phase` u8）；新增 `for_each_void` / `for_each_forward_void` |
| `examples/driver_flags_smoke.cpp` | 重写 | 按新 driver 布局重写，`static_assert` 已通过 |
| `examples/flags128_smoke.cpp` | 1 处 | `flags_operator<0>` → `flags_operator<u128{0}>` |
| `meta_states.hpp` | 1 处 | 同上 |
| `meta_core64.hpp` | 新增（319 行） | chunk 内部的控制面：两字布局、打包 `decode`（一个 u64）、`apply`/`communicate` 单字进出；无任何结构体值 |
| `test/core64_test.cpp` | 新增 | 正向断言 + 6 个负例（非法指令 / 不收敛）；只 include `meta_core64.hpp` |
| `meta_stream64.hpp` | 新增 | `stream_cond64<Fn>`（流规则）+ `ops::` 指令构造便利函数；Fn 直接返回 user word（方案 A） |
| `test/stream64_test.cpp` | 新增 | 窗口级规则测试：首轮延迟、空 generator、timer 结束、eol、receiver 初始化失败、指令保留/改写、USER 隔离 |
| `meta_loop64.hpp` | 新增（约 180 行） | 新模型的一轮（四窗口 + 门位裁决 + 计时器 tick + POS 前进）+ 有界驱动器 `run<S, Rounds>` |
| `test/loop64_test.cpp` | 新增 | 有界驱动的循环：3 轮流结束、首轮延迟、用户 stop、预算耗尽 |
| `meta_looper64.hpp` | 新增 | **真正的 looper**：跑到结束、产物 `::type`、值平面 `for_each`（含 stage 视图）、边界 chunk 钩子 |
| `test/looper64_test.cpp` | 新增 | 产物断言（`meta_jostream` 收集出 `exp_list<0,1,2,3>`）、`for_each` 计数与视图、边界 chunk 生效/非边界不动 |

**尚未做**：

- 9 处 `flags_operator<condition_seed>` / `flags_operator<counted::value.low>` 等未迁移到 `u128{...}`，u64 载体落地后这些示例**预期编译失败**；
- `stream_states_co` 未按 v2 重写；
- looper `step / chunk / driver` 未切到“双字 + `communicate`”；
- `meta_states_looper.md` §19 仍描述旧的 128 位 driver 布局；
- `examples/decision_smoke.cpp:41` 在**原始 HEAD** 上就失败（`filter_calls()==5` 实际为 10），属既有回归，与本次改动无关。

**约定**：重构期间不跑全量 smoke（耗时），只用 `07_plan.md` 列出的独立 test 验收。

## 分层（包含依赖，从底到顶）

```text
meta_core64   字 / 门位 / ISA / apply / communicate           （不含对象协议）
meta_loop64   step + 有界驱动；只认 state::fn，不认 stream    （循环层）
meta_objects  （既有）对象协议 meta_invoke / meta_object_invoke
meta_stream64  stream_cond64 + ops 指令糖                      （流层）
meta_looper64  数据驱动 loop + 产物 + 值平面                    （对外入口）
meta_chunk64   payload 级 chunk                                （边界）
```

循环层不得 include 流层：`step` 只把 `state::fn` 当作 condition 传给 `communicate`，
用户要流语义就自己传 `stream_cond64<...>`。

## 阅读顺序

1. `01` → `02` → `03` → `04` → `05`（概念到协议）
2. `06` → `07`（迁移与施工顺序）
3. `08`（历史与理由，回答“为什么不那样做”）

## 度量约定

- 只度量**编译期**：实例化数、模板深度、`-fsyntax-only` 时间、编译器 maxRSS。
- 运行期开销不作为度量标准：产物是常量（`type_safe_aligned_range` = 常量偏移 + 指针），
  运行期基准只用于验证“逻辑没有泄漏到运行期”。见 `08_decisions.md` D19。

## 文档约定

- 每篇四段：**目的 / 结论 / 待定 / 关联**。
- 数字必须标注测量条件；结论必须能指回代码位置或协议条款。
- 被否方案写进 `08_decisions.md`，不散落在正文。
