# 05 communicate：窗口内握手

## 目的

把“发指令 → 解码 → 回执 → 用户也许改主意”收敛成每窗口内的一个有界循环，替代“回执晚一轮”。

## 结论

### 签名

```cpp
// 一个 u64 进，一个 u64 出：返回用户最终请求字（ACK 只在握手内部出现），
// 调用方用 apply(L, family, arg) 应用一次。
template <u64 L, u64 U, class Obj, class UFun, std::size_t Fuel = 2,
          u32 PrevKey = 0xFFFF'FFFF>
consteval u64 communicate();
```

### 一个单位交换

```text
读 user.MAIL
  ├─ FAMILY == ACK            → 收敛（用户没有新请求）
  ├─ 解码 → GATES / TIMER / GATES_ARM（约束 static_assert）
  ├─ 内部 `apply` 一次，用于回执里的计时器槽位与 reason（实测比不 apply 省 38 MiB 内存）
  ├─ 写 user.MAIL = ACK{mirror, reason, t0, t1}
  └─ 用户看到回执后再发一条 → 递归，Fuel - 1
```

### 终止条件（四重）

1. 出现 `FAMILY == ACK`（用户没发新请求）；
2. **同一请求重复**：新指令的 `key_of` 与上一条相同 → 只回执、不重复应用（计时器不会被装两次）；
3. 不动点：用户看到回执后没有改字；
4. `Fuel == 0` 仍未收敛 → `static_assert`。

### 不变量

- `write_ack` 只写 `MAIL`，`USER` 48 位原样保留；
- 用户 Fn 每窗口最多被实例化 `Fuel + 1` 次（含首次解码前的那次）；
- 用户 Fn 必须是纯的（MSO 无运行期状态，重复调用安全）；
- 收敛失败是**编译错误**，不是静默收敛。

### `Fuel` 取值

| Fuel | 语义 |
|---|---|
| 1 | 请求 → 回执，一步握手（等价“晚一轮”但同窗口返回） |
| 2 | 允许“看到回执后再修正一次”（当前选择） |
| >2 | 无实际用例，徒增实例化 |

### 用户 Fn 的新签名

```cpp
template <u8 Window>
struct my_condition {
  template <class L, class U, class Object>   // L=looper word，U=自己的 user word
  struct apply { static constexpr auto value = /* 新的 user word */; };
};
```

`POS` 从 `L` 读（`index_of<L>()`），`Window` 与 `L.WINDOW` 一致。

## 待定

- 用户 Fn 是否允许在回执后“撤回”上一条请求（当前允许，因为允许再发一条）。

## 关联

`02_chunk_protocol.md`（边界不变量 3）、`04_isa.md`（`ACK` 编码）、`08_decisions.md` D9。
