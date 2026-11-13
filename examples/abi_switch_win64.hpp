#pragma once

// ============================================================================
// abi_switch_win64 —— Win64 换栈器的 C++ 侧：布局契约、启动帧、FP 控制字
//
// 汇编在 assemble/abi_switch_win64.s（不是内联 asm，也不是 naked 函数）：
//   void abi_switch_swap(void* from_frame, void* to_frame);   // rcx / rdx
//   [[noreturn]] void abi_switch_exit();                     // 入口函数返回后的落点
//
// 固定帧布局（相对 frame()，与 meta_abi_range.md 第 3、4 节逐项对应）：
//   +0   resume_pc      首次切入时 ret 吃掉的字（入口地址）
//   +8   exit_pc        入口函数正常返回时落到的假返回地址
//   +16  shadow[32]     Win64 home area
//   +48  xmm6..xmm15    10×16；+208 mxcsr，+212 x87 CW
//   +224 rbx rbp rdi rsi r12 r13 r14 r15（各 8 字节）
//   +288 sp             保存的 rsp，指向"恢复后要 ret 的那个返回地址"
//
// 偏移从 meta_abi_range 算出来并用 static_assert 钉死；汇编里写的是同一组数字
// （COFF 汇编没法拿 C++ 常量做操作数替换），两边是否真的一致由
// examples/abi_switch_smoke.cpp 在运行期核对：它在 B 的上下文里直接读 A 被挂起后的
// 固定帧，逐槽检查 xmm / GPR / sp 是否落在上表的位置。
//
// 三条硬约定（meta_abi_range.md 第 4、5 节）：
//   1) 切换器全程不 push：rsp 保存时正好指向自己的返回地址槽，恢复后直接 ret 回原位。
//   2) 存的是 FP 块 + GPR 块全量；少存 xmm6..xmm15 是 ABI 破坏（它们 callee-saved）。
//   3) 零点环境不能直接 ldmxcsr：mxcsr=0 等于解除全部浮点异常屏蔽，bootstrap 必须把
//      当前线程的合法控制字抄进目标 fp 槽。
// ============================================================================

#if !defined(_WIN32)
#error "abi_switch_win64 只支持 Windows x64（rcx/rdx 调用约定、xmm6..15 callee-saved）"
#endif

#include "meta_abi_range.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace abi_switch {

using meta_abi::x86_control;

// 下面全是"相对 frame()"的偏移，与 N 无关：拿一个参照实例把常量算出来。
using reference_range = meta_abi::meta_abi_range_win_x64<1024>;

inline constexpr std::size_t fp_rel = reference_range::fp_offset - reference_range::frame_offset;     // 48
inline constexpr std::size_t gpr_rel = reference_range::gpr_offset - reference_range::frame_offset;  // 224
inline constexpr std::size_t sp_rel = reference_range::sp_offset - reference_range::frame_offset;    // 288
// 控制字在 fp 块里不是从 0 开始：前面是 10 个 xmm 槽（160 字节）。
inline constexpr std::size_t fpctl_rel = fp_rel + offsetof(meta_abi::win_x64_fp_state, control);  // 208
inline constexpr std::size_t mxcsr_rel = fpctl_rel + offsetof(x86_control, mxcsr);                // 208
inline constexpr std::size_t x87_rel = fpctl_rel + offsetof(x86_control, x87_control_word);       // 212

// 这些数字同时出现在 assemble/abi_switch_win64.s 里。布局一变，这里先炸。
static_assert(reference_range::frame_bytes == 304, "Win x64 固定帧应为 304 字节");
static_assert(reference_range::frame_offset % 16 == 0, "frame() 必须 16 字节对齐（xmm 槽）");
static_assert(fp_rel == 48 && gpr_rel == 224 && sp_rel == 288);
static_assert(mxcsr_rel == 208 && x87_rel == 212);
static_assert(reference_range::ret_address_offset - reference_range::frame_offset == 0);     // resume_pc
static_assert(reference_range::return_address_offset - reference_range::frame_offset == 8);  // exit_pc
static_assert(offsetof(meta_abi::win_x64_bootstrap, shadow_space) == 16);
static_assert(sizeof(reference_range) == sizeof(void*), "meta_abi_range 是非拥有视图，只存一个指针");

// ---- 汇编侧的符号（定义在 assemble/abi_switch_win64.s）-----------------------
extern "C" void abi_switch_swap(void* from_frame, void* to_frame);
extern "C" [[noreturn]] void abi_switch_exit();

// C++ 侧的切换入口：参数由 Win64 调用约定装好。
inline void swap(void* from_frame, void* to_frame) {
  abi_switch_swap(from_frame, to_frame);
}

// ---- FP 控制字：读 / 写 ----------------------------------------------------
inline x86_control current_control() {
  x86_control control{};
  __asm__ volatile("stmxcsr %0" : "=m"(control.mxcsr));
  __asm__ volatile("fnstcw %0" : "=m"(control.x87_control_word));
  return control;
}

inline void apply_control(x86_control const& control) {
  __asm__ volatile("ldmxcsr %0" : : "m"(control.mxcsr));
  __asm__ volatile("fldcw %0" : : "m"(control.x87_control_word));
}

// ---- 退出路径 --------------------------------------------------------------
using finish_fn = void (*)();

// 默认处理：入口函数返回了就说明这一轮没人把控制权交回调度器。
[[noreturn]] inline void default_finish() {
  std::fputs("abi_switch: 协程入口函数返回了，但没有切回调度器\n", stderr);
  std::abort();
}

// 完成处理函数槽：assemble/abi_switch_win64.s 里的 abi_switch_exit 用 RIP 相对寻址
// 间接调用，所以要固定符号名。必须 used：只被汇编引用时编译器也不能把这个对象丢掉。
// 也不能写成 `extern "C" inline finish_fn x = ...` —— GCC 会警告 "initialized and
// declared 'extern'"，而且属性放错位置时符号根本发不出来（链接期 undefined reference）。
extern "C" {
__attribute__((used)) inline finish_fn abi_switch_finish_slot = &default_finish;
}

inline void set_finish(finish_fn fn) {
  abi_switch_finish_slot = fn == nullptr ? &default_finish : fn;
}

// ---- 启动帧：sp / resume_pc / exit_pc / 合法 FP 控制字 ----------------------
template <class Range>
inline void bootstrap(Range& range, void (*entry)()) {
  range.initialize();  // 整块清零：gpr / xmm 初始值全 0，帧里的 shadow space 也归零

  const auto resume_pc = reinterpret_cast<std::uintptr_t>(entry);
  const auto exit_pc = reinterpret_cast<std::uintptr_t>(&abi_switch_exit);
  const auto sp = reinterpret_cast<std::uintptr_t>(range.initial_sp());  // 指向 resume_pc 那个字

  std::memcpy(range.ret_address(), &resume_pc, sizeof resume_pc);
  std::memcpy(range.return_address(), &exit_pc, sizeof exit_pc);
  std::memcpy(range.sp(), &sp, sizeof sp);

  const auto control = current_control();  // 不能留全零：mxcsr=0 会解除全部浮点异常屏蔽
  std::memcpy(range.fp_control(), &control, sizeof control);
}

}  // namespace abi_switch
