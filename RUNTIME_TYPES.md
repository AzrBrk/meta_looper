# TSA 运行时类型系统

> 状态：首版已实现，入口为 `type_safe_aligned_iterator.hpp`。
> 数据仍在原始 buffer 中；运行时数值能力直接扩展现有槽代理，不新增存储容器。

## 用法

```cpp
#include "type_safe_aligned_iterator.hpp"

using namespace type_safe_aligned;

int main() {
  alignas(double) std::byte buffer[field_size<0, int, double, char>()]{};
  type_safe_aligned_range<0, int, double, char> range(buffer);
  type_safe_aligned::initialize(range, -7, 2.5, 'a');

  std::size_t i = 1;
  std::uint64_t k = 2;
  double result = range[i] + k;       // 4.5，不修改槽位
  range[i] += k;                     // 原地写入 double，变为 4.5
  range[i] -= k;                     // 变回 2.5
  range[i] += 1.4;                   // 浮点增量：double 通道
  double half = range[i] - 0.5;      // 浮点操作数参与算术
  bool greater = range[i] > k;       // true
  range[i] = 3.5;                    // 保留原有严格类型赋值
  double& exact = range[i].as<double>();
  int& fixed = range.get<0>();       // 编译期类型、常量绝对偏移
  (void)result;
  (void)half;
  (void)greater;
  (void)exact;
  (void)fixed;
}
```

数值运算的右操作数走**一条**统一通道 `type_safe_aligned::operand`，它只有两种
解释：`std::uint64_t`（精确整数）与 `double`（浮点）。其它算术类型——普通 `int`
字面量、`std::uint32_t`、`float`——由 deleted 构造显式拒绝，避免隐式转换截断
或改变符号；需要时写 `std::uint64_t{2}` 或 `2.0`。
不要给每个运算符再加一族浮点重载：代理的重载集翻倍，而且每种槽位类型会多
实例化一份 thunk。新增操作数种类应该给 `operand` 加字段，而不是给运算符加重载。
`as<T>()` 后则是普通 C++ 引用，可以使用原生运算。

## 结构与分工

- `range[i]` 仍返回 `aligned_iterator_storage<Ty...>`，不是新的代理类型。
  代理仍只有槽位指针与 `std::size_t` 下标两个成员，原有类型数上限仍为 128。
- 布局与编译期绝对偏移表不变；运行期下标仍通过静态偏移表定位。
- `exp_list<stored_type_t<Ty>...>::for_each` 生成每槽位类型的 `op_set<T>`，
  再用 `exp_list::to` 生成操作表；不额外实现类型递归或 executor。
- 每种操作各有一张静态 `constexpr` 函数指针表，表项按槽位顺序排列。
  重复类型保留多个槽位下标，指向同一种类型的 thunk；下标不是去重后的类型编号。
- 数值派发不引入继承、虚函数、RTTI、动态注册、堆分配或额外的值容器。
  失败复用现有 `aligned_iterator_storage::exception`。

操作表只为实际使用的操作实例化，不要求所有 `Ty...` 都支持数值运算。
例如 `int, std::string, double` 可以共存，数值槽正常工作，字符串槽的数值操作抛异常。

## 首版操作与语义

| 入口 | 语义 |
|---|---|
| `slot + k` / `- k` / `* k` / `/ k` | 两边先转 `double`，返回 `double`，不写回 |
| `slot == k` / `!= k` / `< k` / `<= k` / `> k` / `>= k` | 数值比较，返回 `bool` |
| `slot <=> k` | 返回 `std::partial_ordering` |
| `slot += k` / `-= k` | 在原槽位写回，返回代理自身的引用 |
| `slot.as<T>()` | 受检的精确 `T&`，允许加 const，不允许去 const |
| `slot = value` | 原有严格类型赋值，不做跨类型数值转换 |

这里的 `slot` 也可以是迭代器解引用得到的代理，`k` 为 `operand`：
`std::uint64_t` 表示精确整数，`double` 表示浮点。
代理之间的赋值仍被删除，不能用 `range[i] = range[j]` 复制数值或重绑定代理。

数值能力限于存储类型为内建算术类型的槽位（包括 `bool`、字符、浮点与 const 数值）。
引用槽位仍存储 `std::reference_wrapper`，不会被数值运算自动解引用；使用
`as<std::reference_wrapper<T>>().get()` 操作被引用对象。
其它对象与枚举可继续使用原有精确类型访问，不自动转成数字。

### 精度与比较

算术结果固定为 `double`：大整数（尤其超过 `2^53`）及 `long double` 可能损失精度，
结果也可能为无穷或 NaN。需要精确结果时用 `as<T>()`、`at<T>(i)` 或 `get<I>()`。

比较**不先转 `double`**，也不先把右值窄化到槽位类型：
负的有符号整数始终小于非负的 `k`；大整数比较不丢精度。
浮点与整数比较先检查符号和 `2^64` 边界，再比较整数部分与小数部分，
因此 `double{2^64}` 大于 `UINT64_MAX`，不会由于舍入误判为相等。
NaN 为 unordered：`==`、所有大小比较为 false，`!=` 为 true。

操作数是 `double` 时，槽位值与操作数都先扩展成 `long double`（它能精确表示
`std::uint64_t` 与 `double`）再比较，整数槽位也不会因为窄化成 `double` 丢位；
NaN 操作数同样给出 unordered。

### 写回与失败

整数 `+=` / `-=` 按数学整数计算并检查目标范围，不先将 `k` 转成槽位类型，
也不使用有符号溢出或无符号回绕。`bool` 只接受结果 0 或 1。
浮点写回按槽位浮点类型运算，右值先转换成该类型，保留其舍入规则；
有限输入若产生非有限结果则拒绝，已有的 NaN / 无穷按浮点语义传播。
整数槽位收到非整数值的浮点操作数（如 `slot += 1.5`）抛 `unsupported_operation`，
不静默截断；整数值（如 `2.0`）仍走上面的精确整数路径，符号由操作数决定。
const 数值槽位可读、可比较，但不能通过复合赋值写回。

所有检查失败都抛 `aligned_iterator_storage<Ty...>::exception`，
通过 `code()` 读取错误。运行时操作依次检查越界、空指针，再检查操作能力和数值条件：

| 错误码 | 条件 |
|---|---|
| `index_exceed` | 无效槽位下标 |
| `null_pointer` | 空缓冲指针 |
| `type_not_safe` | `as<T>()` 或原有严格赋值的类型不符 |
| `unsupported_operation` | 非算术槽位的数值操作、const 槽位的复合赋值，或整数槽位的非整数浮点增量 |
| `arithmetic_overflow` | 复合赋值结果超出目标范围 |
| `division_by_zero` | 数值除法的 `k == 0`（含浮点槽位） |

失败的复合赋值保持原值。不存在任意尺寸 `bit_cast` 写入，也没有失败时返回假成功值。
对象必须已经构造；代理不拥有 buffer，也不开始或结束对象生命周期。
const range 仍是可写视图，与既有 `get<I>()` / `at<T>(i)` 语义一致。
对齐要求完全沿用 TSA；新派发不是字节拷贝访问，不会修复 packed 引用的未对齐问题。

## 性能入口

| 入口 | 路径 |
|---|---|
| `get<I>()` | 编译期类型与常量偏移，无运行期检查、无操作表 |
| `at<T>(i)` | 唯一匹配时验证槽位号，再用常量偏移；重复类型保留查表 |
| `range[i].as<T>()` | 运行期定位、边界/空指针检查、类型位图检查，然后直接给引用 |
| `range[i] + k` | 运行期定位与检查，然后按操作表间接调用 |

`as<T>()` 复用现有类型位图，支持重复类型与加 const，不承诺所有情况只做一次下标比较。
热循环可在循环前取出精确引用；`get<I>()` 路径不因新增操作表发生变化。

`bench/hetero_access_bench.cpp --runtime-only` 对比 `get<5>()`、`as<uint64_t>()`、
固定槽位的数值表派发，以及混合槽位的静态 switch / 数值表派发。
索引与数值在运行期生成，每轮使用内存屏障，交替测量 15 次、报告中位数并检查校验和，
不能将结果解读为所有数据分布下的性能承诺。

```sh
g++ -std=c++23 -O2 -Wall -Wextra -pedantic-errors \
  -falign-functions=64 -falign-loops=32 -I. \
  bench/hetero_access_bench.cpp -o bench_hetero
./bench_hetero --runtime-only
```

## 尚未实现

`view<T>()` 同质投影、带 tag 的精确混合 `runtime_value`、其它右操作数类型、
跨类型数值赋值、开放类型注册与 inline cache 不属于首版。
槽位类型固定，无需 per-slot 缓存；未来扩展不能改变现有精确访问与布局语义。
