// type_safe_aligned_range 的两套下标：
//
//   运行期 r[i]        -> aligned_iterator_storage<Ty...> 受检代理（按值，越界抛）
//   编译期 get<I>(r)   -> 该槽位的精确类型引用（base + 常量偏移，零检查）
//   r.get<I>()         -> 同上（结构化绑定走的成员形式）
//   结构化绑定         -> tuple-like 协议：auto& [a, b, ...] = r;
//
// 两者并存不冲突：std::array 就是 operator[] + get<I> 共存的形状。
// 区别只在于我们这一格是异构的，运行期拿不到 Ty_i&，只能给代理。
//
// g++ -std=c++23 -O0 -I. -o aligned_index examples/aligned_index_smoke.cpp

#include "type_safe_aligned_iterator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <tuple>
#include <type_traits>
#include <utility>

using namespace type_safe_aligned;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// 与 wire_socket_smoke / hetero_access_bench 同一条 32 字节协议帧。
using frame_t = type_safe_aligned_range<1, u32, u8, u8, u16, u32, u64, double, u32>;

static_assert(field_size<1, u32, u8, u8, u16, u32, u64, double, u32>() == 32);

// ---- tuple-like 协议 ----
static_assert(std::tuple_size_v<frame_t> == 8);
static_assert(std::tuple_size_v<frame_t const> == 8);
static_assert(std::is_same_v<std::tuple_element_t<0, frame_t>, u32>);
static_assert(std::is_same_v<std::tuple_element_t<5, frame_t>, u64>);
static_assert(std::is_same_v<std::tuple_element_t<6, frame_t>, double>);
// 视图语义：const range 的元素类型不带 const（与 get<I>() 返回 T& 一致）。
static_assert(std::is_same_v<std::tuple_element_t<0, frame_t const>, u32>);

// ---- get<I> 的返回类型就是精确引用 ----
static_assert(std::is_same_v<decltype(std::declval<frame_t const&>().get<0>()),
                             u32&>);
static_assert(std::is_same_v<decltype(get<5>(std::declval<frame_t const&>())),
                             u64&>);

// ---- 引用槽位：布局按 reference_wrapper 算，tuple_element 也照它给 ----
using ref_range_t = type_safe_aligned_range<0, int&, double const&>;
static_assert(std::is_same_v<std::tuple_element_t<0, ref_range_t>,
                             std::reference_wrapper<int>>);
static_assert(std::is_same_v<std::tuple_element_t<1, ref_range_t>,
                             std::reference_wrapper<double const>>);
static_assert(std::tuple_size_v<ref_range_t> == 2);
static_assert(field_size<0, int&, double const&>() == 2 * sizeof(void*));

int main() {
  bool ok = true;

  alignas(8) std::byte buf[32]{};
  frame_t range(buf);
  type_safe_aligned::initialize(range, u32{0xCAFEBABE}, u8{1}, u8{2}, u16{0},
                                u32{0x123}, u64{0x17090}, 23.5, u32{0});

  // ---- 1) 编译期下标：精确类型、可写 ----
  ok = ok && range.get<0>() == 0xCAFEBABEu;
  ok = ok && get<5>(range) == 0x17090u;
  ok = ok && get<6>(range) == 23.5;
  get<6>(range) = 24.5;  // 直接就是 double&
  ok = ok && range.get<6>() == 24.5;

  // ---- 2) 运行期下标：均匀代理 ----
  auto expected = [&](std::size_t k) {
    switch (k) {
      case 0: return static_cast<double>(get<0>(range));
      case 1: return static_cast<double>(get<1>(range));
      case 2: return static_cast<double>(get<2>(range));
      case 3: return static_cast<double>(get<3>(range));
      case 4: return static_cast<double>(get<4>(range));
      case 5: return static_cast<double>(get<5>(range));
      case 6: return get<6>(range);
      default: return static_cast<double>(get<7>(range));
    }
  };
  for (std::size_t k = 0; k < frame_t::size(); ++k) {
    auto p = range[k];  // 按值拷贝出来，不存在悬垂引用
    double actual = 0;
    switch (k) {
      case 0: actual = static_cast<double>(static_cast<u32 const&>(p)); break;
      case 1: actual = static_cast<double>(static_cast<u8 const&>(p)); break;
      case 2: actual = static_cast<double>(static_cast<u8 const&>(p)); break;
      case 3: actual = static_cast<double>(static_cast<u16 const&>(p)); break;
      case 4: actual = static_cast<double>(static_cast<u32 const&>(p)); break;
      case 5: actual = static_cast<double>(static_cast<u64 const&>(p)); break;
      case 6: actual = static_cast<double>(static_cast<double const&>(p)); break;
      default: actual = static_cast<double>(static_cast<u32 const&>(p)); break;
    }
    ok = ok && actual == expected(k);
  }

  // 运行期下标也能写：写入后编译期读回来
  range[7] = u32{99};
  ok = ok && get<7>(range) == 99u;

  // ---- 3) 越界：operator[] 抛 index_exceed ----
  bool threw = false;
  try {
    (void)range[frame_t::size()];
  } catch (aligned_iterator_storage<u32, u8, u8, u16, u32, u64, double,
                                   u32>::exception const& e) {
    threw = e.code() ==
            aligned_iterator_storage<u32, u8, u8, u16, u32, u64, double,
                                     u32>::error::index_exceed;
  }
  ok = ok && threw;

  // ---- 4) 结构化绑定（tuple-like 协议）----
  {
    auto& [magic, version, kind, flags, device, timestamp, value, length] =
        range;
    ok = ok && magic == 0xCAFEBABEu && version == 1 && kind == 2 && flags == 0;
    ok = ok && device == 0x123u && timestamp == 0x17090u;
    ok = ok && value == 24.5 && length == 99u;
    value = 25.5;  // 绑的是缓冲区里的对象，写回可见
  }
  ok = ok && get<6>(range) == 25.5;

  // const 视图上同样可绑（元素仍可写，与 get<I>() 一致）
  {
    frame_t const& crange = range;
    auto const& [m2, v2, k2, f2, d2, t2, val2, len2] = crange;
    ok = ok && m2 == 0xCAFEBABEu && v2 == 1 && val2 == 25.5;
    (void)k2;
    (void)f2;
    (void)d2;
    (void)t2;
    (void)len2;
  }

  // ---- 5) 引用槽位 ----
  int a = 5;
  double d = 1.5;
  alignas(8) std::byte rbuf[2 * sizeof(void*)]{};
  ref_range_t refs(rbuf);
  type_safe_aligned::initialize(refs, std::ref(a), std::cref(d));
  get<0>(refs).get() = 7;
  ok = ok && a == 7 && std::cref(d).get() == 1.5;

  std::printf("aligned_index_smoke: %s\n", ok ? "ok" : "FAILED");
  return ok ? 0 : 1;
}
