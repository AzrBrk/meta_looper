#pragma once

// ============================================================================
// meta_abi_frames —— 把 ABI 的栈帧写成 type_safe_aligned_range 的布局
//
// 目的：CPU 调用栈本来是“裸内存”，只能靠手算 offsetof 或手写汇编去读。
// 这里把每一段（返回地址、shadow space、栈参数、callee-saved 寄存器、
// AArch64 的 fp/lr 帧记录）都写成一个有类型的槽，于是：
//
//   - 布局、偏移、总大小由 type_safe_aligned_iterator 在编译期算出来，
//     不需要任何 sizeof/offsetof 手写常量；
//   - 运行期拿一个 sp，就能 frame_view<Layout>(sp) 得到一个类型化视图，
//     range.get<slot_index(enum)>() 直接取出那一段；
//   - 这只是“布局 + 视图”，不分配内存、不做任何拷贝（Alignment=0 表示
//     每个槽按自身 alignof 排；x86-64 的栈槽是 8 字节，不要用 <16>）。
//
// 平台不是靠宏去“猜”的：布局本身是普通类型，三个平台的定义可以同时存在、
// 同时编译，只有 frame_view 的基址来自当前进程的栈指针时才和平台相关。
// ============================================================================

#include "type_safe_aligned_iterator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace meta_abi {

using namespace type_safe_aligned;

using word = std::uint64_t;

// ---- 槽位类型 --------------------------------------------------------------
// 每个槽只是一个“有大小和对齐的类型”，真正决定位置的是它在布局里的次序。

// 返回地址：[rsp] 处由 call 压入的那个值。
struct ret_address {
  word value;
};

// Win64 的 shadow space：调用者为前四个整参数预留的 32 字节。
struct shadow_space {
  std::byte bytes[32];
};

// 一个 8 字节栈槽（第 5 个及以后的整参数、或者调用者自己压的值）。
struct stack_word {
  word value;
};

// 16 字节的 XMM 槽：Win64 的 xmm6..xmm15 是 callee-saved。
struct xmm_slot {
  alignas(16) std::byte bytes[16];
};

// AArch64 的帧记录：x29(fp) / x30(lr)。
struct frame_record {
  word fp;
  word lr;
};

// AArch64 的 callee-saved：x19..x28。
struct callee_gprs {
  std::array<word, 10> value;
};

// AArch64 的 callee-saved：d8..d15。
struct callee_simd {
  std::array<word, 8> value;
};

// ---- 布局 ------------------------------------------------------------------

// SysV AMD64：进入被调函数时 [rsp] 是返回地址，之后是第 7 个及以后的参数。
template <class... Args>
using sysv_x86_64_entry = type_safe_aligned_range<0, ret_address, Args...>;

// Win64：进入被调函数时 [rsp] 是返回地址，[rsp+8] 起 32 字节 shadow space，
// 之后才是第 5 个及以后的参数。所以第一个栈参数在偏移 40，不是 8。
template <class... Args>
using win64_x86_64_entry =
    type_safe_aligned_range<0, ret_address, shadow_space, Args...>;

// Win64 上下文保存区：八条 callee-saved GPR 由切换代码自己压栈，XMM6..15
// 需要 16 字节对齐，所以单独开 10 个 xmm_slot。
using win64_x86_64_context =
    type_safe_aligned_range<0, ret_address, shadow_space,
                            std::array<xmm_slot, 10>>;

// AArch64：这是 trampoline/context 的自定义帧，不是默认入口帧 —— 默认入口时
// x30 在寄存器里，这里把它和 x29 一起写进 frame_record。
using aarch64_trampoline =
    type_safe_aligned_range<0, frame_record, callee_gprs, callee_simd>;

// ---- 具名槽位 --------------------------------------------------------------
// 枚举值就是布局里的下标，可以同时喂给 get<I>() 和运行期的 r[i]。

enum class sysv_x86_64_slot : std::size_t {
  ret_address = 0,
  arg0 = 1,
  arg1 = 2,
  arg2 = 3,
};

enum class win64_x86_64_slot : std::size_t {
  ret_address = 0,
  shadow_space = 1,
  arg0 = 2,
  arg1 = 3,
  arg2 = 4,
};

enum class win64_context_slot : std::size_t {
  ret_address = 0,
  shadow_space = 1,
  xmm6_15 = 2,
};

enum class aarch64_trampoline_slot : std::size_t {
  frame_record = 0,  // x29/x30
  callee_gprs = 1,   // x19..x28
  callee_simd = 2,   // d8..d15
};

template <class Slot>
  requires std::is_enum_v<Slot>
constexpr std::size_t slot_index(Slot slot) noexcept {
  return static_cast<std::size_t>(slot);
}

// ---- 编译期校验 ------------------------------------------------------------
// 偏移全部由布局算出；这里只是把“ABI 要求的值”钉死，改坏一处就编译不过。

template <class... Slots>
inline constexpr std::size_t frame_size = field_size<0, Slots...>();

static_assert(frame_size<ret_address> == 8);
static_assert(frame_size<ret_address, stack_word, stack_word> == 24);
// 8 返回地址 + 32 shadow + 8 + 8 = 56（早期草稿写成 48，是把 shadow 当成 24 了）
static_assert(frame_size<ret_address, shadow_space, stack_word, stack_word> == 56);
static_assert(frame_size<frame_record, callee_gprs, callee_simd> == 160);
static_assert(frame_size<ret_address, shadow_space, std::array<xmm_slot, 10>> == 208);

using sysv_layout = aligned_offset_stream<0, ret_address, stack_word, stack_word>;
static_assert(sysv_layout::template at<0>::value == 0);
static_assert(sysv_layout::template at<1>::value == 8);
static_assert(sysv_layout::template at<2>::value == 16);

using win64_layout =
    aligned_offset_stream<0, ret_address, shadow_space, stack_word, stack_word>;
static_assert(win64_layout::template at<0>::value == 0);
static_assert(win64_layout::template at<1>::value == 8);   // shadow space
static_assert(win64_layout::template at<2>::value == 40);  // 第一个栈参数
static_assert(win64_layout::template at<3>::value == 48);

using arm64_layout = aligned_offset_stream<0, frame_record, callee_gprs, callee_simd>;
static_assert(arm64_layout::template at<0>::value == 0);
static_assert(arm64_layout::template at<1>::value == 16);
static_assert(arm64_layout::template at<2>::value == 96);

// 枚举下标必须和布局里的次序一致。
static_assert(slot_index(sysv_x86_64_slot::ret_address) == 0);
static_assert(slot_index(sysv_x86_64_slot::arg0) == 1);
static_assert(slot_index(win64_x86_64_slot::ret_address) == 0);
static_assert(slot_index(win64_x86_64_slot::shadow_space) == 1);
static_assert(slot_index(win64_x86_64_slot::arg0) == 2);
static_assert(slot_index(win64_x86_64_slot::arg1) == 3);
static_assert(slot_index(win64_context_slot::xmm6_15) == 2);
static_assert(slot_index(aarch64_trampoline_slot::frame_record) == 0);
static_assert(slot_index(aarch64_trampoline_slot::callee_gprs) == 1);
static_assert(slot_index(aarch64_trampoline_slot::callee_simd) == 2);

// ---- 运行期视图 ------------------------------------------------------------
// 把一段裸内存（通常就是某个 sp）按 Layout 解释成类型化视图。
// 调用者负责保证这块内存确实属于该 ABI 帧、且满足对齐要求。
//   auto frame = frame_at<win64_x86_64_entry<stack_word>>(sp);
//   frame.get<slot_index(win64_x86_64_slot::arg0)>().value;
template <class Layout>
inline Layout frame_at(std::byte* sp) noexcept {
  return Layout{sp};
}

// 帧里的返回地址（第 0 槽），不需要知道其余布局。
inline word& return_address_of(std::byte* sp) noexcept {
  return *reinterpret_cast<word*>(sp);
}

}  // namespace meta_abi
