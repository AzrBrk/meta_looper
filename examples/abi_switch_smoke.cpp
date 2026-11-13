// abi_switch_smoke —— 验证 assemble/abi_switch_win64.s 的换栈器
//
// 汇编全在 assemble/：
//   assemble/abi_switch_win64.s        abi_switch_swap / abi_switch_exit
//   assemble/abi_switch_probe_win64.s  abi_switch_probe / abi_switch_trash_and_swap
//
// 这个文件只负责编排和检查。为什么要专门写汇编探针：只测 C++ 局部变量不可靠——
// -O2 会把常量比较折叠掉，编译器也不一定真的用 xmm6..xmm15。探针把 18 个非易失
// 寄存器（10 个 xmm + 8 个 GPR）填成魔数、换一次栈再核对，中间没有编译器指令。
//
// 三个方向分别验：
//   1) 存：B 在自己的上下文里读 A 被挂起后的固定帧 —— xmm / GPR 槽必须是魔数，
//      sp 槽必须指向 A 自己那段栈；
//   2) 取：A 恢复后 xmm6..xmm15 与 8 个非易失 GPR 必须整块回来。B 在切回去之前
//      会先把这些寄存器砸成垃圾，所以"存了但忘了恢复"也会被抓到；
//   3) FP 控制字按上下文隔离：B 打开 FTZ，A 回来后仍是自己那份；main 也一样。

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "abi_switch_win64.hpp"
#include "meta_abi_range.hpp"

using namespace meta_abi;

// 测试专用汇编（assemble/abi_switch_probe_win64.s）
extern "C" std::uint64_t abi_switch_probe(void* from_frame, void* to_frame);
extern "C" void abi_switch_trash_and_swap(void* from_frame, void* to_frame);

namespace {

using range_t = meta_abi_range_win_x64<64 * 1024>;
using main_range_t = meta_abi_range_win_x64<512>;

// 探针魔数基址，必须与 assemble/abi_switch_probe_win64.s 里的 0x600000000000 一致。
inline constexpr std::uint64_t magic_base = 0x6000'0000'0000ULL;

alignas(16) std::array<std::byte, range_t::storage_bytes> stack_a_storage{};
alignas(16) std::array<std::byte, range_t::storage_bytes> stack_b_storage{};
alignas(16) std::array<std::byte, main_range_t::storage_bytes> main_storage{};

range_t co_a{stack_a_storage.data()};
range_t co_b{stack_b_storage.data()};
main_range_t co_main{main_storage.data()};

int frame_memory_ok = 0;
int control_ok = 0;
int peer_done = 0;
std::uint64_t roundtrip_diff = ~std::uint64_t{0};

std::uint64_t load_u64(std::byte const* p) {
  std::uint64_t v = 0;
  std::memcpy(&v, p, sizeof v);
  return v;
}

// 读 A 被挂起后的固定帧：xmm 槽 = base+i（高 64 位为 0，因为 movq 会清零），
// GPR 槽按 win_x64_gpr_state 里的顺序 = base+10+i，sp 槽落在 A 的栈区间里。
bool frame_holds_magics(range_t const& range) {
  std::byte* frame = range.frame();
  for (std::size_t i = 0; i < 10; ++i) {
    if (load_u64(frame + abi_switch::fp_rel + 16 * i) != magic_base + i) {
      return false;
    }
    if (load_u64(frame + abi_switch::fp_rel + 16 * i + 8) != 0) {
      return false;
    }
  }
  constexpr std::size_t gpr_slots[8] = {
      offsetof(win_x64_gpr_state, rbx), offsetof(win_x64_gpr_state, rbp),
      offsetof(win_x64_gpr_state, rdi), offsetof(win_x64_gpr_state, rsi),
      offsetof(win_x64_gpr_state, r12), offsetof(win_x64_gpr_state, r13),
      offsetof(win_x64_gpr_state, r14), offsetof(win_x64_gpr_state, r15)};
  for (std::size_t i = 0; i < 8; ++i) {
    if (load_u64(frame + abi_switch::gpr_rel + gpr_slots[i]) != magic_base + 10 + i) {
      return false;
    }
  }
  const auto sp = static_cast<std::uintptr_t>(load_u64(frame + abi_switch::sp_rel));
  return sp >= reinterpret_cast<std::uintptr_t>(range.data()) &&
         sp < reinterpret_cast<std::uintptr_t>(range.stack_top());
}

void probe_entry();
void peer_entry();

}  // namespace

namespace {

// A：跑探针（内部换到 B，B 查完内存再砸烂寄存器换回来），核对 FP 控制字，
// 然后把控制权交回 main。
void probe_entry() {
  const auto baseline = abi_switch::current_control();
  roundtrip_diff = abi_switch_probe(co_a.frame(), co_b.frame());
  control_ok = abi_switch::current_control().mxcsr == baseline.mxcsr ? 1 : 0;
  abi_switch::swap(co_a.frame(), co_main.frame());
  std::abort();  // 不会再回到 A
}

// B：第一次进来时 A 正挂在探针里，它的固定帧刚被存过一份魔数。
// 查完内存后把自己的 MXCSR 打开 FTZ（A 回来时若被污染，control_ok 会挂），
// 再把非易失寄存器砸成垃圾切回 A —— 恢复这一半也必须是确定性的。
void peer_entry() {
  frame_memory_ok = frame_holds_magics(co_a) ? 1 : 0;
  auto control = abi_switch::current_control();
  control.mxcsr |= 0x8000u;  // FTZ
  abi_switch::apply_control(control);
  peer_done = 1;
  abi_switch_trash_and_swap(co_b.frame(), co_a.frame());
  std::abort();  // 不会再回到 B
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const auto main_control = abi_switch::current_control();

  abi_switch::bootstrap(co_a, &probe_entry);
  abi_switch::bootstrap(co_b, &peer_entry);
  abi_switch::swap(co_main.frame(), co_a.frame());  // main → A(探针) → B → A → main

  const bool ok = peer_done == 1 && frame_memory_ok == 1 && roundtrip_diff == 0 &&
                  control_ok == 1 &&
                  abi_switch::current_control().mxcsr == main_control.mxcsr;
  std::printf("frame memory ok = %d (B 读到 A 的 xmm/gpr/sp 槽)\n", frame_memory_ok);
  std::printf("roundtrip diff  = %llu (0 = A 的 xmm6..15 与 8 个 GPR 整块回来)\n",
              static_cast<unsigned long long>(roundtrip_diff));
  std::printf("control ok      = %d (A 的 MXCSR 没被 B 的 FTZ 污染)\n", control_ok);
  std::printf("peer done       = %d\n", peer_done);
  if (!ok) {
    std::puts("abi_switch_smoke: FAILED");
    return 1;
  }
  std::puts("abi_switch_smoke: ok");
  return 0;
}
