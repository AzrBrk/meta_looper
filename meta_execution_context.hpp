#pragma once

// ============================================================================
// meta_execution_context —— 用 naked 函数切栈的协程执行上下文
//
// 回答的是 11/9 那个问题：static_function_series 里已经有一块“公共参数栈”，
// 能不能用 naked 函数换出一块真正的函数栈，在里面直接套娃调用、任意点挂起，
// 而不是把参数搬进一块共享缓冲、再靠状态机逐段推。
//
// 结论（也是这里的实现范围）：
//   - 行，而且不需要 Boost.Context：一个 naked 的 swap_context（只保存 ABI
//     要求的 callee-saved 寄存器 + 换 rsp）+ 一块 bump 分配的栈 + 一个入口
//     trampoline，就是全部；
//   - 但它是**平台锁定**的：栈帧布局、callee-saved 集合、入口对齐、异常
//     unwind 全都跟 ABI 绑死。这里只实现 x86-64 的 Win64 / SysV 两条路径，
//     其它目标直接 #error，不做“看起来能编”的降级；
//   - 协程帧的分配交给 stack_arena（bump，不回收，整块 reset），所以既没有
//     堆分配，也没有标准无栈协程那种一帧一 malloc 的碎片。
//
// 使用形状：
//   std::byte memory[256 * 1024];
//   meta_execution::stack_arena arena{memory, sizeof(memory)};
//   meta_execution::fiber f{arena, 64 * 1024, [](meta_execution::fiber& self) {
//     ...
//     meta_execution::yield();     // 任意深度的普通函数都能挂起
//   }};
//   while (f.resume()) { ... }     // 挂起 / 恢复，返回 false 表示已结束
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <utility>

namespace meta_execution {

// 一个被挂起的执行点：rsp + FP 控制状态。
// FP 状态为什么单独存：
//   - Win64：MXCSR[6:15]（控制位）与 x87 CW（FPCSR）都是非易失，callee
//     修改必须恢复；XMM6..15 也非易失，但它们已经压在这个 rsp 指向的栈帧里。
//   - SysV ：MXCSR 控制位与 x87 控制字同样 callee-saved；XMM 全部 caller-saved，
//     不需要保存。
// 不存 MXCSR[0:5] 的取舍：状态位是 caller-saved 的粘滞标志，存整个 32 位
// 也无害（恢复旧标志不会触发新异常），这里直接存整个寄存器。
struct context {
  void* sp = nullptr;
  std::uint32_t mxcsr = 0;
  std::uint16_t x87_cw = 0;
};
// swap_context 的裸汇编用固定偏移访问这两个字段，布局一变就是 ABI bug。
static_assert(offsetof(context, sp) == 0);
static_assert(offsetof(context, mxcsr) == 8);
static_assert(offsetof(context, x87_cw) == 12);

namespace detail {

// ---------------------------------------------------------------------------
// swap_context(from, to)
//
// naked：编译器不生成 prologue/epilogue，函数体就是下面这段汇编，所以入口处
// 的寄存器还是调用者传参时的样子（Win64: rcx/rdx，SysV: rdi/rsi）。
//
// 保存顺序即恢复顺序的逆序：
//   push rbp, rbx, rsi, rdi, r12, r13, r14, r15   （Win64 还要 xmm6..xmm15）
//   [rsp] = 当前栈顶 → *from
//   rsp = *to
//   弹回寄存器 → ret
//
// 新栈的初始形态必须和恢复路径逐字节对齐：见 fiber::prepare_stack。
// ---------------------------------------------------------------------------
#if defined(_WIN64) && (defined(__GNUC__) || defined(__clang__))

// Win64：XMM6..XMM15 是非易失的，必须一起保存，否则两台协程会互相踩浮点寄存器。
__attribute__((naked, noinline, used)) void swap_context(context* /*from*/,
                                                          context* /*to*/) {
  __asm__ volatile(
      "pushq %rbp\n\t"
      "pushq %rbx\n\t"
      "pushq %rsi\n\t"
      "pushq %rdi\n\t"
      "pushq %r12\n\t"
      "pushq %r13\n\t"
      "pushq %r14\n\t"
      "pushq %r15\n\t"
      "subq $160, %rsp\n\t"
      "movups %xmm6, 0(%rsp)\n\t"
      "movups %xmm7, 16(%rsp)\n\t"
      "movups %xmm8, 32(%rsp)\n\t"
      "movups %xmm9, 48(%rsp)\n\t"
      "movups %xmm10, 64(%rsp)\n\t"
      "movups %xmm11, 80(%rsp)\n\t"
      "movups %xmm12, 96(%rsp)\n\t"
      "movups %xmm13, 112(%rsp)\n\t"
      "movups %xmm14, 128(%rsp)\n\t"
      "movups %xmm15, 144(%rsp)\n\t"
      // FP 控制状态先存进 context（此时 rcx = from 还没被覆盖）：
      //   stmxcsr 存整个 MXCSR；fnstcw 是不等待版的 x87 控制字读取。
      "stmxcsr 8(%rcx)\n\t"
      "fnstcw 12(%rcx)\n\t"
      "movq %rsp, (%rcx)\n\t"
      "movq (%rdx), %rsp\n\t"
      // 换成目标栈后立刻恢复目标的 FP 控制状态（rdx = to，仍然有效）。
      "ldmxcsr 8(%rdx)\n\t"
      "fldcw 12(%rdx)\n\t"
      "movups 0(%rsp), %xmm6\n\t"
      "movups 16(%rsp), %xmm7\n\t"
      "movups 32(%rsp), %xmm8\n\t"
      "movups 48(%rsp), %xmm9\n\t"
      "movups 64(%rsp), %xmm10\n\t"
      "movups 80(%rsp), %xmm11\n\t"
      "movups 96(%rsp), %xmm12\n\t"
      "movups 112(%rsp), %xmm13\n\t"
      "movups 128(%rsp), %xmm14\n\t"
      "movups 144(%rsp), %xmm15\n\t"
      "addq $160, %rsp\n\t"
      "popq %r15\n\t"
      "popq %r14\n\t"
      "popq %r13\n\t"
      "popq %r12\n\t"
      "popq %rdi\n\t"
      "popq %rsi\n\t"
      "popq %rbx\n\t"
      "popq %rbp\n\t"
      "ret\n\t");
}

#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))

// SysV AMD64：rdi = from，rsi = to；callee-saved 只有 6 个 GPR，没有非易失 XMM。
// MXCSR 控制位与 x87 控制字同样 callee-saved，必须随上下文一起换。
__attribute__((naked, noinline, used)) void swap_context(context* /*from*/,
                                                          context* /*to*/) {
  __asm__ volatile(
      "pushq %rbp\n\t"
      "pushq %rbx\n\t"
      "pushq %r12\n\t"
      "pushq %r13\n\t"
      "pushq %r14\n\t"
      "pushq %r15\n\t"
      "stmxcsr 8(%rdi)\n\t"
      "fnstcw 12(%rdi)\n\t"
      "movq %rsp, (%rdi)\n\t"
      "movq (%rsi), %rsp\n\t"
      "ldmxcsr 8(%rsi)\n\t"
      "fldcw 12(%rsi)\n\t"
      "popq %r15\n\t"
      "popq %r14\n\t"
      "popq %r13\n\t"
      "popq %r12\n\t"
      "popq %rbx\n\t"
      "popq %rbp\n\t"
      "ret\n\t");
}

#else
#error "meta_execution_context: only x86-64 Win64/SysV context switching is implemented"
#endif

}  // namespace detail

// 当前正在运行的协程；没有协程时是 nullptr（主流程）。
class fiber;
inline thread_local fiber* running_fiber = nullptr;

namespace detail {

// 新栈第一次被 ret 进来时落在这里。它是普通 C++ 函数：入口时 rsp 满足
// ABI 的“刚被 call 过”的对齐（见 prepare_stack），所以可以安全地走 prologue。
// 定义放在 fiber 之后（需要完整类型）。
[[noreturn]] void fiber_trampoline();

}  // namespace detail

// ---------------------------------------------------------------------------
// stack_arena：从一整块内存里向下 bump 出协程栈，不回收，整块 reset。
// ---------------------------------------------------------------------------
class stack_arena {
 public:
  struct bounds {
    std::byte* low;
    std::byte* high;
  };

  stack_arena(void* memory, std::size_t bytes) noexcept
      : base_(static_cast<std::byte*>(memory)),
        size_(bytes),
        cursor_(static_cast<std::byte*>(memory)) {}

  // 每块栈按 64 字节对齐：x86-64 的 ABI 只要 16，这里留出余量给 SSE 和
  // 以后可能加的 guard 区。低端写一个 canary，溢出时能当场发现。
  bounds allocate(std::size_t bytes) noexcept {
    constexpr std::size_t alignment = 64;
    constexpr std::size_t guard = sizeof(std::uint64_t);
    auto* low = reinterpret_cast<std::byte*>(
        (reinterpret_cast<std::uintptr_t>(cursor_) + alignment - 1) &
        ~std::uintptr_t{alignment - 1});
    auto* high = low + bytes;
    if (high > base_ + size_) std::abort();
    std::memset(low, 0, guard);
    cursor_ = high;
    return {low, high};
  }

  std::size_t used() const noexcept {
    return static_cast<std::size_t>(cursor_ - base_);
  }
  std::size_t size() const noexcept { return size_; }
  void reset() noexcept {
    std::memset(base_, 0, size_);
    cursor_ = base_;
  }

 private:
  std::byte* base_;
  std::size_t size_;
  std::byte* cursor_;
};

// ---------------------------------------------------------------------------
// fiber：一块真栈 + 一个保存点。resume 进入/恢复，yield 任意深度挂起。
// ---------------------------------------------------------------------------
class fiber {
 public:
  using entry_type = void (*)(fiber&);

  fiber(stack_arena& arena, std::size_t stack_bytes, entry_type entry)
      : entry_(entry) {
    stack_ = arena.allocate(stack_bytes);
    prepare_stack();
  }

  fiber(fiber const&) = delete;
  fiber& operator=(fiber const&) = delete;

  // 返回 true 表示这次恢复后还会继续（没结束）。
  // 协程里抛出的异常不会穿过 naked 换栈帧，而是在这里、调用者的栈上重抛。
  bool resume() {
    if (finished_) {
      if (failure_ != nullptr) {
        std::exception_ptr failure = std::exchange(failure_, nullptr);
        std::rethrow_exception(failure);
      }
      return false;
    }
    fiber* previous = running_fiber;
    running_fiber = this;
    detail::swap_context(&caller_, &ctx_);
    running_fiber = previous;
    if (finished_ && failure_ != nullptr) {
      std::exception_ptr failure = std::exchange(failure_, nullptr);
      std::rethrow_exception(failure);
    }
    return !finished_;
  }

  // 从任意深度的调用栈里挂起，回到 resumer。
  void suspend() { detail::swap_context(&ctx_, &caller_); }

  bool finished() const noexcept { return finished_; }
  std::byte* stack_low() const noexcept { return stack_.low; }
  std::byte* stack_high() const noexcept { return stack_.high; }
  bool owns(std::byte const* pointer) const noexcept {
    return pointer >= stack_.low && pointer < stack_.high;
  }

  // 由 trampoline 调用：跑用户函数，标记结束，然后永久挂起。
  // 异常在这里就地截住（naked 换栈帧没有 unwind 信息，穿过去就是 UB），
  // 交给 resumer 在它自己的栈上重抛。
  [[noreturn]] void run() {
    try {
      entry_(*this);
    } catch (...) {
      failure_ = std::current_exception();
    }
    finished_ = true;
    suspend();
    std::abort();  // 结束的 fiber 不该被再次恢复（resume 也会拦）
  }

 private:
  // 新栈的初始形态必须和 swap_context 的恢复路径逐字节对上（Win64）：
  //   [xmm 160][r15..rbp 64][trampoline 地址] [入口返回地址占位] [shadow 32]
  //                                    ^ top        ^ rsp(入口)      ^ [rsp+8, rsp+40)
  // 恢复时先弹 xmm、再弹 8 个 GPR、最后 ret 吃走 [top]，于是正好落在
  // trampoline 上，且入口 rsp = top+8，% 16 == 8（和刚被 call 过一样）。
  //
  // 两个容易混淆的槽：
  //   - [top]        ：swap_context 的 ret 用它当跳转目标（trampoline 地址），
  //                    ret 本身就会把它从栈上吃掉，它不是任何人的返回地址槽；
  //   - [top+8]      ：trampoline 入口的 [rsp]，也就是"假如它被 call 过"时
  //                    返回地址会躺的位置。trampoline 是 [[noreturn]]，永远
  //                    不 ret，所以这里填 0 占位——但这个槽必须存在：入口
  //                    rsp % 16 == 8 的对齐状态就是靠它撑出来的。
  //   - [top+16..]   ：32 字节 shadow space，是"调用方替 callee 预留的草稿区"，
  //                    归 trampoline 支配（可溢出 RCX/RDX/R8/R9 或当暂存用）。
  //                    它不是返回地址槽：ret 只从 [rsp] 取地址，往 shadow 里
  //                    写"返回地址"不会让任何 ret 跳过去。内容按 ABI 无定义，
  //                    这里 memset 归零只是为了让首帧可复现、便于调试。
  void prepare_stack() {
    constexpr std::size_t saved_gprs =
#if defined(_WIN64)
        8 * sizeof(void*);
#else
        6 * sizeof(void*);
#endif
    constexpr std::size_t saved_xmm =
#if defined(_WIN64)
        160;
#else
        0;
#endif
    constexpr std::size_t saved = saved_gprs + saved_xmm;
    constexpr std::size_t entry_bytes = sizeof(void*) +
  #if defined(_WIN64)
      32;
  #else
      0;
  #endif

    // ret 消耗跳转槽后，入口仍需返回地址占位和 Win64 shadow space。
    auto* top = reinterpret_cast<std::byte*>(
      (reinterpret_cast<std::uintptr_t>(stack_.high) - sizeof(void*) -
       entry_bytes) &
        ~std::uintptr_t{15});
    auto* frame = top - saved;
    std::memset(frame, 0, saved + sizeof(void*) + entry_bytes);
    *reinterpret_cast<void**>(top) =
        reinterpret_cast<void*>(&detail::fiber_trampoline);
    ctx_.sp = frame;

    // 新协程的 FP 控制状态继承创建者当前值（不是 0）：
    //   MXCSR = 0 意味着异常全开、DAZ/FTZ 全关；x87 CW = 0 意味着单精度、
    //   异常不屏蔽——首次恢复就会带着错误状态跑。这里用创建时的真实值播种。
    std::uint32_t mxcsr = 0;
    std::uint16_t fpucw = 0;
    __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr));
    __asm__ __volatile__("fnstcw %0" : "=m"(fpucw));
    ctx_.mxcsr = mxcsr;
    ctx_.x87_cw = fpucw;
  }

  context ctx_{};
  context caller_{};
  entry_type entry_;
  stack_arena::bounds stack_{};
  bool finished_ = false;
  std::exception_ptr failure_{};
};

namespace detail {

[[noreturn]] inline void fiber_trampoline() {
  fiber* self = running_fiber;
  self->run();   // 跑用户函数 → 标记结束 → 切回 resumer
  std::abort();  // run() 不会返回
}

}  // namespace detail

// 从当前协程让出；不在协程里时什么也不做。
inline void yield() {
  if (running_fiber != nullptr) running_fiber->suspend();
}

inline fiber* current_fiber() noexcept { return running_fiber; }

// 当前 rsp。always_inline 之后读到的是调用者这一层的栈指针，
// 用来验证“协程真的跑在自己的栈上”。
__attribute__((always_inline)) inline void* stack_pointer() noexcept {
  void* sp = nullptr;
#if defined(__x86_64__)
  __asm__ __volatile__("movq %%rsp, %0" : "=r"(sp));
#else
#error "stack_pointer: only x86-64 is implemented"
#endif
  return sp;
}

}  // namespace meta_execution
