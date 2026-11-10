// 用 naked swap_context 换出真正的函数栈，然后在里面直接套娃调用、任意点挂起。
//
// 验证六件事：
//   1. 协程真的跑在 arena 给的那块内存上（stack_pointer 落在自己那块栈里）；
//   2. 嵌套的普通函数可以在任意深度挂起，恢复后局部变量原样还在；
//   3. 非易失的 XMM6..15 被换栈代码保存/恢复（Win64 上最容易漏的一段）；
//   4. 编译期的 static_function_series 可以整条流水线跑在协程栈上，
//      中途某个 stage 挂起，恢复后从同一个位置继续 —— 参数不再需要
//      “搬进公共缓冲再按段推进”，段与段之间就是一串真实调用；
//   5. MXCSR 控制位与 x87 控制字按 ABI 的非易失语义隔离到各协程；
//   6. 协程内异常在 resumer 的栈上重抛，不穿过裸换栈帧。
#include "meta_execution_context.hpp"

#include <emmintrin.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "meta_static_function_series.hpp"

namespace {

// ---------------------------------------------------------------- 1) 两块栈
std::byte* g_stack_a = nullptr;
std::byte* g_stack_b = nullptr;
std::byte* g_stack_a_after = nullptr;

void entry_a(meta_execution::fiber&) {
  g_stack_a = static_cast<std::byte*>(meta_execution::stack_pointer());
  meta_execution::yield();
  g_stack_a_after = static_cast<std::byte*>(meta_execution::stack_pointer());
}

void entry_b(meta_execution::fiber&) {
  g_stack_b = static_cast<std::byte*>(meta_execution::stack_pointer());
  meta_execution::yield();
}

// ------------------------------------------------- 2/3) 嵌套调用 + XMM 存活
int g_depth3_local_before = 0;
int g_depth3_local_after = 0;
double g_simd_before = 0;
double g_simd_after = 0;

void stage3() {
  int local = 7;
  __m128d keep = _mm_set1_pd(3.5);  // 跨挂起点必须是活的
  g_depth3_local_before = local;
  g_simd_before = _mm_cvtsd_f64(keep);
  meta_execution::yield();          // 在最深的一层挂起
  g_depth3_local_after = local;
  g_simd_after = _mm_cvtsd_f64(keep);
}

void stage2(int a) {
  assert(a == 5);
  stage3();
  assert(a == 5);  // stage3 挂起过，stage2 的栈帧完好
}

void stage1(int x) {
  int a = x;
  stage2(a);
  assert(a == 5);
}

void entry_nested(meta_execution::fiber&) { stage1(5); }

// -------------------------------------- 4) static_function_series 跑在协程上
auto make_series() {
  return meta_bind::link_static(
      [](int input) { return input + 1; },
      [](int first, int scale) {
        meta_execution::yield();        // 流水线走到一半挂起
        return first * scale;
      },
      [](int input) { return std::to_string(input); });
}

using series_type = decltype(make_series());

series_type* g_series = nullptr;
int g_series_stage2_done = 0;

void entry_series(meta_execution::fiber&) {
  g_series->execute_feed();
  g_series_stage2_done = 1;
}

// ------------------------------------------------------- 5) 反复往返压力测试
int g_ping = 0;

void entry_ping(meta_execution::fiber&) {
  for (int i = 0; i < 1000; ++i) {
    ++g_ping;
    meta_execution::yield();
  }
}

// --------------------------------- 6) 协程里的异常：在调用者栈上重抛，不穿过裸帧
void entry_throws(meta_execution::fiber&) {
  throw std::runtime_error("fiber boom");
}

// ------------------------- 7) FP 控制状态：MXCSR 控制位 + x87 CW 的协程隔离
// 两套 x86-64 ABI 都规定 MXCSR 控制位与 x87 控制字是 callee-saved（非易失），
// 所以换栈时必须整组换掉，不能让两个协程互相污染取整/精度模式。
constexpr std::uint32_t kMxcsrRcMask = 0x6000;  // bits 13:14 = 取整模式
constexpr std::uint16_t kX87PcMask = 0x0300;    // bits 8:9   = 精度控制

std::uint32_t read_mxcsr() {
  std::uint32_t value = 0;
  __asm__ __volatile__("stmxcsr %0" : "=m"(value));
  return value;
}
void write_mxcsr(std::uint32_t value) {
  __asm__ __volatile__("ldmxcsr %0" : : "m"(value));
}
std::uint16_t read_x87_cw() {
  std::uint16_t value = 0;
  __asm__ __volatile__("fnstcw %0" : "=m"(value));
  return value;
}
void write_x87_cw(std::uint16_t value) {
  __asm__ __volatile__("fldcw %0" : : "m"(value));
}

std::uint32_t g_main_mxcsr_rc = 0;
std::uint32_t g_fiber_mxcsr_set = 0;
std::uint32_t g_fiber_mxcsr_rc = 0;
std::uint16_t g_main_x87_pc = 0;
std::uint16_t g_fiber_x87_set = 0;
std::uint16_t g_fiber_x87_pc = 0;

void entry_fp(meta_execution::fiber&) {
  // 改成与创建者不同的一种模式，挂起，让主线程验证自己的状态没被污染。
  std::uint32_t mxcsr = read_mxcsr();
  g_fiber_mxcsr_set =
      (mxcsr & kMxcsrRcMask) == kMxcsrRcMask ? 0x0000 : kMxcsrRcMask;
  write_mxcsr((mxcsr & ~kMxcsrRcMask) | g_fiber_mxcsr_set);

  std::uint16_t cw = read_x87_cw();
  g_fiber_x87_set = (cw & kX87PcMask) == kX87PcMask ? 0x0200 : kX87PcMask;
  write_x87_cw((cw & ~kX87PcMask) | g_fiber_x87_set);

  meta_execution::yield();

  // 恢复后必须看到自己被挂起前的那套模式（而不是主线程的）。
  g_fiber_mxcsr_rc = read_mxcsr() & kMxcsrRcMask;
  g_fiber_x87_pc = read_x87_cw() & kX87PcMask;
}

}  // namespace

int main() {
  using namespace meta_execution;

  alignas(64) std::byte memory[512 * 1024];
  stack_arena arena{memory, sizeof(memory)};

  // ---- 1) 两块协程栈互不重叠，且都在 arena 里
  fiber a{arena, 64 * 1024, entry_a};
  fiber b{arena, 64 * 1024, entry_b};

  constexpr std::size_t entry_bytes =
#if defined(_WIN64)
      sizeof(void*) + 32;
#else
      sizeof(void*);
#endif
  auto* launch_slot = static_cast<std::byte*>(nullptr);
  for (std::size_t offset = sizeof(void*); offset <= 64;
       offset += sizeof(void*)) {
    void* target = nullptr;
    std::memcpy(&target, a.stack_high() - offset, sizeof(target));
    if (target == reinterpret_cast<void*>(&detail::fiber_trampoline)) {
      launch_slot = a.stack_high() - offset;
      break;
    }
  }
  assert(launch_slot != nullptr);
  auto* entry_sp = launch_slot + sizeof(void*);
  assert(reinterpret_cast<std::uintptr_t>(entry_sp) % 16 == 8);
  assert(static_cast<std::size_t>(a.stack_high() - entry_sp) >= entry_bytes);
  std::memset(entry_sp, 0xA5, entry_bytes);

  std::printf("arena: %zu bytes\n", arena.size());
  assert(a.resume());
  assert(b.resume());
  assert(g_stack_a != nullptr && g_stack_b != nullptr);
  assert(a.owns(g_stack_a));
  assert(b.owns(g_stack_b));
  assert(!a.owns(g_stack_b));
  std::printf("fiber a sp=%p  in [%p, %p)\n", static_cast<void*>(g_stack_a),
              static_cast<void*>(a.stack_low()),
              static_cast<void*>(a.stack_high()));
  std::printf("fiber b sp=%p  in [%p, %p)\n", static_cast<void*>(g_stack_b),
              static_cast<void*>(b.stack_low()),
              static_cast<void*>(b.stack_high()));
  std::printf("arena used after two stacks: %zu\n", arena.used());

  // ---- 2/3) 深层嵌套 + 跨挂起点的局部变量与 XMM
  fiber nested{arena, 64 * 1024, entry_nested};
  assert(nested.resume());
  assert(g_depth3_local_before == 7);
  assert(g_simd_before == 3.5);
  assert(g_depth3_local_after == 0);  // 还没恢复
  assert(nested.resume() == false);   // 第二次恢复后跑完
  assert(g_depth3_local_after == 7);
  assert(g_simd_after == 3.5);
  std::printf("nested: local %d -> %d, simd %.1f -> %.1f\n",
              g_depth3_local_before, g_depth3_local_after, g_simd_before,
              g_simd_after);

  // 恢复之后 a 的栈指针仍然落在自己那块栈里，而不是跑到 nested 的栈上
  assert(a.resume() == false);
  assert(a.owns(g_stack_a_after));
  std::printf("fiber a after resume sp=%p (still in its own stack)\n",
              static_cast<void*>(g_stack_a_after));

  // ---- 4) 编译期流水线跑在协程栈上
  auto series = make_series();
  series.initialize(2, 0, 4, 0);
  g_series = &series;

  fiber worker{arena, 64 * 1024, entry_series};
  assert(worker.resume());            // 停在第 2 段中间
  assert(g_series_stage2_done == 0);  // 还没跑完
  assert(worker.resume() == false);   // 继续到结束
  assert(g_series_stage2_done == 1);

  auto [incremented, scaled, text] = series.returns();
  assert(incremented == 3);
  assert(scaled == 12);
  assert(text == "12");
  std::printf("series: %d, %d, \"%s\"\n", incremented, scaled, text.c_str());

  // ---- 5) 1000 次往返：换栈不能有累积漂移
  fiber ping{arena, 64 * 1024, entry_ping};
  int rounds = 0;
  while (ping.resume()) ++rounds;
  assert(g_ping == 1000);
  assert(rounds == 1000);
  std::printf("ping: %d yields, %d resumes\n", g_ping, rounds);

  // ---- 6) 协程内的异常在 resumer 的栈上重抛
  fiber bad{arena, 64 * 1024, entry_throws};
  bool caught = false;
  try {
    bad.resume();
  } catch (std::runtime_error const& error) {
    caught = std::string(error.what()) == "fiber boom";
  }
  assert(caught);
  assert(bad.resume() == false);  // 异常只会抛出一次
  std::puts("fiber exception rethrown on the caller stack");

  // ---- 7) MXCSR 控制位 / x87 控制字跨协程隔离
  g_main_mxcsr_rc = read_mxcsr() & kMxcsrRcMask;
  g_main_x87_pc = read_x87_cw() & kX87PcMask;
  fiber fp{arena, 64 * 1024, entry_fp};
  assert(fp.resume());  // 协程改成不同模式后挂起
  assert((read_mxcsr() & kMxcsrRcMask) == g_main_mxcsr_rc);
  assert((read_x87_cw() & kX87PcMask) == g_main_x87_pc);
  assert(fp.resume() == false);
  assert(g_fiber_mxcsr_rc == g_fiber_mxcsr_set);
  assert(g_fiber_x87_pc == g_fiber_x87_set);
  std::printf("fp: main rc=%#x pc=%#x | fiber rc=%#x->%#x pc=%#x->%#x\n",
              g_main_mxcsr_rc, g_main_x87_pc, g_fiber_mxcsr_set,
              g_fiber_mxcsr_rc, g_fiber_x87_set, g_fiber_x87_pc);

  std::printf("arena used total: %zu of %zu\n", arena.used(), arena.size());

  std::puts("execution_context_smoke: ok");
}
