# meta_objects 使用说明

`meta_objects.hpp` 是 `meta_states_looper` 的最底层。**先读本文再动手**；直接把元对象里的
`::apply` / `::type` / `::function` 拆出来拼装，是最常见的误用，会绕开协议、破坏未初始化
处理，并让 MSO 的语义失效。

## 1. 元对象是什么（以及不是什么）

元对象是**模板递归的抽象**：一个对象 = 载荷（`::type`）+ 更新函数（`::function`），
每次调用 `apply<Args...>` 产出**一个新的元对象**（载荷已被更新），这就是 looper 一轮一轮递归
推进的基础。

- **不是类型列表。** 载荷可以*恰好*是列表，但元对象本身不是容器，不要对它做
  `exp_size` / 遍历 / 拼接 / 取元素；要列表就操作载荷（`pack::to_t`、`exp_list`）。
  （`exp_size<MO>` 只是为已初始化对象穿透到载荷的便利特化。）
- **不是函数的包装壳。** 不要为了“调一下 F”去取 `MO::function` 再自己 `meta_invoke`；
  这样得到的是裸载荷，丢掉了对象协议。
- **不是值。** 它们没有运行期状态；全部语义发生在类型层。

## 2. 对象家族

| 类型 | 载荷/产出 | 说明 |
|---|---|---|
| `meta_object<F>` | 未初始化 | `F` 须满足 `object_initializer`，首次调用用 `F::initialize` 建立载荷 |
| `meta_object<OBJ, F>` | `::type` | `apply<A...>` → `meta_object<meta_invoke<F, OBJ, A...>, F>` |
| `meta_ret_object<OBJ, F, Ret>` | `::type` + `::ret` | 更新状态用 `F`，对外产出 `::ret = Ret<OBJ>`（状态与结果分离） |
| `meta_timer_object<N, ...>` | `::type` + `::timer` | 带计数/中断判定的对象 |
| `meta_states_object<Flags, Fn>`（MSO） | `::flags` | 以 `u128` flags 为状态；`Fn::apply<flags, objects...>::value` 给出新 flags。**观察者** |
| `meta_empty_o` 等 | — | 占位对象 |

统一接口（`meta_object_t` 概念）：`meta_set<T>`（换载荷）、`apply<...>`、`function`。
初始化状态用 `initialized_meta_object_t` / `uninitialized_meta_object_t` 判断，
不要自己写 `requires { typename T::type; }`。

## 3. 对象之间怎么调用（核心规则）

> **元对象调用元对象，必须走 `meta_object_invoke<OBJ1, OBJ2, Args...>`
> （条件调用用 `invoke_object_if<cond>::apply<OBJ1, OBJ2>`）。
> 不要把 OBJ2 拆开（`OBJ2::type` / `::ret` / `::flags`）再喂给 OBJ1。**

`meta_object_invoke` 替你完成了这些事：

1. **按对象种类取正确产出**：pure/timer → `::type`，ret → `::ret`，MSO → `::flags`。
   自己拆很容易取错（例如把 ret 对象的 `::type` 当成它的结果）。
2. **未初始化处理**：OBJ2 可无参初始化则先初始化再调用；不能则返回“等待初始化参数的
   元函数”（用 `is_meta_function_v` 检测）。手拆 `::type` 会直接编译失败或拿到
   `meta_uninitialized`。
3. **MSO 观察者语义**：OBJ1 是 MSO 时，收到的是 OBJ2 **整个对象**（包括另一个 MSO，
   不自动初始化、不投影）；需要 flags 就显式读 `Object::flags`。手拆会丢掉这一层。
4. **一个入口，新增对象种类时只改协议**，调用方不必改。

```cpp
// 正确
using r  = meta_objects::meta_object_invoke<Receiver, Source, Extra...>;
using r2 = meta_invoke<meta_objects::invoke_object_if<cond>, Receiver, Source>;

// 错误：拆出载荷，绕开协议
using bad  = meta_invoke<Receiver::function, typename Source::type>;
using bad2 = meta_invoke<Receiver, typename Source::ret>;   // 种类变了就错
```

### 元对象调用非元对象

被调用方**不是元对象**（普通类型、类型列表、值类型等）时，直接 `meta_invoke<MO, Args...>`：
返回值是**更新后的元对象**（`MO::apply<Args...>`，载荷已更新），不是载荷本身。
`meta_object_invoke` 只用于“元对象调元对象”，因为只有那时才需要按对象种类投影产出。

```cpp
using next = meta_invoke<meta_object<int, F>, double>;  // 仍是元对象
using v    = typename next::type;                       // 需要载荷时再读
```

### 元对象当元函数用（含 meta_iostream 的流）

元对象本身就是元函数：**`meta_invoke` 整个元对象**，不要取出它的 `::function`。
`meta_iostream` 里的流也一样，它们只是元对象别名：

```cpp
using r = meta_invoke<meta_jostream<listA>, listB>::type;  // listA 与 listB 摊平合并
```

**严格禁止**直接使用 `xxx_stream_details::xxx_f`（如 `join_ostream_detail::auto_join`、
`basic_ostream_detail::add_f`）等流的底层算法：`detail` 命名空间不是公开接口，
绕过元对象就绕过了初始化、产出投影等协议，实现一改就坏。要这种行为就 `meta_invoke`
对应的流对象。

### 状态迁移与产出

- `meta_transfer_object<From, To>`：把 From 的产出写进 To 的载荷（`To::meta_set`），
  looper 用它把当前状态适配给条件对象。不要手写 `To::meta_set<From::type>`。
- `meta_produce_t<MO>`：只读取产出，**不调用**；未初始化为 `meta_uninitialized`。
  这是 cache / 流结束检查（`eol<meta_produce_t<...>>`）的来源。
- `meta_object_initialize<OBJ>`：保留已初始化对象 / 无参初始化 / 返回延迟初始化元函数。

## 3.5 什么时候可以读载荷（对照 looper / iostream / main.cpp）

对照现有代码，合法的“拆”只有两类：

1. **回调里的最终读取**：looper 回调拿到的 `stage_t` 就是当前载荷，用户读
   `stage_t::type`、`states.object()`、`typename decltype(states)::to_t::type`
   （见 `main.cpp`）是消费结果，不是元对象之间的调用。
2. **looper 实现内部（特例，勿效仿）**：`meta_states_looper.hpp` 为了编译时间必须做极端的
   位运算优化，否则整个状态机的编译时间会爆炸，所以允许在内部直接读写 MSO 的 flags。
   这个特例只属于 looper 本体；其它代码（binder / series / 示例 / 新组件）不得借此拆对象。
   具体表现：`meta_states_looper.hpp` 里对**自己持有的** MSO 条件对象读
   `Cond_o::flags` / `meta_set<flags_operator<...>>` 来暂存、恢复 flags，但**调用**仍走
   `meta_object_invoke<staged, Obj>`；Receiver 与 generator 之间用
   `invoke_object_if<_receive_>`；读生成器产物用 `meta_produce_t<Generator>`。
   `meta_iostream.hpp` 的 istream/ostream 都是 `meta_object` / `meta_ret_object` 的
   **别名**（`meta_jostream<TL> = meta_object<TL, auto_join>`），载荷是
   `exp_list`，但对外仍以对象身份交给 looper，调用者不碰 `::type`。

判定：对象 A 去调用对象 B 时，只要你手里在写 `B::type` / `B::ret` / `B::flags`，
就该换成协议。

## 4. 与 looper 的关系

`meta_looper<Condition, State, Generator>` 每轮做三件事，全部是对象协议：

1. `meta_transfer_object<State, Condition>` 求条件；
2. `invoke_if<cont>` 推进 Generator；
3. `invoke_object_if<cont>::apply<State, generatorStage>` 把生成结果应用到 State。

结束后 `::type` 是最终状态，回调里拿到的 `stage_t` 是每轮载荷。
**不要在 looper 之外重造这套递归**（手写 fold / 小 executor）；需要自定义条件用
`make_looper_condition`，需要执行入口用 `meta_states::run<...>`。

## 5. 常见误用清单

| 误用 | 后果 | 改法 |
|---|---|---|
| 把元对象当 `exp_list` 使用 | 语义错位，长度/遍历无意义 | 操作载荷，或用 `pack::to_t` |
| `OBJ2::type` / `::ret` / `::flags` 手拆后调用 OBJ1 | 种类/未初始化/MSO 语义丢失 | `meta_object_invoke` |
| 手写 `cond ? invoke : keep` | 两分支实例化，ill-formed 分支炸编译 | `invoke_object_if<cond>` |
| 用 `std::conditional_t` 选对象 | 同上 | `invoke_object_if` / `select_if_t` |
| 使用 `xxx_stream_details::xxx_f` 等流底层算法 | 绕过对象协议，依赖非公开实现 | `meta_invoke<某流, ...>` |
| 直接取 `MO::function` 另起 executor | 绕过协议 | 保持在对象内，用 `meta_object_invoke` |
| 假设 MSO 收到 flags | MSO 收到整个对象 | 显式读 `Object::flags` |
| 修改 `meta_objects.hpp` 的协议 | 影响全栈 | 先取得明确同意（见 `AGENTS.md`） |

## 6. 自检

1. 我是不是在 `typename X::type` / `::ret` / `::flags` / `::function` 上操作另一个元对象？
   → 换成 `meta_object_invoke` / `meta_transfer_object` / `meta_produce_t`。
2. 我是不是在把元对象当列表遍历？→ 取载荷，用 `exp_list`。
3. 条件分支会不会实例化不合法分支？→ `invoke_object_if` / `if constexpr`。

参考示例：`examples/object_invoke_smoke.cpp`。
