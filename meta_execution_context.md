# 运行时执行上下文：用 naked 函数换出真正的函数栈

> 解决的问题：`static_function_series` 已经把参数压在一块对齐缓冲里（“公共参数栈”），
> 但它只是数据布局，函数调用本身仍然是“取参 → 调用 → 存结果”的逐段推进。
> 能不能用 naked 函数换出一块真正的 CPU 栈，在里面直接套娃调用、任意点挂起？
>
> 结论：**行，而且不需要 Boost.Context；但它是平台锁定的原型，不是可移植的库能力。**

## 结论先说

| 方案 | 可行性 | 代价 |
|---|---|---|
| x86-64 SysV：naked trampoline + 手写 `swap_context`，arena 当栈 | 可行 | ABI 锁定；不能跨栈抛异常；调试器/栈回溯看不到协程栈 |
| x86-64 Win64：同上 | 可行，本仓库已跑通 | 必须保存 XMM6..15；SEH 没有 naked 帧的 unwind 信息 |
| AArch64 | 原理相同（x19–x28 / d8–d15 / fp/lr） | 本仓库只提供帧布局，不提供切换实现 |
| 库级可移植 | 不可行 | 等于重写 Boost.Context 的 ABI 与 OS 层 |
| C++20 无栈协程 | 可行但语义不同 | 只能在 `co_await` 点挂起，且默认一帧一次堆分配 |

`naked` 本身不是难点：它只是让你拿到一个没有 prologue/epilogue 的入口，
入口处的寄存器还是调用者传参时的样子。真正要处理的是 **ABI 契约**：
哪些寄存器是非易失的、新栈的初始形态怎么和恢复路径逐字节对上、入口的
`rsp % 16` 对齐、以及异常不能穿过没有 unwind 信息的帧。

## 组成

`meta_execution_context.hpp` 只有三样东西，加上一个入口 trampoline：

1. **`swap_context(from, to)`** —— `naked`，函数体就是一段汇编。
   - Win64：保存 `rbp`/`rbx`/`rsi`/`rdi`/`r12..r15` 和 `xmm6..xmm15`（XMM 在
     Win64 上是非易失的，漏了就是两台协程互相踩浮点寄存器）；
   - SysV：只有 `rbp`/`rbx`/`r12..r15` 六个，没有非易失 XMM。
2. **`stack_arena`** —— 从一整块内存里按 64 字节对齐 bump 出协程栈，不回收，
   整块 `reset()`。低端写一个 canary，越界能当场发现。
3. **`fiber`** —— 一块栈 + 一个保存点。`resume()` 进入/恢复，`yield()` 从任意
   深度挂起，`finished()` 判断是否结束。

新栈的初始形态必须和恢复路径**逐字节对齐**，这是整个实现里最容易写错的地方：

```
低地址                                           高地址
[xmm 160][r15][r14][r13][r12][rdi][rsi][rbx][rbp][跳转地址=trampoline][入口返回地址占位][shadow 32]
                    ↑ 恢复时 rsp 从这里开始，弹完再 ret 正好落进 trampoline
```

`ret` 之前 `rsp` 指向“返回地址”槽，`ret` 之后 `rsp = top + 8`，于是入口处
`rsp % 16 == 8` —— 和被 `call` 压过返回地址一样，trampoline 可以是个普通
C++ 函数，正常走 prologue。

首次进入时，`ret` 消耗的是跳转地址，不是 trampoline 自己的入口返回地址。
因此跳转槽之后还要预留一个 8 字节入口返回地址占位；Win64 还必须在其后
预留 32 字节 shadow space，即使 trampoline 不使用这块空间也不能省略。
入口占位清零，trampoline 必须不返回。SysV 只需入口占位，不需要 shadow space。
旧实现只预留跳转槽，优化后的入口未使用 home area 时可能照常运行，但不满足 ABI，
访问 home area 可能越过协程栈上界。普通可读写 arena 不会自动捕获这种越界。

## ABI 帧视图：`meta_abi_frames.hpp`

把 `rsp` 处的裸内存按 `type_safe_aligned_range` 解释成有名字的槽，偏移由布局
在编译期算出，不手写 `offsetof`：

```cpp
auto frame = meta_abi::frame_at<meta_abi::win64_x86_64_entry<meta_abi::stack_word>>(sp);
frame.get<meta_abi::slot_index(meta_abi::win64_x86_64_slot::arg0)>().value;
```

| 布局 | 槽 | 字节数 |
|---|---|---|
| `sysv_x86_64_entry<...>` | `ret_address`, 栈参数… | `8 + 8n` |
| `win64_x86_64_entry<...>` | `ret_address`, `shadow_space`(32), 栈参数… | `40 + 8n` |
| `win64_x86_64_context` | `ret_address`, `shadow_space`, `xmm6_15`(10×16) | 208 |
| `aarch64_trampoline` | `frame_record`(fp/lr), `callee_gprs`(x19–28), `callee_simd`(d8–15) | 160 |

被 `static_assert` 钉死的两个数字：Win64 第一个栈参数在偏移 **40**（不是 8），
`ret_address + shadow_space + 2×stack_word` 是 **56**（早期草稿写成 48，是把
shadow space 当成 24 字节了）。布局只是“类型化视图”，不分配、不拷贝。

## 例外与边界

- **异常不穿过裸帧。** 协程里抛出的异常在 `run()` 里被就地截住，`resume()` 在
  调用者的栈上重抛；否则 unwind 会走进没有 `.pdata` 的 naked 帧，直接 UB。
- **协程栈上的对象析构由 C++ 自己保证**（正常 return / 异常都会被展开到
  `run()` 之前），但**引用协程栈的指针不能逃逸**：`fiber` 一结束，那块栈就
  交给 arena 不再使用。
- **调试与栈回溯看不到跨协程的调用链。** 每个协程是独立栈，`backtrace` 只看
  得到当前这一条。
- **只在 x86-64 上实现。** Win64 路径在本机实测；SysV 路径只做了汇编级校验
  （本机没有 Linux 工具链，未运行）。

## 和 `static_function_series` 的关系

不改 `meta_static_function_series.hpp`：把整条流水线放进协程里跑，某个 stage
调用 `yield()` 就地从流水线中间挂起，恢复后从同一个位置继续。段与段之间
就是普通函数调用，参数真在 CPU 栈上：

```cpp
auto series = meta_bind::link_static(
    [](int input) { return input + 1; },
    [](int first, int scale) { meta_execution::yield(); return first * scale; },
    [](int input) { return std::to_string(input); });

series.initialize(2, 0, 4, 0);

// fiber 的入口是普通函数指针（不捕获），流水线对象放文件作用域。
auto* job = &series;
void run_pipeline(meta_execution::fiber&) { job->execute_feed(); }

meta_execution::stack_arena arena{memory, sizeof(memory)};
meta_execution::fiber worker{arena, 64 * 1024, run_pipeline};

worker.resume();   // 停在第 2 段中间
worker.resume();   // 跑完
```

## 验证

```sh
g++ -std=c++23 -Wall -Wextra -Werror -pedantic-errors -O2 -I. \
    examples/execution_context_smoke.cpp -o output/execution_context_smoke && \
    ./output/execution_context_smoke

g++ -std=c++23 -Wall -Wextra -Werror -pedantic-errors -O2 -I. \
    examples/abi_frames_smoke.cpp -o output/abi_frames_smoke && ./output/abi_frames_smoke
```

本机（MinGW g++ 16.2 / clang++ llvm-mingw）实测输出：

```
arena: 524288 bytes
fiber a sp=...8fb50  in [...7fbc0, ...8fbc0)
fiber b sp=...9fb50  in [...8fbc0, ...9fbc0)
nested: local 7 -> 7, simd 3.5 -> 3.5
fiber a after resume sp=...8fb50 (still in its own stack)
series: 3, 12, "12"
ping: 1000 yields, 1000 resumes
fiber exception rethrown on the caller stack
execution_context_smoke: ok
```

`clang++` 编同一个例子时需要额外加 `-Wno-unused-lambda-capture`：`-Werror` 下
`meta_static_function_series.hpp` 里原有的 `[this]` 捕获会触发该诊断（与本次改动无关，
任何用 `link_static` 的例子在 clang 上都会遇到）。

这四行分别证明：两块协程栈互不重叠且都落在 arena 里；嵌套调用在第三层挂起后
局部变量与 `__m128d` 原样还在；编译期流水线能从中间挂起再续；1000 次往返与
协程内异常的路径都稳定。
