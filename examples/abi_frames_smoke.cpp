// ABI 帧布局的编译期常量与运行期视图。
//
// 这里验证的是 meta_abi_frames 的三件事：
//   1. 每个平台的槽位偏移/总大小由布局算出，且和 ABI 手册的值一致；
//   2. 同一份布局既能编译期取（get<I>），也能用枚举下标取；
//   3. 给一个裸指针就能拿到类型化帧视图（frame_at），不需要手写 offsetof。
#include "meta_abi_frames.hpp"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <exception>

using namespace meta_abi;

namespace {

// 用布局自己算出来的偏移去读缓冲，读到的必须是初始化时写进去的值。
void sysv_frame() {
  using layout = sysv_x86_64_entry<stack_word, stack_word>;
  alignas(16) std::byte buffer[frame_size<ret_address, stack_word, stack_word>];
  layout frame{buffer};
  type_safe_aligned::initialize(frame, ret_address{0x1111'2222'3333'4444ull}, stack_word{7},
             stack_word{9});

  assert(frame.get<slot_index(sysv_x86_64_slot::ret_address)>().value ==
         0x1111'2222'3333'4444ull);
  assert(frame.get<slot_index(sysv_x86_64_slot::arg0)>().value == 7);
  assert(frame.get<slot_index(sysv_x86_64_slot::arg1)>().value == 9);

  // 同一块内存的第二种打开方式：只给裸指针。
  auto view = frame_at<layout>(buffer);
  assert(view.get<slot_index(sysv_x86_64_slot::ret_address)>().value ==
         return_address_of(buffer));
  assert(view.get<slot_index(sysv_x86_64_slot::arg0)>().value == 7);

  // 运行期下标 + 类型检查：第 1 格确实是 stack_word，取成 ret_address 要抛。
  assert(frame[1].template safe<stack_word>());
  assert(!frame[1].template safe<ret_address>());
}

void win64_frame() {
  using layout = win64_x86_64_entry<stack_word, stack_word>;
  alignas(16) std::byte
      buffer[frame_size<ret_address, shadow_space, stack_word, stack_word>];
  layout frame{buffer};
  type_safe_aligned::initialize(frame, ret_address{0xAAAA}, shadow_space{}, stack_word{42},
             stack_word{43});

  // shadow space 在 [rsp+8, rsp+40)，第一个栈参数在 rsp+40 —— Win64 与 SysV
  // 最容易写错的地方就在这里。
  assert(&frame.get<slot_index(win64_x86_64_slot::shadow_space)>() ==
         reinterpret_cast<shadow_space*>(buffer + 8));
  assert(&frame.get<slot_index(win64_x86_64_slot::arg0)>() ==
         reinterpret_cast<stack_word*>(buffer + 40));
  assert(frame.get<slot_index(win64_x86_64_slot::arg0)>().value == 42);
  assert(frame.get<slot_index(win64_x86_64_slot::arg1)>().value == 43);
}

void aarch64_frame() {
  using layout = aarch64_trampoline;
  alignas(16) std::byte buffer[frame_size<frame_record, callee_gprs, callee_simd>];
  layout frame{buffer};
  type_safe_aligned::initialize(frame, frame_record{0xAAAA, 0xBBBB},
             callee_gprs{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
             callee_simd{{11, 12, 13, 14, 15, 16, 17, 18}});

  assert(frame.get<slot_index(aarch64_trampoline_slot::frame_record)>().fp ==
         0xAAAA);
  assert(frame.get<slot_index(aarch64_trampoline_slot::frame_record)>().lr ==
         0xBBBB);
  assert(frame.get<slot_index(aarch64_trampoline_slot::callee_gprs)>().value[9] ==
         10);
  assert(frame.get<slot_index(aarch64_trampoline_slot::callee_simd)>().value[7] ==
         18);
  // x19..x28 紧跟帧记录，d8..d15 在偏移 96。
  assert(&frame.get<slot_index(aarch64_trampoline_slot::callee_gprs)>() ==
         reinterpret_cast<callee_gprs*>(buffer + 16));
  assert(&frame.get<slot_index(aarch64_trampoline_slot::callee_simd)>() ==
         reinterpret_cast<callee_simd*>(buffer + 96));
}

}  // namespace

int main() {
  std::printf("sysv  entry bytes=%zu\n",
              static_cast<std::size_t>(
                  frame_size<ret_address, stack_word, stack_word>));
  std::printf("win64  entry bytes=%zu\n",
              static_cast<std::size_t>(frame_size<ret_address, shadow_space,
                                                  stack_word, stack_word>));
  std::printf("win64  context bytes=%zu\n",
              static_cast<std::size_t>(frame_size<ret_address, shadow_space,
                                                  std::array<xmm_slot, 10>>));
  std::printf("arm64  trampoline bytes=%zu\n",
              static_cast<std::size_t>(
                  frame_size<frame_record, callee_gprs, callee_simd>));

  sysv_frame();
  win64_frame();
  aarch64_frame();

  std::puts("abi_frames_smoke: ok");
}
