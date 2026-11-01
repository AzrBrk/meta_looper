// 情景测试：直接按 wire 布局读一个真实 IPv4 头，不定义任何结构体。
//
//   - pipe 语境：算出 20 字节头的 packed 偏移表
//   - looper 语境：逐窗口按「编译期偏移」取字段（不是按类型分派 —— IPv4 头里
//     u16 出现了 4 次，按类型根本分不清）
//   - 用解出来的值做一次 ones-complement 校验和验证
//   - 再逐字段编码回去，与原帧逐字节相同
//   - 与 meta_aligned_iterator<1> 对照：同一份 Ty... 的偏移表是否一致
//   - 用 type_safe_aligned_range 直接迭代这些头部类型、直接写进 array 的 data
//
//   g++ -std=c++23 -O0 -I. -o smoke examples/wire_ipv4_smoke.cpp
#include "type_safe_aligned_iterator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <tuple>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;

namespace ipv4 {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

// 20 字节 IPv4 头：全部大端、packed，没有任何 padding。
//   0  version|IHL
//   1  DSCP|ECN
//   2  total length
//   4  identification
//   6  flags|fragment offset
//   8  TTL
//   9  protocol
//  10  header checksum
//  12  source
//  16  destination
using header_fields = meta_istream_list<u8, u8, u16, u16, u16, u8, u8, u16, u32,
                                        u32>;

// 一帧真实形态的 IPv4 头（src 192.168.1.1 -> dst 192.168.1.2，TCP，DF，TTL 64）
// 校验和 0x9B22 是使整个头 ones-complement 求和等于 0xFFFF 的值。
inline constexpr std::array<std::byte, 20> frame{
    std::byte{0x45}, std::byte{0x00},                    // version 4 / IHL 5, DSCP 0
    std::byte{0x00}, std::byte{0x3C},                    // total length 60
    std::byte{0x1C}, std::byte{0x46},                    // identification
    std::byte{0x40}, std::byte{0x00},                    // DF, fragment offset 0
    std::byte{0x40}, std::byte{0x06},                    // TTL 64, TCP
    std::byte{0x9B}, std::byte{0x22},                    // header checksum
    std::byte{0xC0}, std::byte{0xA8}, std::byte{0x01}, std::byte{0x01},  // 192.168.1.1
    std::byte{0xC0}, std::byte{0xA8}, std::byte{0x01}, std::byte{0x02},  // 192.168.1.2
};

// ---- pipe 语境：偏移表（协议事实，与机器端序无关）----
using offsets = meta_pipe<header_fields>::all_to<meta_wire_iterator<>>
                    ::run<meta_ostream<>>::type::to_t;
static constexpr auto offset_table =
    exp_utilities::to_meta_array_t<offsets>::array();
static_assert(offset_table == std::array<std::size_t, 10>{0, 1, 2, 4, 6, 8, 9,
                                                          10, 12, 16});
// 总长就是最后一格的 advance：12 + sizeof(u32) = 16 + 4 = 20。
static_assert(offsets::template at<9>::advance_t::value == 20);

// ---- looper 语境：逐字段解码（值落在普通局部变量里，没有结构体）----
constexpr bool decodes_header() {
  u8 version_ihl = 0, dscp_ecn = 0, ttl = 0, protocol = 0;
  u16 total_length = 0, identification = 0, flags_fragment = 0, checksum = 0;
  u32 source = 0, destination = 0;

  meta_states::run<meta_ios::stream_states_co<>, meta_wire_iterator<>,
                   header_fields>::for_each([&]<class Stream>(Stream s) {
    using cursor = typename Stream::to_t;
    // 偏移是编译期常量，用它分派字段；类型在这里不可用（u16 出现 4 次）。
    constexpr std::size_t offset = cursor::value;
    std::byte const* base = frame.data();
    if constexpr (offset == 0) {
      version_ihl = s.object().read(base);
    } else if constexpr (offset == 1) {
      dscp_ecn = s.object().read(base);
    } else if constexpr (offset == 2) {
      total_length = s.object().read(base);
    } else if constexpr (offset == 4) {
      identification = s.object().read(base);
    } else if constexpr (offset == 6) {
      flags_fragment = s.object().read(base);
    } else if constexpr (offset == 8) {
      ttl = s.object().read(base);
    } else if constexpr (offset == 9) {
      protocol = s.object().read(base);
    } else if constexpr (offset == 10) {
      checksum = s.object().read(base);
    } else if constexpr (offset == 12) {
      source = s.object().read(base);
    } else {
      static_assert(offset == 16);
      destination = s.object().read(base);
    }
  });

  if ((version_ihl >> 4) != 4 || (version_ihl & 0x0F) != 5) return false;
  if (dscp_ecn != 0x00) return false;
  if (total_length != 60) return false;
  if (identification != 0x1C46) return false;
  if (flags_fragment != 0x4000) return false;  // DF set, offset 0
  if (ttl != 64) return false;
  if (protocol != 6) return false;  // TCP
  if (source != 0xC0A8'0101u || destination != 0xC0A8'0102u) return false;

  // ones-complement 校验和：把解出的字段两两拼回 16 位再累加（含校验和字段）。
  std::uint32_t sum = 0;
  const std::uint32_t words[] = {
      std::uint32_t(version_ihl) << 8 | dscp_ecn,
      total_length,
      identification,
      flags_fragment,
      std::uint32_t(ttl) << 8 | protocol,
      checksum,
      source >> 16,
      source & 0xFFFFu,
      destination >> 16,
      destination & 0xFFFFu,
  };
  for (std::uint32_t w : words) sum += w;
  while ((sum >> 16) != 0) sum = (sum & 0xFFFFu) + (sum >> 16);
  return sum == 0xFFFFu;
}
static_assert(decodes_header());

// ---- looper 语境：逐字段编码回去，必须和原帧逐字节相同 ----
constexpr std::array<std::byte, 20> encodes_header() {
  std::array<std::byte, 20> out{};
  meta_states::run<meta_ios::stream_states_co<>, meta_wire_iterator<>,
                   header_fields>
      ::for_each_forward(
          [&]<class Stream, class V>(Stream s, V v) {
            s.object().write(out.data(), v);
          },
          u8{0x45}, u8{0x00}, u16{0x003C}, u16{0x1C46}, u16{0x4000},
          u8{0x40}, u8{0x06}, u16{0x9B22}, u32{0xC0A8'0101}, u32{0xC0A8'0102});
  return out;
}
static_assert(encodes_header() == frame);

// ---- 4) 三种迭代器对这份头的布局是否一致 ----
// 这个头恰好是自然对齐的（u8/u16/u32 的排列没产生填充），所以 aligned(alignof)、
// aligned<1>、wire(packed) 的偏移表逐格相同。
using aligned_offsets = meta_pipe<header_fields>
                            ::all_to<meta_aligned_iterator<>>
                            ::run<meta_ostream<>>::type::to_t;
using packed_offsets = meta_pipe<header_fields>
                           ::all_to<meta_aligned_iterator<1>>
                           ::run<meta_ostream<>>::type::to_t;

static_assert(exp_utilities::to_meta_array_t<aligned_offsets>::array() ==
              offset_table);
static_assert(exp_utilities::to_meta_array_t<packed_offsets>::array() ==
              offset_table);

// 单格对照：偏移一样，语义不一样（内存引用 vs 按值解码）。
using aligned_u16 = typename meta_invoke<meta_aligned_iterator<1>, u16>::type;
using wire_u16 = typename meta_invoke<meta_wire_iterator<>, u16>::type;
static_assert(aligned_u16::value == wire_u16::value);

// 能力差异用 concept 判定（requires-expression 直接写非法成员在非模板语境
// 会变成硬错误，包一层 concept 才是可判定的）。
template <class C>
concept cursor_has_read = requires(C c, std::byte const* p) { c.read(p); };
template <class C>
concept cursor_has_emplace =
    requires(C c, std::byte* p, u16 v) { c.emplace(p, v); };

static_assert(cursor_has_emplace<aligned_u16>);   // 内存语义：构造对象
static_assert(!cursor_has_read<aligned_u16>);     // 没有按值解码
static_assert(cursor_has_read<wire_u16>);         // 线上语义：按值解码
static_assert(!cursor_has_emplace<wire_u16>);     // 不往缓冲区里构造对象

// range 的总大小也是同一套偏移：刚好 20 字节，等于标准头长。
// 这里用 Alignment = 0（按 alignof）；对这个头它和 packed(1) 完全一样，但不会
// 在弱对齐 CPU 上触发未对齐访问的编译期拒绝（range 走的是引用语义）。
static_assert(type_safe_aligned::field_size<1, u8, u8, u16, u16, u16, u8, u8,
                                            u16, u32, u32>() == 20);
static_assert(type_safe_aligned::field_size<0, u8, u8, u16, u16, u16, u8, u8,
                                            u16, u32, u32>() == 20);

// 上面两个相等是**这份字段顺序**的性质，不是通用规律：这个头每一格的 packed
// 偏移本来就已经满足自己的 alignof（u8@0, u8@1, u16@2, u16@4, u16@6, u8@8,
// u8@9, u16@10, u32@12, u32@16），所以对齐布局插不进 padding。
// 对照组：u8 后面直接跟 u32 时两者立刻分叉（对齐布局 8 字节，packed 5 字节）。
static_assert(type_safe_aligned::field_size<0, u8, u32>() == 8);
static_assert(type_safe_aligned::field_size<1, u8, u32>() == 5);

// ---- 5) type_safe_aligned_range：直接迭代这些头部类型、直接写 array 的 data ----
// 布局与 wire 相同（上面已断言），所以不用手写 if offset == I；区别只在值语义：
// range 写的是原生表示，wire 才是协议端序。
bool range_scenario() {
  bool ok = true;

  using header_range = type_safe_aligned::type_safe_aligned_range<
      0, u8, u8, u16, u16, u16, u8, u8, u16, u32, u32>;

  std::array<std::byte, 20> buf{};
  header_range range{buf.data()};
  type_safe_aligned::initialize(range, u8{0x45}, u8{0x00}, u16{0x003C},
                                u16{0x1C46}, u16{0x4000}, u8{0x40}, u8{0x06},
                                u16{0x9B22}, u32{0xC0A8'0101},
                                u32{0xC0A8'0102});

  // (a) 顺着 range 的迭代器 + 流给的编译期槽位号，generic 地读回一组值：
  //     没有字节偏移，也没有逐字段的 if。注意：这一步能成立是因为缓冲区是**原生
  //     表示**（range 的槽代理只给 T&，不会 byteswap）。
  {
    using header_values =
        std::tuple<u8, u8, u16, u16, u16, u8, u8, u16, u32, u32>;
    using header_types =
        meta_istream_list<u8, u8, u16, u16, u16, u8, u8, u16, u32, u32>;

    header_values values{};
    auto it = range.begin();
    meta_states::run<meta_ios::stream_states_co<>, meta_iterator,
                     header_types>::for_each([&]<class Stream>(Stream s) {
      using field = typename Stream::to_t;      // 当前槽位类型（编译期）
      constexpr std::size_t slot = s.index();   // 当前槽位号（编译期）
      std::get<slot>(values) = static_cast<field&>(*it);
      ++it;
    });

    ok = ok && it == range.end();
    ok = ok && values == header_values{u8{0x45}, u8{0x00}, u16{0x003C},
                                       u16{0x1C46}, u16{0x4000}, u8{0x40},
                                       u8{0x06}, u16{0x9B22},
                                       u32{0xC0A8'0101}, u32{0xC0A8'0102}};
  }

  // (b) 同一块缓冲区按 wire（大端）解一遍：偏移一样，值语义不一样。
  u16 wire_len = 0;
  u32 wire_src = 0;
  meta_states::run<meta_ios::stream_states_co<>, meta_wire_iterator<>,
                   header_fields>::for_each([&]<class Stream>(Stream s) {
    using cursor = typename Stream::to_t;
    constexpr std::size_t offset = cursor::value;
    if constexpr (offset == 2) {
      wire_len = s.object().read(buf.data());
    } else if constexpr (offset == 12) {
      wire_src = s.object().read(buf.data());
    }
  });

  if constexpr (std::endian::native == std::endian::big) {
    // 大端主机：range 写出来的字节就是线格式。
    ok = ok && buf == frame && wire_len == 60 &&
         wire_src == 0xC0A8'0101u;
  } else {
    // 小端主机：偏移相同，但多字节字段是原生序，按大端读会被翻过来。
    ok = ok && wire_len == 0x3C00 && wire_src == 0x0101A8C0u;
  }

  return ok;
}

}  // namespace ipv4

int main() {
  // 非 constexpr 语境再走一遍。
  bool const ok = ipv4::decodes_header() && ipv4::encodes_header() == ipv4::frame &&
                  ipv4::range_scenario();
  std::cout << (ok ? "ipv4 wire header ok" : "ipv4 wire header FAILED") << '\n';
  return ok ? 0 : 1;
}
