// wire 解码基准：同一份 32 字节协议帧 × N 帧，比较六种解码路线的吞吐。
//
//   A wire-only       : meta_wire_iterator 单程解码（不写 native 缓冲）
//   B two-cursors     : 单循环，wire 读 + slot_at<> 写 aligned range
//   C fork            : meta_pipe 主线 wire + 支线 aligned，同一轮完成
//   D hand-written    : 手写单程 memcpy+bswap，直接写 native struct（理论上限）
//   E two-pass        : 先 memcpy 成 packed struct，再逐字段转 native struct
//   F packed-in-place : #pragma pack(1) 直接按字段读 + bswap（未对齐 / UB，仅作对照）
//   G idle            : 只对 buffer 求和（循环/带宽地板）
//
// 两个方向：协议大端（本机小端时要 bswap）与协议小端（本机 identity）。
//
//   g++ -std=c++23 -O2 -I. -o bench_wire bench/wire_decode_bench.cpp
//
// 指标：ns/帧（多次取最小）、GB/s（32B/帧）、相对 D 的倍率。

#include "meta_ios.hpp"
#include "type_safe_aligned_iterator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;
using namespace type_safe_aligned;

namespace bench {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// 协议：magic u32 / version u8 / kind u8 / flags u16 / device u32 /
//       timestamp u64 / value double / length u32 —— packed、32 字节。
using fields = meta_istream_list<u32, u8, u8, u16, u32, u64, double, u32>;
inline constexpr std::size_t frame_bytes = 32;

// 与协议同序的本地结构（native 布局，有 padding）。
struct native_frame {
  u32 magic;
  u8 version;
  u8 kind;
  u16 flags;
  u32 device;
  u64 timestamp;
  double value;
  u32 length;
};

#pragma pack(push, 1)
struct packed_frame {
  u32 magic;
  u8 version;
  u8 kind;
  u16 flags;
  u32 device;
  u64 timestamp;
  double value;
  u32 length;
};
#pragma pack(pop)

static_assert(sizeof(packed_frame) == frame_bytes);
static_assert(field_size<0, u32, u8, u8, u16, u32, u64, double, u32>() ==
              sizeof(native_frame));

// ---- 手写基线用的 codec（与库内 wire_codec 同语义）----
template <class T, std::endian Wire>
inline T load_as(std::byte const* p) noexcept {
  std::array<std::byte, sizeof(T)> bytes{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    bytes[i] = p[i];
  }
  T v = std::bit_cast<T>(bytes);
  if constexpr (sizeof(T) > 1 && std::endian::native != Wire) {
    if constexpr (std::is_enum_v<T>) {
      using U = std::underlying_type_t<T>;
      return static_cast<T>(std::byteswap(static_cast<U>(v)));
    } else if constexpr (std::is_floating_point_v<T>) {
      using U = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
      return std::bit_cast<T>(std::byteswap(std::bit_cast<U>(v)));
    } else if constexpr (std::is_integral_v<T>) {
      return std::byteswap(v);
    }
  }
  return v;
}

template <std::endian Wire>
inline std::uint64_t mix(std::uint64_t acc, std::uint64_t v) noexcept {
  return acc * 0x100000001B3ull + v;
}

// ---- A：wire 游标单程解码（不落 native 缓冲）----
template <std::endian Wire>
__attribute__((noinline)) std::uint64_t
wire_only(std::byte const* buf, std::size_t frames) {
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < frames; ++i) {
    std::byte const* base = buf + i * frame_bytes;
    meta_states::run<meta_ios::stream_states_co<>, meta_wire_iterator<1, Wire>,
                     fields>::for_each([&]<class Stream>(Stream s) {
      using cursor = typename Stream::to_t;
      constexpr std::size_t offset = cursor::value;
      auto const v = s.object().read(base);
      if constexpr (offset == 0 || offset == 8 || offset == 12 ||
                    offset == 20) {
        acc = mix<Wire>(acc, static_cast<std::uint64_t>(v));
      }
    });
  }
  return acc;
}

// ---- B：单循环，wire 读 + slot_at<> 写 aligned range ----
template <std::endian Wire>
__attribute__((noinline)) std::uint64_t
two_cursors(std::byte const* buf, std::size_t frames) {
  using native_range =
      type_safe_aligned_range<0, u32, u8, u8, u16, u32, u64, double, u32>;
  alignas(8) std::byte native[sizeof(native_frame)]{};
  native_range view{native};

  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < frames; ++i) {
    std::byte const* base = buf + i * frame_bytes;
    meta_states::run<meta_ios::stream_states_co<>, meta_wire_iterator<1, Wire>,
                     fields>::for_each([&]<class Stream>(Stream s) {
      constexpr std::size_t slot = s.index();
      slot_at<slot>(view).get(native) = s.object().read(base);
    });
    acc = mix<Wire>(acc, slot_at<0>(view).get(native));
    acc = mix<Wire>(acc, slot_at<4>(view).get(native));
    acc = mix<Wire>(acc, slot_at<5>(view).get(native));
    acc = mix<Wire>(acc, slot_at<7>(view).get(native));
  }
  return acc;
}

// ---- C：fork，主线 wire / 支线 aligned，同一轮完成 ----
template <std::endian Wire>
__attribute__((noinline)) std::uint64_t
forked(std::byte const* buf, std::size_t frames) {
  using pipe_t = typename meta_pipe<fields>
                     ::template all_to<meta_wire_iterator<1, Wire>>
                     ::template fork<meta_aligned_iterator<>>
                     ::template run<>;
  using native_range =
      type_safe_aligned_range<0, u32, u8, u8, u16, u32, u64, double, u32>;
  alignas(8) std::byte native[sizeof(native_frame)]{};
  native_range view{native};

  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < frames; ++i) {
    std::byte const* base = buf + i * frame_bytes;
    pipe_t::for_each([&]<class Stream>(Stream s) {
      s.branch().emplace(native, s.main().read(base));
    });
    acc = mix<Wire>(acc, slot_at<0>(view).get(native));
    acc = mix<Wire>(acc, slot_at<4>(view).get(native));
    acc = mix<Wire>(acc, slot_at<5>(view).get(native));
    acc = mix<Wire>(acc, slot_at<7>(view).get(native));
  }
  return acc;
}

// ---- D：手写单程，直接写 native struct ----
template <std::endian Wire>
__attribute__((noinline)) std::uint64_t
hand_written(std::byte const* buf, std::size_t frames) {
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < frames; ++i) {
    std::byte const* p = buf + i * frame_bytes;
    native_frame f;
    f.magic = load_as<u32, Wire>(p + 0);
    f.version = load_as<u8, Wire>(p + 4);
    f.kind = load_as<u8, Wire>(p + 5);
    f.flags = load_as<u16, Wire>(p + 6);
    f.device = load_as<u32, Wire>(p + 8);
    f.timestamp = load_as<u64, Wire>(p + 12);
    f.value = load_as<double, Wire>(p + 20);
    f.length = load_as<u32, Wire>(p + 28);
    acc = mix<Wire>(acc, f.magic);
    acc = mix<Wire>(acc, f.device);
    acc = mix<Wire>(acc, f.timestamp);
    acc = mix<Wire>(acc, f.length);
  }
  return acc;
}

// ---- E：两趟，先 memcpy 成 packed struct，再转 native ----
template <std::endian Wire>
__attribute__((noinline)) std::uint64_t
two_pass(std::byte const* buf, std::size_t frames) {
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < frames; ++i) {
    packed_frame packed;
    std::memcpy(&packed, buf + i * frame_bytes, sizeof(packed));
    native_frame f;
    f.magic = load_as<u32, Wire>(reinterpret_cast<std::byte const*>(&packed.magic));
    f.version = packed.version;
    f.kind = packed.kind;
    f.flags = load_as<u16, Wire>(reinterpret_cast<std::byte const*>(&packed.flags));
    f.device =
        load_as<u32, Wire>(reinterpret_cast<std::byte const*>(&packed.device));
    f.timestamp = load_as<u64, Wire>(
        reinterpret_cast<std::byte const*>(&packed.timestamp));
    f.value =
        load_as<double, Wire>(reinterpret_cast<std::byte const*>(&packed.value));
    f.length =
        load_as<u32, Wire>(reinterpret_cast<std::byte const*>(&packed.length));
    acc = mix<Wire>(acc, f.magic);
    acc = mix<Wire>(acc, f.device);
    acc = mix<Wire>(acc, f.timestamp);
    acc = mix<Wire>(acc, f.length);
  }
  return acc;
}

// ---- F：packed 原地按字段读 + bswap（未对齐访问，UB，仅对照）----
template <std::endian Wire>
__attribute__((noinline)) std::uint64_t
packed_in_place(std::byte const* buf, std::size_t frames) {
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < frames; ++i) {
    auto const* p = reinterpret_cast<packed_frame const*>(buf + i * frame_bytes);
    acc = mix<Wire>(acc, load_as<u32, Wire>(
                             reinterpret_cast<std::byte const*>(&p->magic)));
    acc = mix<Wire>(acc, load_as<u32, Wire>(
                             reinterpret_cast<std::byte const*>(&p->device)));
    acc = mix<Wire>(acc, load_as<u64, Wire>(
                             reinterpret_cast<std::byte const*>(&p->timestamp)));
    acc = mix<Wire>(acc, load_as<u32, Wire>(
                             reinterpret_cast<std::byte const*>(&p->length)));
  }
  return acc;
}

// ---- G：只求和（地板）----
__attribute__((noinline)) std::uint64_t idle(std::byte const* buf,
                                             std::size_t frames) {
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < frames; ++i) {
    acc += static_cast<std::uint64_t>(buf[i * frame_bytes + 3]);
  }
  return acc;
}

// ---- 计时 ----
using clock_type = std::chrono::steady_clock;

template <class T>
inline void keep(T const& v) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "m"(v) : "memory");
#else
  (void)v;
#endif
}

template <class F>
double min_ns_per_frame(F&& f, std::size_t frames, int reps, std::uint64_t& sink) {
  double best = 1e300;
  for (int r = 0; r < reps; ++r) {
    auto const t0 = clock_type::now();
    sink ^= f();
    auto const t1 = clock_type::now();
    double const ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count();
    best = std::min(best, ns / static_cast<double>(frames));
  }
  return best;
}

struct row {
  const char* name;
  double ns_per_frame;
  double vs_hand;
  std::uint64_t checksum;
};

template <std::endian Wire>
std::vector<row> run_table(std::byte const* buf, std::size_t frames, int reps) {
  std::uint64_t sink = 0;
  std::vector<row> rows;

  // D 先跑，作为“相对”基准。
  std::uint64_t const checksum_d = hand_written<Wire>(buf, frames);
  double const d = min_ns_per_frame(
      [&] { return hand_written<Wire>(buf, frames); }, frames, reps, sink);
  rows.push_back(row{"D hand-written", d, 1.0, checksum_d});

  auto add = [&](const char* name, auto&& fn) {
    double const ns = min_ns_per_frame([&] { return fn(); }, frames, reps, sink);
    rows.push_back(row{name, ns, ns / d, fn()});
  };
  add("A wire-only", [&] { return wire_only<Wire>(buf, frames); });
  add("B two-cursors", [&] { return two_cursors<Wire>(buf, frames); });
  add("C fork", [&] { return forked<Wire>(buf, frames); });
  add("E two-pass", [&] { return two_pass<Wire>(buf, frames); });
  add("F packed-in-place", [&] { return packed_in_place<Wire>(buf, frames); });
  add("G idle", [&] { return idle(buf, frames); });

  keep(sink);
  return rows;
}

void print_table(const char* title, std::vector<row> const& rows) {
  std::printf("\n== %s ==\n", title);
  std::printf("%-18s %10s %8s %10s\n", "variant", "ns/frame", "vs D", "GB/s");
  for (auto const& r : rows) {
    // 1 byte/ns == 1 GB/s
    double const gb_s = static_cast<double>(frame_bytes) / r.ns_per_frame;
    std::printf("%-18s %10.3f %8.2f %10.2f\n", r.name, r.ns_per_frame,
                r.vs_hand, gb_s);
  }
}

}  // namespace bench

int main() {
  using namespace bench;

  std::size_t const frames = 1u << 16;  // 65536 帧 = 2 MiB
  int const reps = 25;

  std::vector<std::byte> buf(frames * frame_bytes);
  // 填一帧有效数据（大端线格式）
  std::array<std::byte, frame_bytes> const one{
      std::byte{0xCA}, std::byte{0xFE}, std::byte{0xBA}, std::byte{0xBE},
      std::byte{0x01}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x23},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
      std::byte{0x70}, std::byte{0x90}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x40}, std::byte{0x37}, std::byte{0x80}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  };
  for (std::size_t i = 0; i < frames; ++i) {
    std::memcpy(buf.data() + i * frame_bytes, one.data(), frame_bytes);
  }

  auto const be = run_table<std::endian::big>(buf.data(), frames, reps);
  auto const le = run_table<std::endian::little>(buf.data(), frames, reps);

  print_table("protocol = big-endian (host needs byteswap on LE)", be);
  print_table("protocol = little-endian (identity on LE)", le);

  std::printf("\nframes=%zu bytes=%zu reps=%d\n", frames, frames * frame_bytes,
              reps);
  return 0;
}
