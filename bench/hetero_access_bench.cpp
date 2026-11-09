// 异构内存访问基准：std::any / std::variant / type_safe_aligned_range 的运行时开销
//
// 同一条记录（8 个异构字段 u32,u8,u8,u16,u32,u64,double,u32）用五种方式存：
//
//   native    : std::vector<native_rec>              每个字段就是它的天然类型（40 B/记录）
//   any       : std::vector<array<any,8>>            每字段一个 std::any（128 B/记录）
//   variant   : std::vector<array<variant<...>,8>>   每字段一个 variant（128 B/记录）
//   packed    : 一整块 std::byte 缓冲                type_safe_aligned_range<1, ...>（32 B/记录）
//   aligned   : 同上但 Alignment = 0                 type_safe_aligned_range<0, ...>（40 B/记录）
//
// 后面两个是同一个模板的两个 Alignment，字段顺序完全一样，只差 u64/double 有没有
// 被挤到偏移 12/20 上：
//   1 = packed      线上/文件格式的布局（协议帧本来就是这个样子）
//   0 = 自然对齐   每格都对上 alignof，与 native_rec 同布局
// 两列都跑，就能看到“压缩存储省下的字节”与“未对齐访存多花的周期”到底哪个赢。
//
// 四组测量，回答不同问题：
//
//   G1 定点访问    字段位置是编译期常量（正常解码循环的形状）
//                  native / any / variant / range<1|0>-slot_at<I> / range<1|0>-get<I>
//                  → any/variant 的每字段代价 = typeid 比较 / 下标比较 + 抛异常分支
//                  → range 的 slot_at<I> 是 base+常量偏移，零检查
//
//   G2 运行期下标  位置是运行期值（k = i & 7），每记录只读 1 个字段
//                  native-switch / any-switch / variant-switch
//                  range<1|0>-r[k] / range<1|0>-迭代器定位 / range<1|0>-走 k 步
//                  → 这就是“运行时迭代器”的真实开销
//
//   G3 无位置信息  不知道位置，逐个槽位动态判定类型后再读（通用 visitor 形状）
//                  native / any（typeid 链）/ variant（visit 跳表）
//                  range<1|0>（type_mask_series 位图链 + 运行时迭代器）
//
//   G4 大载荷      字段里放 24 字节 blob（超过 std::any 的 SBO）
//                  any / variant / range<1|0> 的构造开销与读取开销
//                  → any 的唯一一次堆分配在这里显形
//
//   G7 受检类型访问 运行期索引数组；对比代理转换 / at<T> / variant。
//                  唯一匹配与多匹配分开，交替测量，报告中位数并校验结果。
//                  ./bench_hetero --typed-only 只运行这一组。
//
//   G8 运行时数值  get<I> / as<T> / op 表；固定槽位与混合槽位分开测。
//                  ./bench_hetero --runtime-only 只运行这一组。
//
//   g++ -std=c++23 -O2 -falign-functions=64 -falign-loops=32 -I. -o
//       bench_hetero bench/hetero_access_bench.cpp
//
// 对齐开关不是可选的：同一份机器码换个地址就会落在不同的 cache line / BTB 槽上，
// 实测能差 22%（g2_range_ctor 与 g2_range_index 编译出完全相同的 125 条指令，
// 不带对齐开关时分别是 1.59 与 1.29 ns）。加了 -falign-functions=64 之后
// 同码不同址的差异降到 3% 以内，表格才有意义。
//
// 指标：ns/记录（多次取最小）；vs 列相对本组地板；ns/op = ns/记录 / 字段访问次数。

#include "meta_ios.hpp"
#include "type_safe_aligned_iterator.hpp"

#include <algorithm>
#include <any>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <variant>
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

inline constexpr std::size_t field_count = 8;

// ---------------------------------------------------------------------------
// 数据形状
// ---------------------------------------------------------------------------

// 字段的天然类型；variant 的候选集就是这 5 种的去重。
using var_t = std::variant<u8, u16, u32, u64, double>;

struct native_rec {
  u32 magic;
  u8 version;
  u8 kind;
  u16 flags;
  u32 device;
  u64 timestamp;
  double value;
  u32 length;
};

struct any_rec {
  std::array<std::any, field_count> f;
};

struct var_rec {
  std::array<var_t, field_count> f;
};

// 同一条记录，两种布局：
//   Alignment = 1：packed 32 B —— 线上/文件格式的布局（协议帧本来就这样）
//   Alignment = 0：自然对齐 40 B —— 内存布局，每格都对上 alignof，与 native_rec 同布局
// 两者的字段顺序、类型完全一样，差别只在“u64/double 有没有被挤到偏移 12/20 上”。
template <std::size_t A>
using range_at = type_safe_aligned_range<A, u32, u8, u8, u16, u32, u64, double, u32>;
template <std::size_t A>
using iter_at =
    type_safe_aligned_iterator<A, u32, u8, u8, u16, u32, u64, double, u32>;
template <std::size_t A>
inline constexpr std::size_t frame_bytes_at =
    field_size<A, u32, u8, u8, u16, u32, u64, double, u32>();

// 兼容旧名字：默认那一列仍是 packed。
using range_t = range_at<1>;
using iter_t = iter_at<1>;
inline constexpr std::size_t packed_bytes = frame_bytes_at<1>;

static_assert(frame_bytes_at<1> == 32);
static_assert(frame_bytes_at<0> == 40);
static_assert(frame_bytes_at<0> == sizeof(native_rec));

// 一条记录的样本值（与 pipe_fork_smoke / wire_socket_smoke 用的是同一帧）。
struct values {
  u32 magic;
  u8 version;
  u8 kind;
  u16 flags;
  u32 device;
  u64 timestamp;
  double value;
  u32 length;
};

inline values const& sample() {
  static values const v{0xCAFEBABEu, 1, 2, 0, 0x123, 0x17090, 23.5, 0};
  return v;
}

struct dataset {
  std::vector<native_rec> native;
  std::vector<any_rec> anys;
  std::vector<var_rec> vars;
  std::vector<std::byte> packed;   // Alignment = 1（32 B/记录）
  std::vector<std::byte> aligned;  // Alignment = 0（40 B/记录）
};

dataset make_dataset(std::size_t n) {
  values const& v = sample();
  dataset d;
  d.native.assign(n, native_rec{v.magic, v.version, v.kind, v.flags, v.device,
                                v.timestamp, v.value, v.length});
  d.anys.assign(n, any_rec{{std::any{v.magic}, std::any{v.version},
                            std::any{v.kind}, std::any{v.flags},
                            std::any{v.device}, std::any{v.timestamp},
                            std::any{v.value}, std::any{v.length}}});
  d.vars.assign(n, var_rec{{var_t{v.magic}, var_t{v.version}, var_t{v.kind},
                            var_t{v.flags}, var_t{v.device}, var_t{v.timestamp},
                            var_t{v.value}, var_t{v.length}}});
  auto fill = [&](std::vector<std::byte>& buf, std::size_t stride, auto make) {
    buf.resize(n * stride);
    for (std::size_t i = 0; i < n; ++i) {
      auto const r = make(buf.data() + i * stride);
      type_safe_aligned::initialize(r, v.magic, v.version, v.kind, v.flags,
                                    v.device, v.timestamp, v.value, v.length);
    }
  };
  fill(d.packed, packed_bytes,
       [](std::byte* p) { return range_at<1>{p}; });
  fill(d.aligned, frame_bytes_at<0>,
       [](std::byte* p) { return range_at<0>{p}; });
  return d;
}

// ---------------------------------------------------------------------------
// 计时
// ---------------------------------------------------------------------------

using clock_type = std::chrono::steady_clock;

template <class T>
inline void keep(T const& v) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "m"(v) : "memory");
#else
  (void)v;
#endif
}

// 返回最好一次的总纳秒数。
template <class F>
double best_total_ns(F&& f, int reps, double& sink) {
  double best = 1e300;
  for (int r = 0; r < reps; ++r) {
    auto const t0 = clock_type::now();
    sink += f();
    auto const t1 = clock_type::now();
    double const ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    best = std::min(best, ns);
  }
  return best;
}

struct row {
  char const* name;
  double ns_per_rec;
  double vs_floor;
  double checksum;
  bool check;  // 参与 checksum 一致性比较
};

// 运行期位图取位（与 aligned_iterator_storage::slot_mask_hit 同一套写法）。
template <class T, class... Ty>
inline bool mask_bit(std::size_t index) {
  constexpr u128 mask = type_mask_series<T, Ty...>;
  std::uint64_t const half = index < 64 ? mask.low : mask.high;
  return ((half >> (index & 63)) & 1) != 0;
}

// ---------------------------------------------------------------------------
// G1：定点访问（字段位置是编译期常量）
// ---------------------------------------------------------------------------

// 注意：每组都用“按字段位置拆开的累加器”（acc[k]），而不是一个 s += ... 链。
// 单个 double 累加器的延迟是 4 周期，8 次串行相加就是 32 周期——那样测到的是
// FP 加法延迟而不是访存/派发成本，会把 any/variant/range 的差异全部掩盖掉。

__attribute__((noinline)) double g1_native(dataset& d) {
  double acc[field_count]{};
  for (auto const& r : d.native) {
    acc[0] += static_cast<double>(r.magic);
    acc[1] += static_cast<double>(r.version);
    acc[2] += static_cast<double>(r.kind);
    acc[3] += static_cast<double>(r.flags);
    acc[4] += static_cast<double>(r.device);
    acc[5] += static_cast<double>(r.timestamp);
    acc[6] += r.value;
    acc[7] += static_cast<double>(r.length);
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

__attribute__((noinline)) double g1_any(dataset& d) {
  double acc[field_count]{};
  for (auto const& r : d.anys) {
    acc[0] += static_cast<double>(std::any_cast<u32 const&>(r.f[0]));
    acc[1] += static_cast<double>(std::any_cast<u8 const&>(r.f[1]));
    acc[2] += static_cast<double>(std::any_cast<u8 const&>(r.f[2]));
    acc[3] += static_cast<double>(std::any_cast<u16 const&>(r.f[3]));
    acc[4] += static_cast<double>(std::any_cast<u32 const&>(r.f[4]));
    acc[5] += static_cast<double>(std::any_cast<u64 const&>(r.f[5]));
    acc[6] += std::any_cast<double const&>(r.f[6]);
    acc[7] += static_cast<double>(std::any_cast<u32 const&>(r.f[7]));
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

__attribute__((noinline)) double g1_variant(dataset& d) {
  double acc[field_count]{};
  for (auto const& r : d.vars) {
    acc[0] += static_cast<double>(std::get<u32>(r.f[0]));
    acc[1] += static_cast<double>(std::get<u8>(r.f[1]));
    acc[2] += static_cast<double>(std::get<u8>(r.f[2]));
    acc[3] += static_cast<double>(std::get<u16>(r.f[3]));
    acc[4] += static_cast<double>(std::get<u32>(r.f[4]));
    acc[5] += static_cast<double>(std::get<u64>(r.f[5]));
    acc[6] += std::get<double>(r.f[6]);
    acc[7] += static_cast<double>(std::get<u32>(r.f[7]));
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

template <std::size_t A>
__attribute__((noinline)) double g1_range(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::byte* base = buf + i * frame_bytes_at<A>;
    range_at<A> const r(base);
    acc[0] += static_cast<double>(slot_at<0>(r).get(base));
    acc[1] += static_cast<double>(slot_at<1>(r).get(base));
    acc[2] += static_cast<double>(slot_at<2>(r).get(base));
    acc[3] += static_cast<double>(slot_at<3>(r).get(base));
    acc[4] += static_cast<double>(slot_at<4>(r).get(base));
    acc[5] += static_cast<double>(slot_at<5>(r).get(base));
    acc[6] += slot_at<6>(r).get(base);
    acc[7] += static_cast<double>(slot_at<7>(r).get(base));
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

// 同一件事的封装形式：range.get<I>()（tuple-like 入口）。
template <std::size_t A>
__attribute__((noinline)) double g1_range_get(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    range_at<A> const r(buf + i * frame_bytes_at<A>);
    acc[0] += static_cast<double>(get<0>(r));
    acc[1] += static_cast<double>(get<1>(r));
    acc[2] += static_cast<double>(get<2>(r));
    acc[3] += static_cast<double>(get<3>(r));
    acc[4] += static_cast<double>(get<4>(r));
    acc[5] += static_cast<double>(get<5>(r));
    acc[6] += get<6>(r);
    acc[7] += static_cast<double>(get<7>(r));
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

// ---------------------------------------------------------------------------
// G2：运行期下标（每记录只读 1 个字段，位置 k = i & 7 是运行期值）
// ---------------------------------------------------------------------------

__attribute__((noinline)) double g2_native(dataset& d, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t const k = i & (field_count - 1);
    auto const& r = d.native[i];
    switch (k) {
      case 0: acc[0] += static_cast<double>(r.magic); break;
      case 1: acc[1] += static_cast<double>(r.version); break;
      case 2: acc[2] += static_cast<double>(r.kind); break;
      case 3: acc[3] += static_cast<double>(r.flags); break;
      case 4: acc[4] += static_cast<double>(r.device); break;
      case 5: acc[5] += static_cast<double>(r.timestamp); break;
      case 6: acc[6] += r.value; break;
      default: acc[7] += static_cast<double>(r.length); break;
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

__attribute__((noinline)) double g2_any(dataset& d, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t const k = i & (field_count - 1);
    auto const& r = d.anys[i];
    switch (k) {
      case 0: acc[0] += static_cast<double>(std::any_cast<u32 const&>(r.f[0])); break;
      case 1: acc[1] += static_cast<double>(std::any_cast<u8 const&>(r.f[1])); break;
      case 2: acc[2] += static_cast<double>(std::any_cast<u8 const&>(r.f[2])); break;
      case 3: acc[3] += static_cast<double>(std::any_cast<u16 const&>(r.f[3])); break;
      case 4: acc[4] += static_cast<double>(std::any_cast<u32 const&>(r.f[4])); break;
      case 5: acc[5] += static_cast<double>(std::any_cast<u64 const&>(r.f[5])); break;
      case 6: acc[6] += std::any_cast<double const&>(r.f[6]); break;
      default: acc[7] += static_cast<double>(std::any_cast<u32 const&>(r.f[7])); break;
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

__attribute__((noinline)) double g2_variant(dataset& d, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t const k = i & (field_count - 1);
    auto const& r = d.vars[i];
    switch (k) {
      case 0: acc[0] += static_cast<double>(std::get<u32>(r.f[0])); break;
      case 1: acc[1] += static_cast<double>(std::get<u8>(r.f[1])); break;
      case 2: acc[2] += static_cast<double>(std::get<u8>(r.f[2])); break;
      case 3: acc[3] += static_cast<double>(std::get<u16>(r.f[3])); break;
      case 4: acc[4] += static_cast<double>(std::get<u32>(r.f[4])); break;
      case 5: acc[5] += static_cast<double>(std::get<u64>(r.f[5])); break;
      case 6: acc[6] += std::get<double>(r.f[6]); break;
      default: acc[7] += static_cast<double>(std::get<u32>(r.f[7])); break;
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

// 迭代器 O(1) 定位：直接带下标构造（构造里只有一次范围检查 + 查表）。
template <std::size_t A>
__attribute__((noinline)) double g2_range_ctor(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t const k = i & (field_count - 1);
    iter_at<A> it(buf + i * frame_bytes_at<A>, k);
    auto& p = *it;
    switch (k) {
      case 0: acc[0] += static_cast<double>(static_cast<u32 const&>(p)); break;
      case 1: acc[1] += static_cast<double>(static_cast<u8 const&>(p)); break;
      case 2: acc[2] += static_cast<double>(static_cast<u8 const&>(p)); break;
      case 3: acc[3] += static_cast<double>(static_cast<u16 const&>(p)); break;
      case 4: acc[4] += static_cast<double>(static_cast<u32 const&>(p)); break;
      case 5: acc[5] += static_cast<double>(static_cast<u64 const&>(p)); break;
      case 6: acc[6] += static_cast<double>(static_cast<double const&>(p)); break;
      default: acc[7] += static_cast<double>(static_cast<u32 const&>(p)); break;
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

// 运行期下标 r[k]：返回受检代理（与迭代器构造同一套检查）。
template <std::size_t A>
__attribute__((noinline)) double g2_range_index(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t const k = i & (field_count - 1);
    range_at<A> const r(buf + i * frame_bytes_at<A>);
    auto p = r[k];
    switch (k) {
      case 0: acc[0] += static_cast<double>(static_cast<u32 const&>(p)); break;
      case 1: acc[1] += static_cast<double>(static_cast<u8 const&>(p)); break;
      case 2: acc[2] += static_cast<double>(static_cast<u8 const&>(p)); break;
      case 3: acc[3] += static_cast<double>(static_cast<u16 const&>(p)); break;
      case 4: acc[4] += static_cast<double>(static_cast<u32 const&>(p)); break;
      case 5: acc[5] += static_cast<double>(static_cast<u64 const&>(p)); break;
      case 6: acc[6] += static_cast<double>(static_cast<double const&>(p)); break;
      default: acc[7] += static_cast<double>(static_cast<u32 const&>(p)); break;
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

// 类型在编译期已知、只有位置是运行期：把 switch 当派发器，每个分支里用 get<I>。
// 这是“真实解码器知道自己在读哪个字段”的写法，和 native/variant 那条 switch
// 完全同构，所以能直接看出代理的那三个检查到底值多少钱。
template <std::size_t A>
__attribute__((noinline)) double g2_range_switch_get(std::byte* buf,
                                                    std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t const k = i & (field_count - 1);
    range_at<A> const r(buf + i * frame_bytes_at<A>);
    switch (k) {
      case 0: acc[0] += static_cast<double>(get<0>(r)); break;
      case 1: acc[1] += static_cast<double>(get<1>(r)); break;
      case 2: acc[2] += static_cast<double>(get<2>(r)); break;
      case 3: acc[3] += static_cast<double>(get<3>(r)); break;
      case 4: acc[4] += static_cast<double>(get<4>(r)); break;
      case 5: acc[5] += static_cast<double>(get<5>(r)); break;
      case 6: acc[6] += get<6>(r); break;
      default: acc[7] += static_cast<double>(get<7>(r)); break;
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

// 运行时迭代器：begin() 之后走 k 步 ++，再读。
template <std::size_t A>
__attribute__((noinline)) double g2_range_advance(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t const k = i & (field_count - 1);
    range_at<A> const r(buf + i * frame_bytes_at<A>);
    auto it = r.begin();
    for (std::size_t j = 0; j < k; ++j) {
      ++it;
    }
    auto& p = *it;
    switch (k) {
      case 0: acc[0] += static_cast<double>(static_cast<u32 const&>(p)); break;
      case 1: acc[1] += static_cast<double>(static_cast<u8 const&>(p)); break;
      case 2: acc[2] += static_cast<double>(static_cast<u8 const&>(p)); break;
      case 3: acc[3] += static_cast<double>(static_cast<u16 const&>(p)); break;
      case 4: acc[4] += static_cast<double>(static_cast<u32 const&>(p)); break;
      case 5: acc[5] += static_cast<double>(static_cast<u64 const&>(p)); break;
      case 6: acc[6] += static_cast<double>(static_cast<double const&>(p)); break;
      default: acc[7] += static_cast<double>(static_cast<u32 const&>(p)); break;
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

// ---------------------------------------------------------------------------
// G3：没有任何位置信息，逐槽位判定类型（通用 visitor 形状）
// ---------------------------------------------------------------------------

__attribute__((noinline)) double g3_any(dataset& d) {
  double acc[field_count]{};
  for (auto const& r : d.anys) {
    std::size_t k = 0;
    for (auto const& a : r.f) {
      std::type_info const& ti = a.type();
      if (ti == typeid(u32)) {
        acc[k] += static_cast<double>(std::any_cast<u32 const&>(a));
      } else if (ti == typeid(u8)) {
        acc[k] += static_cast<double>(std::any_cast<u8 const&>(a));
      } else if (ti == typeid(u16)) {
        acc[k] += static_cast<double>(std::any_cast<u16 const&>(a));
      } else if (ti == typeid(u64)) {
        acc[k] += static_cast<double>(std::any_cast<u64 const&>(a));
      } else {
        acc[k] += std::any_cast<double const&>(a);
      }
      ++k;
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

__attribute__((noinline)) double g3_variant(dataset& d) {
  double acc[field_count]{};
  for (auto const& r : d.vars) {
    std::size_t k = 0;
    for (auto const& x : r.f) {
      acc[k] += std::visit([](auto y) { return static_cast<double>(y); }, x);
      ++k;
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

template <std::size_t A>
__attribute__((noinline)) double g3_range(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    range_at<A> const r(buf + i * frame_bytes_at<A>);
    std::size_t k = 0;
    for (auto it = r.begin(); it != r.end(); ++it, ++k) {
      auto& p = *it;
      if (mask_bit<u32, u32, u8, u8, u16, u32, u64, double, u32>(k)) {
        acc[k] += static_cast<double>(static_cast<u32 const&>(p));
      } else if (mask_bit<u64, u32, u8, u8, u16, u32, u64, double, u32>(k)) {
        acc[k] += static_cast<double>(static_cast<u64 const&>(p));
      } else if (mask_bit<u16, u32, u8, u8, u16, u32, u64, double, u32>(k)) {
        acc[k] += static_cast<double>(static_cast<u16 const&>(p));
      } else if (mask_bit<u8, u32, u8, u8, u16, u32, u64, double, u32>(k)) {
        acc[k] += static_cast<double>(static_cast<u8 const&>(p));
      } else {
        acc[k] += static_cast<double>(static_cast<double const&>(p));
      }
    }
  }
  return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

// ---------------------------------------------------------------------------
// G4：24 字节 blob（超过 std::any 的 SBO），看构造/读取代价
// ---------------------------------------------------------------------------

struct blob {
  std::array<char, 24> b{};
};

inline blob const& sample_blob() {
  static blob const v = [] {
    blob x;
    for (std::size_t i = 0; i < x.b.size(); ++i) {
      x.b[i] = static_cast<char>(i + 1);
    }
    return x;
  }();
  return v;
}

using heavy_var_t = std::variant<u32, blob, double>;
template <std::size_t A>
using heavy_range_at = type_safe_aligned_range<A, u32, blob, double>;
template <std::size_t A>
inline constexpr std::size_t heavy_bytes_at = field_size<A, u32, blob, double>();

// packed 36 B（u32@0, blob@4, double@28）/ 自然对齐 40 B（u32@0, blob@4, double@32）
using heavy_range_t = heavy_range_at<1>;
inline constexpr std::size_t heavy_bytes = heavy_bytes_at<1>;

struct heavy_any_rec {
  std::array<std::any, 3> f;
};

struct heavy_var_rec {
  std::array<heavy_var_t, 3> f;
};

static_assert(heavy_bytes_at<1> == 36);
static_assert(heavy_bytes_at<0> == 40);

__attribute__((noinline)) double g4_build_any(std::vector<heavy_any_rec>& v) {
  blob const& B = sample_blob();
  for (auto& r : v) {
    // 最直观的写法：先造临时 any，再拷贝赋值 → 每个 blob 会分配两次、释放两次
    r.f[0] = std::any{u32{7}};
    r.f[1] = std::any{B};
    r.f[2] = std::any{double{2.5}};
  }
  return 1.0;
}

__attribute__((noinline)) double g4_build_any_emplace(std::vector<heavy_any_rec>& v) {
  blob const& B = sample_blob();
  for (auto& r : v) {
    r.f[0].emplace<u32>(7);
    r.f[1].emplace<blob>(B);
    r.f[2].emplace<double>(2.5);
  }
  return 1.0;
}

__attribute__((noinline)) double g4_build_variant(std::vector<heavy_var_rec>& v) {
  blob const& B = sample_blob();
  for (auto& r : v) {
    r.f[0].emplace<u32>(7);
    r.f[1].emplace<blob>(B);
    r.f[2].emplace<double>(2.5);
  }
  return 1.0;
}

template <std::size_t A>
__attribute__((noinline)) double g4_build_range(std::byte* buf, std::size_t n) {
  blob const& B = sample_blob();
  for (std::size_t i = 0; i < n; ++i) {
    heavy_range_at<A> const r(buf + i * heavy_bytes_at<A>);
    type_safe_aligned::initialize(r, u32{7}, B, 2.5);
  }
  return 1.0;
}

__attribute__((noinline)) double g4_read_any(std::vector<heavy_any_rec> const& v) {
  double acc[3]{};
  for (auto const& r : v) {
    acc[0] += static_cast<double>(std::any_cast<u32 const&>(r.f[0]));
    acc[1] += static_cast<double>(std::any_cast<blob const&>(r.f[1]).b[0]);
    acc[2] += std::any_cast<double const&>(r.f[2]);
  }
  return acc[0] + acc[1] + acc[2];
}

__attribute__((noinline)) double g4_read_variant(std::vector<heavy_var_rec> const& v) {
  double acc[3]{};
  for (auto const& r : v) {
    acc[0] += static_cast<double>(std::get<u32>(r.f[0]));
    acc[1] += static_cast<double>(std::get<blob>(r.f[1]).b[0]);
    acc[2] += std::get<double>(r.f[2]);
  }
  return acc[0] + acc[1] + acc[2];
}

template <std::size_t A>
__attribute__((noinline)) double g4_read_range(std::byte* buf, std::size_t n) {
  double acc[3]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::byte* base = buf + i * heavy_bytes_at<A>;
    heavy_range_at<A> const r(base);
    acc[0] += static_cast<double>(slot_at<0>(r).get(base));
    acc[1] += static_cast<double>(slot_at<1>(r).get(base).b[0]);
    acc[2] += slot_at<2>(r).get(base);
  }
  return acc[0] + acc[1] + acc[2];
}

// ---------------------------------------------------------------------------
// G5：未对齐到底值多少钱 —— 同一个 u64，只把地址平移 4 / 60 字节
// ---------------------------------------------------------------------------
// 步长 64 B（每格一条 cache line），三列读的 u64 个数完全一样，只差地址：
//   offset 0  ：自然对齐，一行取一个
//   offset 4  ：跨 8 字节边界但仍在同一行内（x86 上应该几乎免费）
//   offset 60 ：每次 8 字节都跨 cache line（应该是真的代价）
// 用 memcpy 取字节再 bit_cast，而不是 reinterpret_cast 解引用：后者在语言层面
// 本来就是未对齐/空指针定义的 UB，编译器有权假设对齐。memcpy(8) 在 -O2 下就是
// 一条普通 mov，不会偷加对齐假设。
__attribute__((noinline)) double g5_load(std::byte const* buf, std::size_t n,
                                        std::size_t offset) {
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < n; ++i) {
    std::array<std::byte, 8> bytes{};
    std::memcpy(bytes.data(), buf + i * 64 + offset, bytes.size());
    acc += std::bit_cast<std::uint64_t>(bytes);
  }
  return static_cast<double>(acc);
}

// ---------------------------------------------------------------------------
// 分组驱动
// ---------------------------------------------------------------------------

std::vector<row> group1(dataset& d, std::size_t n, int reps) {
  double sink = 0;
  auto bench = [&](auto&& f) {
    return best_total_ns(f, reps, sink) / static_cast<double>(n);
  };
  double const floor = bench([&] { return g1_native(d); });
  std::vector<row> rows{{"native struct", floor, 1.0, g1_native(d), true}};
  auto add = [&](char const* name, auto&& f) {
    double const ns = bench(f);
    rows.push_back(row{name, ns, ns / floor, f(), true});
  };
  add("std::any", [&] { return g1_any(d); });
  add("std::variant", [&] { return g1_variant(d); });
  add("range<1> slot_at<I>", [&] { return g1_range<1>(d.packed.data(), n); });
  add("range<0> slot_at<I>", [&] { return g1_range<0>(d.aligned.data(), n); });
  add("range<1> get<I>", [&] { return g1_range_get<1>(d.packed.data(), n); });
  add("range<0> get<I>", [&] { return g1_range_get<0>(d.aligned.data(), n); });
  keep(sink);
  return rows;
}

std::vector<row> group2(dataset& d, std::size_t n, int reps) {
  double sink = 0;
  auto bench = [&](auto&& f) {
    return best_total_ns(f, reps, sink) / static_cast<double>(n);
  };
  double const floor = bench([&] { return g2_native(d, n); });
  std::vector<row> rows{{"native switch", floor, 1.0, g2_native(d, n), true}};
  auto add = [&](char const* name, auto&& f) {
    double const ns = bench(f);
    rows.push_back(row{name, ns, ns / floor, f(), true});
  };
  add("any switch", [&] { return g2_any(d, n); });
  add("variant switch", [&] { return g2_variant(d, n); });
  add("range<1> iter ctor(k)",
      [&] { return g2_range_ctor<1>(d.packed.data(), n); });
  add("range<0> iter ctor(k)",
      [&] { return g2_range_ctor<0>(d.aligned.data(), n); });
  add("range<1> op[] proxy",
      [&] { return g2_range_index<1>(d.packed.data(), n); });
  add("range<0> op[] proxy",
      [&] { return g2_range_index<0>(d.aligned.data(), n); });
  add("range<1> iter ++ x k",
      [&] { return g2_range_advance<1>(d.packed.data(), n); });
  add("range<0> iter ++ x k",
      [&] { return g2_range_advance<0>(d.aligned.data(), n); });
  add("range<1> switch->get<I>",
      [&] { return g2_range_switch_get<1>(d.packed.data(), n); });
  add("range<0> switch->get<I>",
      [&] { return g2_range_switch_get<0>(d.aligned.data(), n); });
  keep(sink);
  return rows;
}

std::vector<row> group3(dataset& d, std::size_t n, int reps) {
  double sink = 0;
  auto bench = [&](auto&& f) {
    return best_total_ns(f, reps, sink) / static_cast<double>(n);
  };
  double const floor = bench([&] { return g1_native(d); });
  std::vector<row> rows{{"native struct", floor, 1.0, g1_native(d), true}};
  auto add = [&](char const* name, auto&& f) {
    double const ns = bench(f);
    rows.push_back(row{name, ns, ns / floor, f(), true});
  };
  add("any typeid chain", [&] { return g3_any(d); });
  add("variant visit", [&] { return g3_variant(d); });
  add("range<1> mask+iter", [&] { return g3_range<1>(d.packed.data(), n); });
  add("range<0> mask+iter", [&] { return g3_range<0>(d.aligned.data(), n); });
  keep(sink);
  return rows;
}

std::vector<row> group4(std::size_t n, int reps) {
  double sink = 0;
  auto bench = [&](auto&& f) {
    return best_total_ns(f, reps, sink) / static_cast<double>(n);
  };

  std::vector<heavy_any_rec> anys(n);
  std::vector<heavy_var_rec> vars(n);
  std::vector<std::byte> packed(n * heavy_bytes_at<1>);
  std::vector<std::byte> aligned(n * heavy_bytes_at<0>);

  // 构造：以 range 为地板（placement new，无分配）
  double const build_floor = bench([&] { return g4_build_range<1>(packed.data(), n); });
  std::vector<row> rows{{"range<1> build", build_floor, 1.0, 0.0, false}};
  auto add = [&](char const* name, auto&& f) {
    double const ns = bench(f);
    rows.push_back(row{name, ns, ns / build_floor, 0.0, false});
  };
  add("any build (temp+assign)", [&] { return g4_build_any(anys); });
  add("any build (emplace)", [&] { return g4_build_any_emplace(anys); });
  add("variant build (emplace)", [&] { return g4_build_variant(vars); });
  add("range<0> build", [&] { return g4_build_range<0>(aligned.data(), n); });

  // 读取：以 packed range 为地板
  double const read_floor = bench([&] { return g4_read_range<1>(packed.data(), n); });
  rows.push_back(row{"--- range<1> read", read_floor, 1.0,
                   g4_read_range<1>(packed.data(), n), true});
  auto add_read = [&](char const* name, auto&& f) {
    double const ns = bench(f);
    rows.push_back(row{name, ns, ns / read_floor, f(), true});
  };
  add_read("any read", [&] { return g4_read_any(anys); });
  add_read("variant read", [&] { return g4_read_variant(vars); });
  add_read("range<0> read", [&] { return g4_read_range<0>(aligned.data(), n); });

  keep(sink);
  return rows;
}

std::vector<row> group5(std::size_t n, int reps) {
  double sink = 0;
  std::vector<std::byte> buf(n * 64 + 64);
  for (std::size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<std::byte>(i * 31 + 7);
  }
  auto bench = [&](std::size_t offset) {
    return best_total_ns([&] { return g5_load(buf.data(), n, offset); }, reps,
                         sink) /
           static_cast<double>(n);
  };
  double const line0 = bench(0);
  std::vector<row> rows{{"u64 @ offset 0", line0, 1.0, 0.0, false}};
  for (std::size_t off : {std::size_t{4}, std::size_t{60}}) {
    double const ns = bench(off);
    char const* name = off == 4 ? "u64 @ offset 4" : "u64 @ offset 60";
    rows.push_back(row{name, ns, ns / line0, 0.0, false});
  }
  keep(sink);
  return rows;
}

// 每记录只读 1 个字段：计算量降到最低，循环变成纯搬运。
// 累加器用整数而不是 double：FP 加法的 4 周期延迟会自己变成瓶颈（约 1.3 ns/记录），
// 那就又测不到带宽了。
__attribute__((noinline)) double g6_thin_native(dataset& d) {
  std::uint64_t acc = 0;
  for (auto const& r : d.native) {
    acc += r.magic;
  }
  return static_cast<double>(acc);
}

template <std::size_t A>
__attribute__((noinline)) double g6_thin_range(std::byte* buf, std::size_t n) {
  std::uint64_t acc = 0;
  for (std::size_t i = 0; i < n; ++i) {
    range_at<A> const r(buf + i * frame_bytes_at<A>);
    acc += get<0>(r);
  }
  return static_cast<double>(acc);
}

__attribute__((noinline)) double g6_thin_variant(dataset& d) {
  std::uint64_t acc = 0;
  for (auto const& r : d.vars) {
    acc += std::get<u32>(r.f[0]);
  }
  return static_cast<double>(acc);
}

__attribute__((noinline)) double g6_thin_any(dataset& d) {
  std::uint64_t acc = 0;
  for (auto const& r : d.anys) {
    acc += std::any_cast<u32 const&>(r.f[0]);
  }
  return static_cast<double>(acc);
}

// ---------------------------------------------------------------------------
// G6：空间密度（工作集 >> L3，同样的记录数、不同的每记录字节数）
// ---------------------------------------------------------------------------
// 和 G1 是同一批循环，只是把记录数拉到让整个数组装不进 cache：
// 这时瓶颈从“每字段的派发/检查”变成“搬运了多少字节”，每记录 32 B 与 128 B
// 的差别就直接变成 4 倍带宽。
//
// GB/s 用“每记录字节数 / 每记录纳秒数”算（1 byte/ns == 1 GB/s）。
// 它不是内存控制器吞吐，而是“这个布局实际要从内存里搬的字节数”的换算，
// 所以可以直接拿来比密度。
void group6(std::size_t n, int reps) {
  dataset d = make_dataset(n);
  double sink = 0;
  auto bench = [&](auto&& f) {
    return best_total_ns(f, reps, sink) / static_cast<double>(n);
  };

  struct g6row {
    char const* name;
    double ns_per_rec;
    std::size_t bytes_per_rec;
  };
  std::vector<g6row> rows;
  double const floor = bench([&] { return g1_range<1>(d.packed.data(), n); });
  rows.push_back({"range<1> packed", floor, frame_bytes_at<1>});
  rows.push_back({"native struct", bench([&] { return g1_native(d); }),
                  sizeof(native_rec)});
  rows.push_back({"range<0> aligned",
                  bench([&] { return g1_range<0>(d.aligned.data(), n); }),
                  frame_bytes_at<0>});
  rows.push_back({"std::variant", bench([&] { return g1_variant(d); }),
                  sizeof(var_rec)});
  rows.push_back({"std::any", bench([&] { return g1_any(d); }),
                  sizeof(any_rec)});
  keep(sink);

  std::printf("\n== G6 space density: same record count, working set >> L3 ==\n");
  std::printf("%-20s %10s %10s %9s %10s\n", "variant", "ns/rec", "vs floor",
              "B/rec", "GB/s");
  for (auto const& r : rows) {
    double const gb_s = static_cast<double>(r.bytes_per_rec) / r.ns_per_rec;
    std::printf("%-20s %10.3f %10.2f %9zu %10.2f\n", r.name, r.ns_per_rec,
                r.ns_per_rec / floor, r.bytes_per_rec, gb_s);
  }

  // 第二张表：每记录只读 1 个字段 —— 计算量足够小，循环变成纯搬运。
  std::vector<g6row> thin;
  double const thin_floor =
      bench([&] { return g6_thin_range<1>(d.packed.data(), n); });
  thin.push_back({"range<1> packed", thin_floor, frame_bytes_at<1>});
  thin.push_back({"native struct", bench([&] { return g6_thin_native(d); }),
                  sizeof(native_rec)});
  thin.push_back({"range<0> aligned",
                  bench([&] { return g6_thin_range<0>(d.aligned.data(), n); }),
                  frame_bytes_at<0>});
  thin.push_back({"std::variant", bench([&] { return g6_thin_variant(d); }),
                  sizeof(var_rec)});
  thin.push_back({"std::any", bench([&] { return g6_thin_any(d); }),
                  sizeof(any_rec)});

  std::printf("\n-- thin scan: one u32 field per record --\n");
  std::printf("%-20s %10s %10s %9s %10s\n", "variant", "ns/rec", "vs floor",
              "B/rec", "GB/s");
  for (auto const& r : thin) {
    double const gb_s = static_cast<double>(r.bytes_per_rec) / r.ns_per_rec;
    std::printf("%-20s %10.3f %10.2f %9zu %10.2f\n", r.name, r.ns_per_rec,
                r.ns_per_rec / thin_floor, r.bytes_per_rec, gb_s);
  }
  std::printf("working set: packed=%zu MiB variant/any=%zu MiB\n",
              (n * frame_bytes_at<1>) >> 20, (n * sizeof(var_rec)) >> 20);
}

// ---------------------------------------------------------------------------
// G7：T 已知、index 仍来自运行期数组；不把受检访问换成 get<I>。
// ---------------------------------------------------------------------------

template <class T, bool Fused>
__attribute__((noinline)) std::uint64_t g7_range_typed(
    std::byte* buf, std::uint8_t const* indices, std::size_t n, unsigned passes) {
  std::uint64_t sums[4]{};
  for (unsigned pass = 0; pass < passes; ++pass) {
    keep(buf);  // memory clobber：每轮重新读取对象和索引，不外提重复扫描。
    for (std::size_t i = 0; i < n; ++i) {
      range_at<0> const range(buf + i * frame_bytes_at<0>);
      if constexpr (Fused)
        sums[i & 3] += range.template at<T const>(indices[i]);
      else
        sums[i & 3] += static_cast<T const&>(range[indices[i]]);
    }
  }
  return sums[0] + sums[1] + sums[2] + sums[3];
}

template <class T>
__attribute__((noinline)) std::uint64_t g7_variant_typed(
    var_rec const* records, std::uint8_t const* indices,
    std::size_t n, unsigned passes) {
  std::uint64_t sums[4]{};
  for (unsigned pass = 0; pass < passes; ++pass) {
    keep(records);
    for (std::size_t i = 0; i < n; ++i)
      sums[i & 3] += std::get<T>(records[i].f[indices[i]]);
  }
  return sums[0] + sums[1] + sums[2] + sums[3];
}

template <class T>
void g7_measure(char const* name, dataset& data,
                std::vector<std::uint8_t> const& indices) {
  constexpr unsigned passes = 32;
  constexpr std::size_t samples = 15;
  auto const n = indices.size();
  auto proxy = [&] {
    return g7_range_typed<T, false>(data.aligned.data(), indices.data(), n, passes);
  };
  auto fused = [&] {
    return g7_range_typed<T, true>(data.aligned.data(), indices.data(), n, passes);
  };
  auto variant = [&] {
    return g7_variant_typed<T>(data.vars.data(), indices.data(), n, passes);
  };
  auto const expected = proxy();
  if (fused() != expected || variant() != expected)
    throw std::runtime_error("G7 warmup checksum mismatch");
  std::array<std::array<double, samples>, 3> times{};
  auto measure = [&](auto&& operation) {
    auto const start = clock_type::now();
    auto const result = operation();
    keep(result);
    auto const end = clock_type::now();
    if (result != expected)
      throw std::runtime_error("G7 checksum mismatch");
    return std::chrono::duration<double, std::nano>(end - start).count()
        / static_cast<double>(n * passes);
  };
  for (std::size_t rep = 0; rep < samples; ++rep) {
    for (std::size_t order = 0; order < times.size(); ++order) {
      switch ((rep + order) % times.size()) {
        case 0: times[0][rep] = measure(proxy); break;
        case 1: times[1][rep] = measure(fused); break;
        case 2: times[2][rep] = measure(variant); break;
      }
    }
  }
  for (auto& sample : times) std::sort(sample.begin(), sample.end());
  auto const before = times[0][samples / 2];
  auto const after = times[1][samples / 2];
  std::printf("%-15s proxy=%7.3f at<T>=%7.3f variant=%7.3f ns/access"
              "  reduction=%6.1f%%  checksum=%llu OK\n",
              name, before, after, times[2][samples / 2],
              100.0 * (1.0 - after / before),
              static_cast<unsigned long long>(expected));
}

void group7(dataset& data, std::size_t n) {
  std::vector<std::uint8_t> unique(n, 5);
  std::vector<std::uint8_t> repeated(n);
  constexpr std::uint8_t u32_slots[]{0, 4, 7};
  std::uint32_t state = 987654321;
  for (std::size_t i = 0; i < n; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    range_at<0> range(data.aligned.data() + i * frame_bytes_at<0>);
    range.get<0>() = state & 0xffff;
    range.get<4>() = (state >> 8) & 0xffff;
    range.get<7>() = (state >> 16) & 0xffff;
    range.get<5>() = state;
    data.vars[i].f[0] = range.get<0>();
    data.vars[i].f[4] = range.get<4>();
    data.vars[i].f[7] = range.get<7>();
    data.vars[i].f[5] = range.get<5>();
    repeated[i] = u32_slots[state % 3];
  }
  std::printf("\n== G7 checked typed runtime index: records=%zu,"
              " median of 15, 32 passes ==\n", n);
  g7_measure<u64>("unique u64", data, unique);
  g7_measure<u32>("repeated u32", data, repeated);
}

template <unsigned Mode>
__attribute__((noinline)) double g8_numeric(
    std::byte* buf, std::uint8_t const* indices, std::size_t n, unsigned passes) {
  double sums[4]{};
  for (unsigned pass = 0; pass < passes; ++pass) {
    keep(buf);
    for (std::size_t i = 0; i < n; ++i) {
      range_at<0> const range(buf + i * frame_bytes_at<0>);
      double value;
      if constexpr (Mode == 0) {
        value = static_cast<double>(range.get<5>()) + 7;
      } else if constexpr (Mode == 1) {
        value = static_cast<double>(range[indices[i]].as<u64 const>()) + 7;
      } else if constexpr (Mode == 2) {
        value = range[indices[i]] + u64{7};
      } else {
        switch (indices[i]) {
          case 0: value = static_cast<double>(range.get<0>()) + 7; break;
          case 1: value = static_cast<double>(range.get<1>()) + 7; break;
          case 2: value = static_cast<double>(range.get<2>()) + 7; break;
          case 3: value = static_cast<double>(range.get<3>()) + 7; break;
          case 4: value = static_cast<double>(range.get<4>()) + 7; break;
          case 5: value = static_cast<double>(range.get<5>()) + 7; break;
          case 6: value = range.get<6>() + 7; break;
          default: value = static_cast<double>(range.get<7>()) + 7; break;
        }
      }
      sums[i & 3] += value;
    }
  }
  return sums[0] + sums[1] + sums[2] + sums[3];
}

void group8(dataset& data, std::size_t n) {
  constexpr unsigned passes = 32;
  constexpr std::size_t samples = 15;
  std::vector<std::uint8_t> unique(n, 5);
  std::vector<std::uint8_t> mixed(n);
  std::uint32_t state = 987654321;
  for (std::size_t i = 0; i < n; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    range_at<0> range(data.aligned.data() + i * frame_bytes_at<0>);
    range.get<5>() = state;
    mixed[i] = state & 7;
  }
  using scan_fn = double (*)(std::byte*, std::uint8_t const*, std::size_t, unsigned);
  constexpr scan_fn scans[]{g8_numeric<0>, g8_numeric<1>, g8_numeric<2>,
                             g8_numeric<3>, g8_numeric<2>};
  constexpr char const* names[]{"get<5> fixed", "as<u64> fixed", "op table fixed",
                                "get switch mixed", "op table mixed"};
  auto run = [&](std::size_t mode) {
    return scans[mode](data.aligned.data(),
                       mode < 3 ? unique.data() : mixed.data(), n, passes);
  };
  const double fixed_expected = run(0);
  const double mixed_expected = run(3);
  std::array<std::array<double, samples>, 5> times{};
  for (std::size_t rep = 0; rep < samples; ++rep) {
    for (std::size_t order = 0; order < times.size(); ++order) {
      const auto mode = (rep + order) % times.size();
      const auto start = clock_type::now();
      const double result = run(mode);
      keep(result);
      const auto end = clock_type::now();
      if (result != (mode < 3 ? fixed_expected : mixed_expected))
        throw std::runtime_error("G8 checksum mismatch");
      times[mode][rep] =
          std::chrono::duration<double, std::nano>(end - start).count()
          / static_cast<double>(n * passes);
    }
  }
  std::printf("\n== G8 numeric access: records=%zu, median of 15, 32 passes ==\n", n);
  for (std::size_t mode = 0; mode < times.size(); ++mode) {
    std::sort(times[mode].begin(), times[mode].end());
    std::printf("%-20s %7.3f ns/access checksum=%.0f OK\n",
                names[mode], times[mode][samples / 2],
                mode < 3 ? fixed_expected : mixed_expected);
  }
}

// ---------------------------------------------------------------------------

bool agree(double a, double b) {
  double const scale = std::max(1.0, std::max(std::abs(a), std::abs(b)));
  return std::abs(a - b) <= 1e-9 * scale;
}

void print_rows(char const* title, std::vector<row> const& rows,
                std::size_t ops_per_rec) {
  std::printf("\n== %s ==\n", title);
  std::printf("%-24s %10s %9s %9s\n", "variant", "ns/rec", "vs floor", "ns/op");
  bool first = true;
  double ref = 0;
  bool ok = true;
  for (auto const& r : rows) {
    if (r.check) {
      if (first) {
        ref = r.checksum;
        first = false;
      } else if (!agree(ref, r.checksum)) {
        ok = false;
      }
    }
    std::printf("%-24s %10.3f %9.2f %9.3f\n", r.name, r.ns_per_rec,
                r.vs_floor, r.ns_per_rec / static_cast<double>(ops_per_rec));
  }
  if (!ok) {
    std::printf("!! checksum mismatch between rows\n");
  }
}

void run_all(std::size_t n, int reps) {
  std::printf("\n######## records=%zu  (%zu KiB packed)  reps=%d ########", n,
              (n * packed_bytes) >> 10, reps);

  dataset d = make_dataset(n);

  auto base_mod = [](std::vector<std::byte> const& v) {
    return reinterpret_cast<std::uintptr_t>(v.data()) % 64;
  };
  std::printf("\nbuffer base %% 64: packed=%zu aligned=%zu\n", base_mod(d.packed),
              base_mod(d.aligned));

  print_rows("G1 fixed positions: native / any / variant / range slot_at<I>",
             group1(d, n, reps), field_count);
  print_rows("G2 runtime index (one field per record)",
             group2(d, n, reps), 1);
  print_rows("G3 no position info: dynamic type dispatch over all fields",
             group3(d, n, reps), field_count);
  print_rows("G4 24-byte blob: build then read", group4(n, reps), 3);
  print_rows("G5 what misalignment really costs (u64, 64 B stride)",
             group5(n, reps), 1);
  group7(d, n);
  group8(d, n);
}

}  // namespace bench

int main(int argc, char** argv) {
  using namespace bench;

  if (argc == 2 && std::string_view(argv[1]) == "--typed-only") {
    for (std::size_t n : {std::size_t{1} << 12, std::size_t{1} << 16}) {
      auto data = make_dataset(n);
      group7(data, n);
    }
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--runtime-only") {
    for (std::size_t n : {std::size_t{1} << 12, std::size_t{1} << 16}) {
      auto data = make_dataset(n);
      group8(data, n);
    }
    return 0;
  }

  std::printf("sizeof: any=%zu variant=%zu native_rec=%zu packed=%zu aligned=%zu\n",
              sizeof(std::any), sizeof(var_t), sizeof(native_rec),
              static_cast<std::size_t>(packed_bytes),
              static_cast<std::size_t>(frame_bytes_at<0>));
  std::printf("bytes per record: any=%zu variant=%zu native=%zu packed=%zu\n",
              sizeof(any_rec), sizeof(var_rec), sizeof(native_rec),
              sizeof(std::array<std::byte, packed_bytes>));

  run_all(1u << 12, 2000);  // 4096 记录：工作集 ~0.5 MiB，常驻 L2
  run_all(1u << 16, 25);    // 65536 记录：工作集 ~8 MiB，压到 L3/内存
  group6(1u << 20, 25);     // 1M 记录：packed 32 MiB vs variant 128 MiB
  return 0;
}
