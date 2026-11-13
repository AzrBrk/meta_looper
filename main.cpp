// Windows x64 换栈 demo —— 这里只放调度逻辑。
//
//   * 切换器本体（字节布局、xmm/gpr 保存、退出 trampoline）在
//     examples/abi_switch_win64.hpp；
//   * 帧布局与寄存器保存的验证在 examples/abi_switch_smoke.cpp；
//   * 这个文件只演示"两个上下文互相让出 + 最后交回 main"。

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "examples/abi_switch_win64.hpp"
#include "meta_abi_range.hpp"

using namespace meta_abi;

namespace {

using range_t = meta_abi_range_win_x64<64 * 1024>;
// main 只需要放"被换出去时"的保存区，不需要协程栈，320 字节够（frame 304 + 16）。
using main_range_t = meta_abi_range_win_x64<512>;

constexpr int kRounds = 10;

alignas(16) std::array<std::byte, range_t::storage_bytes> stack_a_storage{};
alignas(16) std::array<std::byte, range_t::storage_bytes> stack_b_storage{};
alignas(16) std::array<std::byte, main_range_t::storage_bytes> main_storage{};

range_t co_a{stack_a_storage.data()};
range_t co_b{stack_b_storage.data()};
main_range_t co_main{main_storage.data()};

int b_done = 0;

void task_a();  // 打印 + 让出给 B；最后一轮之后不再回来
void task_b();  // 打印 + 让出给 A；最后一轮收尾交回 main

}  // namespace

// 入口函数不该正常返回：它一返回就说明这一轮没人把控制权交回调度器。
extern "C" [[noreturn]] void demo_finished() {
  std::printf("demo: 入口函数返回了，但没人交回 main\n");
  std::abort();
}

namespace {

void task_a() {
  for (int i = 1; i <= kRounds; ++i) {
    std::printf("A: hello world #%d\n", i);
    abi_switch::swap(co_a.frame(), co_b.frame());
  }
}

void task_b() {
  for (int i = 1; i <= kRounds; ++i) {
    std::printf("B: hello world #%d\n", i);
    if (i == kRounds) {
      break;  // 最后一轮不再切回 A，留给下面的收尾
    }
    abi_switch::swap(co_b.frame(), co_a.frame());
  }
  b_done = 1;
  abi_switch::swap(co_b.frame(), co_main.frame());
  assert(false);  // 不会再回到 B
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // 关缓冲，否则换栈后输出顺序看不出来

  abi_switch::set_finish(&demo_finished);
  abi_switch::bootstrap(co_a, &task_a);
  abi_switch::bootstrap(co_b, &task_b);

  // 第一次切入 A：main 的 rsp 存进 co_main 的 sp 槽，
  // 之后就冻结在 B 把控制权交回来的那一刻。
  abi_switch::swap(co_main.frame(), co_a.frame());

  assert(b_done == 1);
  std::printf("main: A/B 各 %d 轮，已回到主栈\n", kRounds);
  return 0;
}
