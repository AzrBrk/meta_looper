#pragma once

// ============================================================================
// meta_execution_context: coroutine execution contexts that switch to a real
// function stack through naked functions.
//
// A naked swap_context (callee-saved registers + rsp only), one
// bump-allocated stack and an entry trampoline are all this needs - no
// Boost.Context. The trade-off is that it is platform-locked: frame layout,
// callee-saved set, entry alignment and exception unwinding are all bound to
// the ABI. Only x86-64 Win64 and SysV are implemented; every other target
// #errors out instead of degrading silently.
//
// Fiber frames come from stack_arena (bump, never freed, whole-arena reset), so
// there is no heap allocation and no per-frame malloc churn.
//
// Usage:
//   std::byte memory[256 * 1024];
//   meta_execution::stack_arena arena{memory, sizeof(memory)};
//   meta_execution::fiber f{arena, 64 * 1024, [](meta_execution::fiber& self) {
//     ...
//     meta_execution::yield();     // an ordinary call at any depth suspends
//   }};
//   while (f.resume()) { ... }     // false once the fiber has finished
//
// Design notes: meta_execution_context.md.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <utility>

namespace meta_execution {

// One suspended execution point: rsp plus floating-point control state.
// FP state is stored separately because both ABIs treat it as callee-saved
// while the XMM registers (Win64 only) are already spilled inside the frame
// this rsp points at. Storing the whole MXCSR, sticky status flags included, is
// harmless: restoring old flags cannot raise new exceptions.
struct context {
  void* sp = nullptr;
  std::uint32_t mxcsr = 0;
  std::uint16_t x87_cw = 0;
};
// The naked assembly below addresses these fields by fixed offset; any layout
// change is an ABI bug.
static_assert(offsetof(context, sp) == 0);
static_assert(offsetof(context, mxcsr) == 8);
static_assert(offsetof(context, x87_cw) == 12);

namespace detail {

// ---------------------------------------------------------------------------
// swap_context(from, to)
//
// naked: the compiler emits no prologue/epilogue, so at entry the argument
// registers still hold the caller's values (Win64: rcx/rdx, SysV: rdi/rsi).
// The save order is the reverse of the restore order:
//   push rbp, rbx, rsi, rdi, r12..r15   (plus xmm6..xmm15 on Win64)
//   [rsp] -> *from, rsp = *to, pop the same registers, ret.
// The initial form of a fresh stack must match this restore path byte for
// byte; see fiber::prepare_stack.
// ---------------------------------------------------------------------------
#if defined(_WIN64) && (defined(__GNUC__) || defined(__clang__))

// Win64: xmm6..xmm15 are callee-saved, so both fibers must spill them or they
// clobber each other's floating-point state.
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
      // FP control state first, while rcx still holds from: stmxcsr stores the
      // whole MXCSR and fnstcw reads the x87 control word without waiting.
      "stmxcsr 8(%rcx)\n\t"
      "fnstcw 12(%rcx)\n\t"
      "movq %rsp, (%rcx)\n\t"
      "movq (%rdx), %rsp\n\t"
      // On the target stack, restore its FP control state right away (rdx = to
      // is still live).
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

// SysV AMD64: rdi = from, rsi = to; six callee-saved GPRs and no callee-saved
// XMM. MXCSR control bits and the x87 control word are callee-saved as well.
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

// Currently running fiber, or nullptr on the main flow.
class fiber;
inline thread_local fiber* running_fiber = nullptr;

namespace detail {

// First landing point when a fresh stack is ret'ed into: an ordinary C++
// function whose entry rsp already satisfies the ABI "just called" alignment
// (see prepare_stack), so it may run a normal prologue. Defined after fiber
// because it needs the complete type.
[[noreturn]] void fiber_trampoline();

}  // namespace detail

// ---------------------------------------------------------------------------
// stack_arena: bump-allocates fiber stacks downward from one block; nothing is
// freed, and reset() rewinds the whole arena.
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

  // Stacks are 64-byte aligned: the ABI asks for 16, the rest is headroom for
  // SIMD and a future guard page. A canary sits at the low end so overruns
  // surface immediately.
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
// fiber: one real stack plus one save point. resume() enters or continues it,
// and yield() suspends from any call depth.
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

  // Returns true while the fiber keeps running after this resume. An exception
  // thrown inside the fiber is rethrown here, on the caller's stack.
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

  // Suspend from any call depth and return to the resumer.
  void suspend() { detail::swap_context(&ctx_, &caller_); }

  bool finished() const noexcept { return finished_; }
  std::byte* stack_low() const noexcept { return stack_.low; }
  std::byte* stack_high() const noexcept { return stack_.high; }
  bool owns(std::byte const* pointer) const noexcept {
    return pointer >= stack_.low && pointer < stack_.high;
  }

  // Called by the trampoline: runs the user function, marks the fiber finished
  // and suspends forever. Exceptions are caught here (a naked frame has no unwind
  // info, so letting them escape is UB); the resumer rethrows on its own stack.
  [[noreturn]] void run() {
    try {
      entry_(*this);
    } catch (...) {
      failure_ = std::current_exception();
    }
    finished_ = true;
    suspend();
    std::abort();  // A finished fiber must never be resumed (resume() refuses too).
  }

 private:
  // The initial stack image must match swap_context's restore path byte for
  // byte (Win64):
  //   [xmm 160][r15..rbp 64][trampoline address][entry return slot][shadow 32]
  //                         ^ top                 ^ rsp at entry
  // Restoring pops the xmm registers, then the eight GPRs, then ret consumes
  // [top], so control lands on the trampoline with rsp = top + 8 and
  // rsp % 16 == 8 - exactly as if it had been called.
  //
  // Three slots that are easy to confuse:
  //   - [top]     : the ret target (the trampoline address). ret consumes it,
  //                 so it is nobody's return-address slot.
  //   - [top+8]   : [rsp] at trampoline entry, where a callee's return address
  //                 would sit. The trampoline is [[noreturn]] and never rets,
  //                 but the slot must exist: it is what makes rsp % 16 == 8.
  //   - [top+16..]: the 32-byte Win64 shadow space owned by the trampoline (it
  //                 may spill RCX/RDX/R8/R9 there). ret never reads it, so the
  //                 contents are ABI-undefined; zeroing keeps first frames
  //                 reproducible for debugging.
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

    // ret consumes the jump slot; entry still needs a return-address slot and,
    auto* top = reinterpret_cast<std::byte*>(
      (reinterpret_cast<std::uintptr_t>(stack_.high) - sizeof(void*) -
       entry_bytes) &
        ~std::uintptr_t{15});
    auto* frame = top - saved;
    std::memset(frame, 0, saved + sizeof(void*) + entry_bytes);
    *reinterpret_cast<void**>(top) =
        reinterpret_cast<void*>(&detail::fiber_trampoline);
    ctx_.sp = frame;

    // A new fiber inherits the creator's current FP control state, never 0:
    // MXCSR = 0 enables every exception and clears DAZ/FTZ; x87 CW = 0 means
    // single precision with unmasked exceptions. Seeding with the real current
    // values keeps the first resume from running with a broken state.
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
  self->run();   // user function -> mark finished -> switch back to the resumer
  std::abort();  // run() never returns
}

}  // namespace detail

// Suspend the current fiber; a no-op outside a fiber.
inline void yield() {
  if (running_fiber != nullptr) running_fiber->suspend();
}

inline fiber* current_fiber() noexcept { return running_fiber; }

// Current rsp. With always_inline this reads the caller's stack pointer,
// which is how the smoke test proves a fiber really runs on its own stack.
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
