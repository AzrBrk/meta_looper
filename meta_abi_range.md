# meta_abi_range：非拥有 ABI 协程栈布局

`meta_abi_range.hpp` 是 `type_safe_aligned_range` 的上层视图，不分配、不释放内存，
不实现上下文切换，也不修改底层元编程组件。所有平台类型可以在同一个翻译单元中共存；
选择类型不会根据当前编译平台偷偷切换布局。

这里的“上下文”是**普通 ABI 函数调用边界上的协作式切换上下文**，不是中断现场、
完整 CPU 快照或操作系统线程上下文。完整抢占式保存需要更多寄存器及平台设施。

## 1. 类型与存储

命名空间为 `meta_abi`，可以使用 `meta_abi_range<N, Abi>` 或以下快捷别名。
`N` 是**外部存储总字节数**，包括工作栈、启动帧、寄存器槽、padding，不是纯可用栈容量。
所有视图要求低地址基址按 **16 字节**对齐，用户不传 alignment 模板参数。
这里是字节，不是“16 位”。

| 别名（均带 `<N>`） | ABI | 寄存器保存范围 |
|---|---|---|
| `meta_abi_range_win_x64` | Microsoft x64 | RBX/RBP/RDI/RSI/R12–R15、XMM6–15 的低 128 位、MXCSR、x87 控制字 |
| `meta_abi_range_sysv_x64` | System V AMD64 | RBX/RBP/R12–R15、MXCSR、x87 控制字；保留初始 128 字节 red zone |
| `meta_abi_range_win_x86` | Microsoft x86，普通 caller-cleanup 入口 | EBX/EBP/ESI/EDI、MXCSR、x87 控制字 |
| `meta_abi_range_sysv_x86` | 现代 System V i386，调用前 16 字节对齐 | EBX/EBP/ESI/EDI、MXCSR、x87 控制字 |
| `meta_abi_range_aapcs64` | 基础 AAPCS64 | X18–X30、D8–D15、FPCR/FPSR |
| `meta_abi_range_aapcs32_vfp` | AAPCS32，带 VFP 状态槽 | R4–R11、LR、D8–D15、FPSCR |
| `meta_abi_range_riscv64_lp64d` | RISC-V LP64D | S0–S11、RA、FS0–FS11（64 位）、FCSR |
| `meta_abi_range_riscv32_ilp32d` | RISC-V ILP32D | S0–S11、RA、FS0–FS11（64 位）、FCSR |

32 位 ARM 的 ABI 公共边界只要求 8 字节对齐，Microsoft x86 通常只要求 4 字节；
本组件提供更强的 16 字节基址/调用前对齐，不声称这些 ABI 自身要求 16 字节。
ARM32 的 VFP 槽也可由使用 VFP 的 softfp 调度器使用；无 VFP 的硬件不能执行这些保存指令。
不将这些 RISC-V D 布局冒充 LP64/ILP32、F 或 E ABI。

```cpp
#include "meta_abi_range.hpp"

#include <array>
#include <span>

using stack_view = meta_abi::meta_abi_range_win_x64<64 * 1024>;
alignas(stack_view::alignment)
std::array<std::byte, stack_view::storage_bytes> storage;

int main() {
    const stack_view stack{std::span<std::byte>{storage}};
    stack.initialize();  // 一次调用清零整个 N 字节；无额外参数
    auto* initial_rsp = stack.initial_sp();
    auto* saved_rsp_slot = stack.sp();
    (void)initial_rsp;
    (void)saved_rsp_slot;
}
```

`std::span<std::byte>` 构造检查容量，不足抛 `std::length_error`；
空指针或未对齐基址抛 `std::invalid_argument`。裸 `std::byte*` 构造与
`type_safe_aligned_range` 一样：容量至少为 N 是调用方前置条件，无法检查。
视图自身仅保存一个指针，`const` 不传播给外部内存，与底层 range / `std::span` 一致。
初始化不发生在构造时；`initialize()` 可以重新清零，但**不能清零正在执行或挂起待恢复的栈**。

初始化只做清零，不自动填入口地址、SP、退出地址、MXCSR 或 x87 默认控制字。
特别是全零 x86 浮点控制状态会解除异常屏蔽，不能直接用作一般 C++ 入口的执行环境。

## 2. 地址方向与统一接口

固定布局从低到高排列如下，工作栈向低地址增长：

```text
data() / stack_limit()
  |
  | 工作栈：函数栈帧、局部变量、嵌套调用
  |
frame() = data() + frame_offset       （16 字节对齐）
  | x86：启动帧；link-register ABI：直接开始寄存器保存区
  | 浮点 / SIMD、GPR、保存 SP 等固定槽位
layout_end() / stack_bottom() = data() + (N & ~15)
  | 0..15 字节不可用尾部余量（initialize 仍清零）
storage_end() = data() + N
```

高地址的寄存器槽不会被正常向下增长的栈覆盖。所有下列查询统一返回可写
`std::byte*`，没有指针解引用，没有隐式修改 SP，也没有动态保存值缓存。

| 接口 | 含义 |
|---|---|
| `data()` / `stack_limit()` | 整块缓冲区的最低地址，也是栈增长下界 |
| `stack_bottom()` | 向低地址增长的栈的高地址底边，即 `layout_end()`；底部固定帧在它下面 |
| `stack_top()` | 首次进入用户入口函数时的 SP 值；x64 必须为 `%16 == 8`，不是对齐分配上界 |
| `initial_sp()` | 第一次恢复时应写入保存 SP 槽的值；x86 指向第一次 `ret` 消费的槽 |
| `sp()` | **保存 SP 的内存槽地址**，不是槽中保存的 SP 值 |
| `ret_address()` | x86 的启动 PC 槽；link-register ABI 的独立恢复 PC 槽 |
| `return_address()` | x86 入口函数正常返回时消费的退出 PC 槽；其它 ABI 的 LR/RA 槽 |
| `frame()` | 固定保存区布局基址，包括 x86 启动帧 |
| `gpr_regs()` / `fp_regs()` | GPR / 浮点状态块首地址 |
| `fp_control()` | MXCSR+x87 控制字、FPCR/FPSR、FPSCR 或 FCSR 的首地址 |
| `simd_regs()` | SIMD / 浮点寄存器数组首地址；仅有实际数组的 ABI 提供 |
| `shadow_space()` | 仅 Windows x64 提供，入口函数的 32 字节 home area |
| `red_zone()` | 仅 SysV x64 提供，初始入口 SP 以下的 128 字节首地址 |
| `layout_end()` / `storage_end()` | 有效布局 / 外部存储的 one-past-end 指针，不可解引用，`stack_bottom()` 同样不可解引用 |

`stack_top()`、`initial_sp()`、`ret_address()` 是固定的启动布局地址，不会随着 yield 更新。
实际挂起后的 SP 必须从 `sp()` 槽读取。尤其 x86 后续恢复的 PC 在实际挂起栈的
`[saved_sp]`，不是固定的启动 `ret_address()` 槽；第一次 `ret` 之后，启动 PC 槽可以被工作栈覆盖。
`red_zone()` 同样只描述初始入口区间，不代表任意运行时 SP 的 red zone。

静态常量包括 `alignment`、`storage_bytes`、`layout_bytes`、`frame_bytes`、
`frame_offset`、`sp_offset`、`ret_address_offset`、`return_address_offset`、
`gpr_offset`、`fp_offset`、`initial_sp_offset`、`stack_top_offset`、
`usable_stack_bytes`、`shadow_bytes`、`red_zone_bytes`。
所有 `*_offset` 都相对于 `data()`。
`usable_stack_bytes` 是 `[stack_limit(), stack_top())` 的初始地址跨度；
SysV x64 的 128 字节 red zone 包含在其中，不是额外分配的空间。
这不保证空间足以运行实际 C++ 任务，调用方还必须预留足够的嵌套调用空间。

`aligned_range()` 返回底层整体 `type_safe_aligned_range` 视图：
第 0 槽是工作栈字节数组，后续是固定帧字段。
`frame_range()` 只包含固定帧字段，可以通过现有 `get<I>()`、游标和迭代器访问。
字段序列见下一节；没有新的类型列表遍历或偏移递归算法。

## 3. 固定帧字段与偏移

以下偏移以 `frame()` 为 0。全局偏移等于 `frame_offset + 表中偏移`。
`frame_offset = (N & ~15) - frame_bytes`，所以 N 增大时整个固定帧自动上移，
内部寄存器偏移不变。尾部 padding 已包含在 `frame_bytes` 中。

| ABI | `frame_range()` 槽 0 | 槽 1 | 槽 2 | 槽 3 | `frame_bytes` | 最小 N |
|---|---|---|---|---|---:|---:|
| Win x64 | bootstrap @0，48 字节 | FP @48，176 字节 | GPR @224，64 字节 | SP @288，8 字节 | 304 | 320 |
| SysV x64 | bootstrap @0，16 字节 | FP @16，16 字节 | GPR @32，48 字节 | SP @80，8 字节 | 96 | 240 |
| Win / SysV x86 | bootstrap @0，16 字节 | FP @16，16 字节 | GPR @32，16 字节 | SP @48，4 字节 | 64 | 80 |
| AAPCS64 | GPR @0，104 字节 | FP @112，80 字节 | SP @192，8 字节 | PC @200，8 字节 | 208 | 224 |
| AAPCS32 VFP | GPR @0，36 字节 | FP @48，80 字节 | SP @128，4 字节 | PC @132，4 字节 | 144 | 160 |
| RV64 LP64D | GPR @0，104 字节 | FP @112，112 字节 | SP @224，8 字节 | PC @232，8 字节 | 240 | 256 |
| RV32 ILP32D | GPR @0，52 字节 | FP @64，112 字节 | SP @176，4 字节 | PC @180，4 字节 | 192 | 208 |

最小 N 只保证帧之外至少 16 字节工作区，SysV x64 还额外保证 128 字节 red zone；
不足时编译期拒绝。N 非 16 倍数时先向下取整，再检查。

GPR 块内部顺序：

| 类型 | 从偏移 0 开始的寄存器顺序 |
|---|---|
| `win_x64_gpr_state` | RBX、RBP、RDI、RSI、R12、R13、R14、R15，各 8 字节 |
| `sysv_x64_gpr_state` | RBX、RBP、R12、R13、R14、R15，各 8 字节 |
| `x86_gpr_state` | EBX、EBP、ESI、EDI，各 4 字节 |
| `aapcs64_gpr_state` | X18..X30，共 13 个 8 字节槽；X29 是 FP，X30 是 LR |
| `aapcs32_gpr_state` | R4..R11，随后 LR，各 4 字节 |
| `riscv_gpr_state<Word>` | S0..S11，随后 RA，各一个 XLEN 字；S0 通常是 FP |

浮点块内部顺序：

| 类型 | 寄存器/控制字段 |
|---|---|
| `win_x64_fp_state` | XMM6..XMM15 @0..159；MXCSR @160（4 字节），x87 CW @164（2 字节） |
| `x86_fp_state` | MXCSR @0（4 字节），x87 CW @4（2 字节） |
| `aapcs64_fp_state` | D8..D15 @0..63；FPCR @64、FPSR @68，各 4 字节 |
| `aapcs32_fp_state` | D8..D15 @0..63；FPSCR @64（4 字节） |
| `riscv_fp_state` | FS0..FS11 @0..95；FCSR @96（4 字节） |

其余字节为 padding/reserved，不应载入硬件寄存器。
控制字段存储宽度不一定等于汇编读取特殊寄存器时的 GPR 宽度；
例如 AArch64 `mrs` 得到 X 寄存器，写槽时只存 W 的 32 位，恢复时零扩展后 `msr`。

## 4. x86：解决返回地址与 16 字节对齐

不能简单把一个返回地址放在“对齐后的最高地址减 8”，然后在第一次 `ret` 后
直接当普通 C++ 函数入口使用：必须同时考虑**入口函数自己的返回地址**以及 Windows
shadow space。这里的启动帧明确提供两级返回：

```text
Win x64（相对于 frame）
 +0   resume_pc     <- initial_sp()，第一次 ret 之前 RSP % 16 == 0
 +8   exit_pc       <- stack_top()，入口 RSP % 16 == 8
 +16  shadow[32]    <- shadow_space()，入口的 home area
 +48  FP 保存区开始

SysV x64
 +0   resume_pc     <- initial_sp()
 +8   exit_pc       <- stack_top()
 +16  FP 保存区开始

Win / SysV x86
 +0   reserved[8]
 +8   resume_pc     <- initial_sp()，ESP % 16 == 8
 +12  exit_pc       <- stack_top()，入口 ESP % 16 == 12
 +16  FP 保存区开始
```

这里 `ret_address()` 本身在 x64 上是 `%16 == 0`，**刻意不是 `%16 == 8`**：
它是额外构造的跳转槽，而不是普通 `call` 压入的唯一返回槽。第一次 `ret` 弹掉它以后，
入口 SP 指向 `exit_pc`，满足 `(RSP + 8) % 16 == 0`。
后续常规切换函数被 `call` 调用时，保存的 RSP 则通常为 `%16 == 8`，
恢复并 `ret` 后回到调用者的 `%16 == 0`；这两种保存 SP 的相位不要混淆。

入口应使用无栈参数的 trampoline。寄存器参数由汇编恢复器另行装入，或在 trampoline
中读取调度器的数据；此布局不自动构造参数。
x86 stdcall/fastcall 的 `ret imm` 会额外弹出参数，不能直接复用这里无参数的退出路径。

入口正常 `ret` 后，x64 退出 trampoline 收到的 SP 为 `%16 == 0`，
**不是普通被 call 的 C++ 函数入口状态**。退出地址应指向汇编 trampoline：
SysV x64 可在此状态直接 `call` 一个不返回的 C++ 完成处理函数；
Windows x64 先向下分配 32 字节 outgoing shadow space，再 `call`。
i386 此时 ESP 也是 `%16 == 0`，可以按调用约定布置参数后调用完成处理函数。
退出 trampoline 必须切回调度器，不能再对未定义的下一槽执行 `ret`。

## 5. 如何编写切换函数

### x86 普通调用边界

1. `from` / `to` 参数推荐传 `frame()`，汇编只需要上表的内部常量偏移。
2. 在更改当前 SP 之前，保存非易失 GPR、MXCSR、x87 CW；Win x64 还保存
   XMM6–15（例如 `movdqu` / `movdqa`，这些槽已按 16 字节对齐）。
3. 保存当前切换函数入口 SP，此时 `[SP]` 是恢复时应消费的普通返回地址。
   若切换器自行 push 寄存器或建立 prologue，必须撤销这些调整再保存，
   或设计一个对称的 push/pop 协议；不能和本节的直接 `ret` 协议混用。
4. 从目标固定槽恢复状态，最后加载目标保存 SP，执行 `ret`。
5. 新协程启动前，向 `sp()` 写入 `initial_sp()` 的地址值，向 `ret_address()`
   写入口 trampoline 地址，向 `return_address()` 写退出 trampoline 地址；
   将当前线程的合法浮点控制状态复制到目标控制槽。不要以全零默认值恢复执行环境。

MXCSR 含控制位与状态位，本布局为它保留完整 32 位；切换器可选择保存全部，
或只恢复 ABI 要求的控制位。不能加载保留位非零的非法 MXCSR。
没有 SSE 的旧 i386 平台不能执行 `stmxcsr` / `ldmxcsr`；槽可以存在，
但切换器须做 CPU feature 检查并只保存平台实际支持的状态。
SysV 的 XMM 寄存器和 x87 数据栈不是普通调用边界的非易失寄存器，不在此布局中保存。

### ARM / RISC-V link-register ABI

1. 保存非易失 GPR、相应浮点寄存器与控制状态，再保存当前 SP。
2. 将切换调用的返回 PC（LR/RA）保存到独立 `ret_address()` 槽。
   LR/RA 也有 GPR 保存槽，两个槽允许独立构造初始入口和退出路径。
3. 恢复目标 GPR/FP/控制状态和保存 SP，加载独立 PC 到 scratch 寄存器，
   AArch64 用 `br`、ARM32 用 `bx`、RISC-V 用 `jr` 跳转。
   不要用 `bl` / `blx` / `jal` 覆盖已经恢复的 LR/RA。
4. 新协程的保存 SP 设为 `initial_sp()`，恢复 PC 设为入口，
   `return_address()` 的 LR/RA 设为不返回的退出 trampoline。
   ARM32 Thumb 入口/退出地址的 bit 0 必须正确保留。

AAPCS64 基础 PCS 只要求保存 V8–V15 的**低 64 位**，因此这里保存 D8–D15；
不能把 `simd_regs()` 当成 8 个完整 Q 寄存器槽。SVE/SME、向量 PCS、Windows ARM64
或带额外平台约束的变种并不由此别名承诺支持。
AArch64 X18、ARM32 R9 是平台寄存器：槽存在不代表允许从其它协程/线程覆盖它；
应遵守目标平台约定，通常保留当前线程值。
RISC-V S 寄存器顺序并非连续的硬件寄存器编号：
S0/S1 为 X8/X9，S2–S11 为 X18–X27；FS0/FS1 为 F8/F9，FS2–FS11 为 F18–F27。
不要按数组下标错误生成连续 `x8..x19` 的保存代码。

### 适用边界与安全责任

- 不保存 caller-saved GPR/SIMD、RFLAGS/NZCV、x87 数据栈、AVX 高半部、AVX-512、
  SVE/SME、TLS、信号屏蔽字或 OS 线程元数据。RISC-V GP/TP 不属于本布局，
  不应切成其它线程的值。异步抢占或跨线程迁移不能仅凭此布局宣称安全。
- SysV red zone 不能被切换器在“任意指令抢占”时踩掉；本组件只保证调用边界协议，
  不保证从任意 leaf 指令切换。切换器自身也不能误写固定保存区。
- CET shadow stack/IBT、AArch64 PAC/BTI/GCS、Windows CFG/异常展开、
  栈探测、TEB StackBase/StackLimit、guard page、栈溢出检查和 sanitizer fiber hooks
  必须由调度器/平台后端处理。普通 `ret` 换栈协议不自动兼容硬件 shadow stack。
- 不应在没有正确 unwind 元数据的汇编边界传播 C++ 异常；入口 trampoline 应在自己的
  C++ 执行边界处理异常，再通知调度器。入口结束后切走，不能返回至未构造栈内容。
- 槽使用固定宽度整数，不使用宿主 `void*` 存储，因此多 ABI 布局可以共存。
  执行地址必须符合目标字宽；32 位槽不能装下任意 64 位宿主地址。
  字段值的端序编码由调用方处理：同机运行使用本机端序，跨平台构造不能盲目复制宿主整数。
- `sp()` 等查询只返回字节槽。可以用 `std::memcpy` 写入 `word_type`，
  或在符合底层对象生命周期约定的存储中使用 `frame_range().get<I>()`。

## 6. 示例验证

`examples/meta_abi_range_smoke.cpp` 同时实例化所有布局，检查容量、尾部余量、
对齐、SP/PC 槽、shadow space、red zone、清零边界及底层 range 写回。
在 x86-64 ELF 主机上额外运行一个**仅测试使用**的 SysV 切换器，实际换栈、
yield、恢复局部变量并通过退出 trampoline 回到调度器；库头文件不提供切换函数。
其它 ABI 的布局检查不等于在目标硬件上验证了切换指令。

Windows x64 这一侧由 `examples/abi_switch_smoke.cpp` 配
`assemble/abi_switch_win64.s`（手写汇编的 Win64 切换器，带 COFF 的
`.def/.scl/.type 32/.endef` 符号属性；换栈必然改 rsp、且 ret 到另一条栈，所以
刻意不产生 `.pdata`/`.xdata`）双向验证：B 在 A 挂起时直接读 A 的固定帧，确认
xmm6..xmm15、8 个非易失 GPR 和 sp 槽都按上表的偏移写进去；A 恢复后再核对寄存器
整块回来。B 还会故意用垃圾值砸一遍非易失寄存器并打开 FTZ，所以"存了但忘了恢复"
和"控制字没按上下文隔离"都会被抓到——只测 C++ 局部变量不可靠：`-O2` 会把常量比较
折叠掉，编译器也不一定用那 10 个 xmm。测试专用的探针汇编在
`assemble/abi_switch_probe_win64.s`。

```sh
g++-14 -std=c++23 -Wall -Wextra -pedantic-errors -O2 -I. examples/meta_abi_range_smoke.cpp -o /tmp/meta_abi_range_smoke
/tmp/meta_abi_range_smoke
clang++-18 -std=c++23 -Wall -Wextra -pedantic-errors -O2 -I. examples/meta_abi_range_smoke.cpp -o /tmp/meta_abi_range_smoke_clang
/tmp/meta_abi_range_smoke_clang

# Windows x64：换栈器本体 + 测试探针都是独立汇编文件
g++ -std=c++23 -Wall -Wextra -pedantic-errors -O2 -I. examples/abi_switch_smoke.cpp \
    assemble/abi_switch_win64.s assemble/abi_switch_probe_win64.s -o /tmp/abi_switch_smoke
/tmp/abi_switch_smoke
```

GCC 13 在当前底层 `meta_states_looper.hpp` 的结构化非类型模板参数处理中可能触发
内部编译器错误，现有 `type_safe_aligned_smoke.cpp` 也可复现；验证使用 GCC 14 和 Clang 18。

ABI 规则参考：
[Microsoft x64](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)、
[System V x86 ABI](https://gitlab.com/x86-psABIs)、
[AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)、
[AAPCS32](https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst)、
[RISC-V psABI](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)。
