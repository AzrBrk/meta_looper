#include "meta_abi_range.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <type_traits>

using namespace meta_abi;

template <class Range>
void check_layout() {
  alignas(16) std::array<std::byte, Range::storage_bytes + 16> buffer;
  buffer.fill(std::byte{0x5a});
  const Range range{std::span<std::byte>{buffer}};
  range.initialize();
  assert(std::all_of(buffer.begin(), buffer.begin() + Range::storage_bytes,
                     [](std::byte b) { return b == std::byte{}; }));
  assert(buffer[Range::storage_bytes] == std::byte{0x5a});
  assert(range.stack_limit() == buffer.data());
  assert(range.stack_bottom() == range.layout_end());
  assert(range.layout_end() == buffer.data() + (Range::storage_bytes & ~15u));
  assert(range.storage_end() == buffer.data() + Range::storage_bytes);
  assert(range.frame() == buffer.data() + Range::frame_offset);
  assert(range.sp() == buffer.data() + Range::sp_offset);
  assert(range.ret_address() == buffer.data() + Range::ret_address_offset);
  assert(range.return_address() == buffer.data() + Range::return_address_offset);
  assert(range.aligned_range().data() == buffer.data());
  assert(type_safe_aligned::slot_at<1>(range.aligned_range()).address_of(range.data()) ==
         range.frame());
  assert(range.frame_range().data() == range.frame());
  assert(reinterpret_cast<std::uintptr_t>(range.frame()) % 16 == 0);
  assert(reinterpret_cast<std::uintptr_t>(range.fp_regs()) % 16 == 0);
  assert(range.gpr_regs() >= range.frame());
  assert(range.fp_control() >= range.fp_regs());
  assert(range.fp_control() < range.fp_regs() + sizeof(typename Range::fp_type));
  assert(range.sp() + sizeof(typename Range::word_type) <= range.layout_end());
  if constexpr (Range::abi_type::has_simd) {
    assert(range.simd_regs() == range.fp_regs());
  }

  if constexpr (Range::abi_type::stack_return) {
    assert(range.stack_top() == range.initial_sp() + sizeof(typename Range::word_type));
    assert(reinterpret_cast<std::uintptr_t>(range.stack_top()) % 16 ==
           16 - sizeof(typename Range::word_type));
  } else {
    assert(range.stack_top() == range.initial_sp());
    assert(reinterpret_cast<std::uintptr_t>(range.stack_top()) % 16 == 0);
  }
  if constexpr (Range::shadow_bytes != 0) {
    assert(range.shadow_space() == range.stack_top() + sizeof(typename Range::word_type));
    assert(range.shadow_space() + Range::shadow_bytes <= range.fp_regs());
  }
  if constexpr (Range::red_zone_bytes != 0) {
    assert(range.red_zone() >= range.stack_limit());
    assert(range.red_zone() + 128 == range.stack_top());
  }

  typename Range::word_type value = 0x12345678;
  std::memcpy(range.ret_address(), &value, sizeof(value));
  typename Range::word_type read{};
  std::memcpy(&read, range.ret_address(), sizeof(read));
  assert(read == value);
  auto frame = range.frame_range();
  frame.template get<Range::abi_type::sp_index>() = value;
  std::memcpy(&read, range.sp(), sizeof(read));
  assert(read == value);

  bool rejected = false;
  try {
    Range bad{std::span<std::byte>{buffer.data(), Range::storage_bytes - 1}};
  } catch (const std::length_error&) {
    rejected = true;
  }
  assert(rejected);
  rejected = false;
  try {
    Range bad{buffer.data() + 1};
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
  rejected = false;
  try {
    Range bad{static_cast<std::byte*>(nullptr)};
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
}

static_assert(meta_abi_range_win_x64<1024>::frame_bytes == 304);
static_assert(meta_abi_range_sysv_x64<1024>::frame_bytes == 96);
static_assert(meta_abi_range_win_x86<1024>::frame_bytes == 64);
static_assert(meta_abi_range_sysv_x86<1024>::frame_bytes == 64);
static_assert(meta_abi_range_aapcs64<1024>::frame_bytes == 208);
static_assert(meta_abi_range_aapcs32_vfp<1024>::frame_bytes == 144);
static_assert(meta_abi_range_riscv64_lp64d<1024>::frame_bytes == 240);
static_assert(meta_abi_range_riscv32_ilp32d<1024>::frame_bytes == 192);
static_assert(sizeof(meta_abi_range_win_x64<1024>) == sizeof(std::byte*));
static_assert(meta_abi_range_win_x64<4096>::frame_offset -
                  meta_abi_range_win_x64<1024>::frame_offset == 3072);

#if defined(__x86_64__) && defined(__ELF__)
namespace {
using switch_range = meta_abi_range_sysv_x64<65536>;
alignas(16) std::array<std::byte, switch_range::storage_bytes> main_storage;
alignas(16) std::array<std::byte, switch_range::storage_bytes> child_storage;
switch_range main_range{main_storage.data()};
switch_range child_range{child_storage.data()};
int phase = 0;
x86_control main_control{}, child_control{};

x86_control current_control() {
  x86_control control{};
  asm volatile("stmxcsr %0" : "=m"(control.mxcsr));
  asm volatile("fnstcw %0" : "=m"(control.x87_control_word));
  return control;
}

void check_control(x86_control expected) {
  const auto actual = current_control();
  assert(actual.mxcsr == expected.mxcsr);
  assert(actual.x87_control_word == expected.x87_control_word);
}
}

// These offsets are relative to frame(), not to the buffer's low address.
static_assert(switch_range::fp_offset - switch_range::frame_offset == 16);
static_assert(switch_range::gpr_offset - switch_range::frame_offset == 32);
static_assert(switch_range::sp_offset - switch_range::frame_offset == 80);
static_assert(offsetof(x86_control, x87_control_word) == 4);
static_assert(offsetof(sysv_x64_gpr_state, r15) == 40);

extern "C" void abi_test_switch(std::byte* from, std::byte* to);
extern "C" void abi_test_entry();
extern "C" void abi_test_exit();

asm(R"(
.text
.globl abi_test_switch
.type abi_test_switch,@function
abi_test_switch:
  stmxcsr 16(%rdi)
  fnstcw 20(%rdi)
  movq %rbx, 32(%rdi)
  movq %rbp, 40(%rdi)
  movq %r12, 48(%rdi)
  movq %r13, 56(%rdi)
  movq %r14, 64(%rdi)
  movq %r15, 72(%rdi)
  movq %rsp, 80(%rdi)
  ldmxcsr 16(%rsi)
  fldcw 20(%rsi)
  movq 32(%rsi), %rbx
  movq 40(%rsi), %rbp
  movq 48(%rsi), %r12
  movq 56(%rsi), %r13
  movq 64(%rsi), %r14
  movq 72(%rsi), %r15
  movq 80(%rsi), %rsp
  ret
.size abi_test_switch,.-abi_test_switch
.globl abi_test_entry
.type abi_test_entry,@function
abi_test_entry:
  movq %rsp, %rax
  andq $15, %rax
  cmpq $8, %rax
  jne 1f
  subq $8, %rsp
  call abi_test_body
  addq $8, %rsp
  ret
1:
  ud2
.size abi_test_entry,.-abi_test_entry
.globl abi_test_exit
.type abi_test_exit,@function
abi_test_exit:
  call abi_test_finish
  ud2
.size abi_test_exit,.-abi_test_exit
)");

extern "C" void abi_test_body() {
  check_control(child_control);
  volatile std::uint64_t stack_value = 0x123456789abcdef0;
  phase = 1;
  abi_test_switch(child_range.frame(), main_range.frame());
  check_control(child_control);
  assert(stack_value == 0x123456789abcdef0);
  phase = 2;
}

extern "C" [[noreturn]] void abi_test_finish() {
  assert(phase == 2);
  phase = 3;
  abi_test_switch(child_range.frame(), main_range.frame());
  std::abort();
}

void check_switch() {
  main_range.initialize();
  child_range.initialize();
  const auto initial_sp = reinterpret_cast<std::uintptr_t>(child_range.initial_sp());
  const auto entry = reinterpret_cast<std::uintptr_t>(&abi_test_entry);
  const auto exit = reinterpret_cast<std::uintptr_t>(&abi_test_exit);
  std::memcpy(child_range.sp(), &initial_sp, sizeof(initial_sp));
  std::memcpy(child_range.ret_address(), &entry, sizeof(entry));
  std::memcpy(child_range.return_address(), &exit, sizeof(exit));
  main_control = current_control();
  child_control = main_control;
  child_control.mxcsr ^= 0x2000;
  child_control.x87_control_word ^= 0x0400;
  std::memcpy(child_range.fp_control(), &child_control, sizeof(child_control));

  abi_test_switch(main_range.frame(), child_range.frame());
  check_control(main_control);
  assert(phase == 1);
  abi_test_switch(main_range.frame(), child_range.frame());
  check_control(main_control);
  assert(phase == 3);
}
#endif

int main() {
  check_layout<meta_abi_range_win_x64<1024>>();
  check_layout<meta_abi_range_win_x64<320>>();
  check_layout<meta_abi_range_win_x64<1031>>();
  check_layout<meta_abi_range_sysv_x64<1024>>();
  check_layout<meta_abi_range_sysv_x64<240>>();
  check_layout<meta_abi_range_win_x86<1024>>();
  check_layout<meta_abi_range_win_x86<80>>();
  check_layout<meta_abi_range_sysv_x86<1024>>();
  check_layout<meta_abi_range_aapcs64<1024>>();
  check_layout<meta_abi_range_aapcs64<224>>();
  check_layout<meta_abi_range_aapcs32_vfp<1024>>();
  check_layout<meta_abi_range_aapcs32_vfp<160>>();
  check_layout<meta_abi_range_riscv64_lp64d<1024>>();
  check_layout<meta_abi_range_riscv64_lp64d<256>>();
  check_layout<meta_abi_range_riscv32_ilp32d<1024>>();
  check_layout<meta_abi_range_riscv32_ilp32d<208>>();
#if defined(__x86_64__) && defined(__ELF__)
  check_switch();
#endif
}
