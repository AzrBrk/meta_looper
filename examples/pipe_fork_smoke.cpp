// meta_pipe::fork 冒烟测试：一条源流，主线用 wire 游标（packed + 大端），
// 支线用 meta_aligned_iterator（原生对齐），两边共享同一轮的 index 帧；
// 产物合并成 pipe_fork<主线, 支线>，用 s.main() / s.branch() 取。
//
//   g++ -std=c++23 -O0 -I. -o smoke examples/pipe_fork_smoke.cpp
#include "meta_ios.hpp"
#include "type_safe_aligned_iterator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;

namespace fork_tests {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

// 协议：u8 / u32 / u16。wire 布局 packed（0/1/5），原生对齐布局 0/4/8。
using source = meta_istream_list<u8, u32, u16>;

// 主线 wire（packed + 大端），支线 aligned（按 alignof），跑完合成一对。
using forked_pipe = meta_pipe<source>::all_to<meta_wire_iterator<>>
                        ::fork<meta_aligned_iterator<>>
                        ::run<>;

using view_t = typename forked_pipe::type;

// ---- 视图必须是 fork 视图，两边游标类型不同 ----
static_assert(view_t::forked, "fork 之后的视图应该是 pipe_fork");
static_assert(!std::is_same_v<typename view_t::main_t,
                              typename view_t::branch_t>,
              "一条线是 wire 游标，另一条是对齐游标");

// 主线有按值解码，支线只有引用语义。
template <class C>
concept cursor_has_read = requires(C c, std::byte const* p) { c.read(p); };
template <class C>
concept cursor_has_get = requires(C c, std::byte* p) { c.get(p); };
static_assert(cursor_has_read<typename view_t::main_t>);
static_assert(cursor_has_get<typename view_t::branch_t>);

// 同一轮里两个游标指的是同一个字段类型（同一个 index 帧）。
static_assert(std::is_same_v<typename view_t::main_t::type,
                             typename view_t::branch_t::type>);

// 最后一格是 u16：wire 偏移 5，对齐偏移 8。
static_assert(view_t::main_t::value == 5);
static_assert(view_t::branch_t::value == 8);

// ---- 运行时：一轮里 wire 读、aligned 写 ----
struct buffers {
  // 线格式：u8@0, u32@1..5, u16@5..7（packed 大端）
  std::array<std::byte, 8> wire{
      std::byte{0x12},
      std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF},
      std::byte{0x56}, std::byte{0x78},
      std::byte{0x00},
  };
  // 原生布局：u8@0, u32@4..8, u16@8..10
  std::array<std::byte, 16> native{};
};

bool run_pipe() {
  buffers b{};
  std::size_t rounds = 0;

  forked_pipe::for_each([&]<class Stream>(Stream s) {
    using wire_cursor = typename Stream::main_t;
    using native_cursor = typename Stream::branch_t;
    using field = typename wire_cursor::type;

    // 两边偏移不同、字段类型相同、轮次相同（同一个 index 帧）。
    static_assert(!std::is_same_v<wire_cursor, native_cursor>);
    static_assert(std::is_same_v<field, typename native_cursor::type>);

    // wire：按值解码；native：就地构造。
    s.branch().emplace(b.native.data(), s.main().read(b.wire.data()));
    ++rounds;
  });

  // 结果核对：原生缓冲里必须是解出来的本机值。
  using native_layout =
      type_safe_aligned::type_safe_aligned_range<0, u8, u32, u16>;
  native_layout view{b.native.data()};
  auto* base = b.native.data();
  bool const values_ok =
      type_safe_aligned::slot_at<0>(view).get(base) == u8{0x12} &&
      type_safe_aligned::slot_at<1>(view).get(base) == u32{0xDEAD'BEEF} &&
      type_safe_aligned::slot_at<2>(view).get(base) == u16{0x5678};

  return rounds >= 3 && values_ok;
}

}  // namespace fork_tests

int main() {
  bool const ok = fork_tests::run_pipe();
  std::cout << (ok ? "pipe fork ok" : "pipe fork FAILED") << '\n';
  return ok ? 0 : 1;
}
