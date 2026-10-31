// 异构内存访问基准：std::any / std::variant / type_safe_aligned_range 的运行时开销
//
// 同一条记录（8 个异构字段 u32,u8,u8,u16,u32,u64,double,u32，packed 32 字节）
// 用四种方式存：
//
//   native    : std::vector<native_rec>              每个字段就是它的天然类型（40 B/记录）
//   any       : std::vector<array<any,8>>            每字段一个 std::any（128 B/记录）
//   variant   : std::vector<array<variant<...>,8>>   每字段一个 variant（128 B/记录）
//   packed    : 一整块 std::byte 缓冲                type_safe_aligned_range<1, ...>（32 B/记录）
//
// 四组测量，回答不同问题：
//
//   G1 定点访问    字段位置是编译期常量（正常解码循环的形状）
//                  native / any / variant / range-slot_at<I>
//                  → any/variant 的每字段代价 = typeid 比较 / 下标比较 + 抛异常分支
//                  → range 的 slot_at<I> 是 base+常量偏移，零检查
//
//   G2 运行期下标  位置是运行期值（k = i & 7），每记录只读 1 个字段
//                  native-switch / any-switch / variant-switch
//                  range-ctor（O(1) 定位）/ range-advance（begin 之后走 k 步）
//                  → 这就是“运行时迭代器”的真实开销
//
//   G3 无位置信息  不知道位置，逐个槽位动态判定类型后再读（通用 visitor 形状）
//                  native / any（typeid 链）/ variant（visit 跳表）
//                  range（type_mask_series 位图链 + 运行时迭代器）
//
//   G4 大载荷      字段里放 24 字节 blob（超过 std::any 的 SBO）
//                  any / variant / range 的构造开销与读取开销
//                  → any 的唯一一次堆分配在这里显形
//
//   g++ -std=c++23 -O2 -I. -o bench_hetero bench/hetero_access_bench.cpp
//
// 指标：ns/记录（多次取最小）；vs 列相对本组地板；ns/op = ns/记录 / 字段访问次数。

#include "meta_ios.hpp"
#include "type_safe_aligned_iterator.hpp"

#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
inline constexpr std::size_t packed_bytes = 32;

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

// 与协议同序的 type_safe_aligned_range：Alignment = 1（packed），32 字节。
using range_t = type_safe_aligned_range<1, u32, u8, u8, u16, u32, u64, double, u32>;
using iter_t = type_safe_aligned_iterator<1, u32, u8, u8, u16, u32, u64, double, u32>;

static_assert(field_size<1, u32, u8, u8, u16, u32, u64, double, u32>() ==
              packed_bytes);

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
  std::vector<std::byte> packed;
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
  d.packed.resize(n * packed_bytes);
  for (std::size_t i = 0; i < n; ++i) {
    range_t const r(d.packed.data() + i * packed_bytes);
    type_safe_aligned::initialize(r, v.magic, v.version, v.kind, v.flags,
                                  v.device, v.timestamp, v.value, v.length);
  }
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

__attribute__((noinline)) double g1_range(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::byte* base = buf + i * packed_bytes;
    range_t const r(base);
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
__attribute__((noinline)) double g2_range_ctor(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t const k = i & (field_count - 1);
    iter_t it(buf + i * packed_bytes, k);
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

// 运行时迭代器：begin() 之后走 k 步 ++，再读。
__attribute__((noinline)) double g2_range_advance(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t const k = i & (field_count - 1);
    range_t const r(buf + i * packed_bytes);
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

__attribute__((noinline)) double g3_range(std::byte* buf, std::size_t n) {
  double acc[field_count]{};
  for (std::size_t i = 0; i < n; ++i) {
    range_t const r(buf + i * packed_bytes);
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
using heavy_range_t = type_safe_aligned_range<1, u32, blob, double>;
inline constexpr std::size_t heavy_bytes = 36;

struct heavy_any_rec {
  std::array<std::any, 3> f;
};

struct heavy_var_rec {
  std::array<heavy_var_t, 3> f;
};

static_assert(field_size<1, u32, blob, double>() == heavy_bytes);

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

__attribute__((noinline)) double g4_build_range(std::byte* buf, std::size_t n) {
  blob const& B = sample_blob();
  for (std::size_t i = 0; i < n; ++i) {
    heavy_range_t const r(buf + i * heavy_bytes);
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

__attribute__((noinline)) double g4_read_range(std::byte* buf, std::size_t n) {
  double acc[3]{};
  for (std::size_t i = 0; i < n; ++i) {
    std::byte* base = buf + i * heavy_bytes;
    heavy_range_t const r(base);
    acc[0] += static_cast<double>(slot_at<0>(r).get(base));
    acc[1] += static_cast<double>(slot_at<1>(r).get(base).b[0]);
    acc[2] += slot_at<2>(r).get(base);
  }
  return acc[0] + acc[1] + acc[2];
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
  add("range slot_at<I>", [&] { return g1_range(d.packed.data(), n); });
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
  add("range iter ctor(k)", [&] { return g2_range_ctor(d.packed.data(), n); });
  add("range iter ++ x k", [&] { return g2_range_advance(d.packed.data(), n); });
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
  add("range mask+iter", [&] { return g3_range(d.packed.data(), n); });
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
  std::vector<std::byte> packed(n * heavy_bytes);

  // 构造：以 range 为地板（placement new，无分配）
  double const build_floor =
      bench([&] { return g4_build_range(packed.data(), n); });
  std::vector<row> rows{{"range build", build_floor, 1.0, 0.0, false}};
  auto add = [&](char const* name, auto&& f) {
    double const ns = bench(f);
    rows.push_back(row{name, ns, ns / build_floor, 0.0, false});
  };
  add("any build (temp+assign)", [&] { return g4_build_any(anys); });
  add("any build (emplace)", [&] { return g4_build_any_emplace(anys); });
  add("variant build (emplace)", [&] { return g4_build_variant(vars); });

  // 读取：以 range 为地板
  double const read_floor =
      bench([&] { return g4_read_range(packed.data(), n); });
  rows.push_back(row{"--- range read", read_floor, 1.0, g4_read_range(packed.data(), n), true});
  auto add_read = [&](char const* name, auto&& f) {
    double const ns = bench(f);
    rows.push_back(row{name, ns, ns / read_floor, f(), true});
  };
  add_read("any read", [&] { return g4_read_any(anys); });
  add_read("variant read", [&] { return g4_read_variant(vars); });

  keep(sink);
  return rows;
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

  print_rows("G1 fixed positions: native / any / variant / range slot_at<I>",
             group1(d, n, reps), field_count);
  print_rows("G2 runtime index (one field per record)",
             group2(d, n, reps), 1);
  print_rows("G3 no position info: dynamic type dispatch over all fields",
             group3(d, n, reps), field_count);
  print_rows("G4 24-byte blob: build then read", group4(n, reps), 3);
}

}  // namespace bench

int main() {
  using namespace bench;

  std::printf("sizeof: any=%zu variant=%zu native_rec=%zu packed=%zu\n",
              sizeof(std::any), sizeof(var_t), sizeof(native_rec),
              static_cast<std::size_t>(packed_bytes));
  std::printf("bytes per record: any=%zu variant=%zu native=%zu packed=%zu\n",
              sizeof(any_rec), sizeof(var_rec), sizeof(native_rec),
              sizeof(std::array<std::byte, packed_bytes>));

  run_all(1u << 12, 200);   // 4096 记录：工作集 ~0.5 MiB，常驻 L2
  run_all(1u << 16, 25);    // 65536 记录：工作集 ~8 MiB，压到 L3/内存
  return 0;
}
