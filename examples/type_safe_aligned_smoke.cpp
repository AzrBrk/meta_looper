#include "type_safe_aligned_iterator.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

using namespace type_safe_aligned;

namespace type_safe_tests {

struct alignas(32) big_align {
  std::string s;
  double d;
};

// 用于验证 initialize 的异常清理：存活计数必须回到 0。
struct tracked {
  static inline int alive = 0;
  static inline int destroyed = 0;

  int v;
  explicit tracked(int x) : v{x} { ++alive; }
  tracked(tracked const& o) : v{o.v} { ++alive; }
  tracked(tracked&& o) noexcept : v{o.v} { ++alive; }
  ~tracked() {
    --alive;
    ++destroyed;
  }
};

struct boom {
  // 不用 explicit：seek_to::emplace 按目标类型取参，initialize 传的是用户给的实参。
  boom(int) { throw std::runtime_error("boom"); }
};

// 布局：int@0, double@8, char@16，总大小按最大对齐（double=8）收尾 -> 24。
static_assert(field_size<0, int, double, char>() == 24);
static_assert(field_size<0, char>() == 1);
static_assert(field_size<0, double, char>() == 16);
static_assert(field_size<0, char, int, double, big_align>() ==
              ((32 + sizeof(big_align) + 31) / 32) * 32);  // 32 = big_align 的对齐

// Alignment 参数：0 = 按 alignof（内存布局，带 padding），2 的幂 = 统一边界。
// 1 = packed（线上/文件格式）：char 1 + int 4 + double 8 = 13，没有 padding。
static_assert(field_size<1, char, int, double>() == 13);
static_assert(field_size<2, char, int, double>() == 14);  // int/double 的对齐降到 2
static_assert(field_size<4, char, int, double>() == 16);  // double 的对齐降到 4

// 引用归一化：模板里写 T& / T const&，布局按 std::reference_wrapper 算。
static_assert(std::is_same_v<stored_type_t<int&>, std::reference_wrapper<int>>);
static_assert(std::is_same_v<stored_type_t<int const&>,
                             std::reference_wrapper<int const>>);
static_assert(std::is_same_v<stored_type_t<double>, double>);
static_assert(field_size<0, int&, double const&>() == 2 * sizeof(void*));

// 最大对齐类型的累积：一个类型本身作为 payload 的 ostream。
using largest = typename meta_states::run<
    meta_ios::stream_states_co<>, larger_alignment_calculator,
    meta_iostream::meta_istream_list<char, int, double, big_align>>::type::to_t;
static_assert(std::is_same_v<largest, big_align>);

// type_mask：重复类型也要有正确的位。
static_assert(type_mask_series<int, int, double, int, char> == 0b101);
static_assert(type_mask_series<double, int, double, int, char> == 0b010);
static_assert(type_mask_series<char, int, double, int, char> == 0b1000);
static_assert(type_mask_series<std::string, int, std::string, char> == 0b010);

// 槽位判据的 cv 方向：加 const 放行（& -> const&），去 const 拒绝。
static_assert(type_mask_series<int const, int, double, const int> == 0b101);
static_assert(type_mask_series<int, int, double, const int> == 0b001);
static_assert(is_slot_convertible<int const, int>);
static_assert(is_slot_convertible<int const&, int>);
static_assert(!is_slot_convertible<int, int const>);
static_assert(!is_slot_convertible<int&, int const>);

// 128 位位图：旧版 64 类型上限已去掉，第 100 个类型也能命中。
template <std::size_t... I>
constexpr auto wide_mask(std::index_sequence<I...>) {
  return type_mask_series<std::integral_constant<std::size_t, 100>,
                          std::integral_constant<std::size_t, I>...>;
}
static_assert(wide_mask(std::make_index_sequence<128>{}).low == 0);
static_assert(wide_mask(std::make_index_sequence<128>{}).high ==
              (std::uint64_t{1} << 36));

// 偏移列表（与 pipe 的静态断言互相印证）。
using offsets = aligned_offset_stream<0, int, double, char>;
static_assert(exp_utilities::exp_size<offsets> == 3);

bool run() {
  bool ok = true;

  // ---- 构造 / 取用 / 输出 ----
  alignas(big_align) std::byte buffer[64]{};
  using range_t = type_safe_aligned_range<0, int, double, char>;
  range_t range{buffer};
  type_safe_aligned::initialize(range, 10, 3.25, 'k');

  {
    std::ostringstream os;
    auto it = range.begin();
    os << *it;
    ++it;
    os << ' ' << *it;
    ++it;
    os << ' ' << *it;
    std::cout << "values: " << os.str() << '\n';
    ok = ok && (os.str() == "10 3.25 k");
  }

  // ---- Alignment = 1（packed）：同一套类型，没有 padding ----
  {
    std::byte packed_buf[16]{};
    type_safe_aligned_range<1, char, int, double> packed{packed_buf};
    type_safe_aligned::initialize(packed, 'x', 7, 1.5);

    auto it = packed.begin();
    ok = ok && static_cast<char&>(*it) == 'x';
    ++it;
    ok = ok && static_cast<int&>(*it) == 7;
    ++it;
    ok = ok && static_cast<double&>(*it) == 1.5;
    ++it;
    ok = ok && it == packed.end();

    // packed 布局：int 紧跟在 char 后面，从偏移 1 开始
    int raw = 0;
    std::memcpy(&raw, packed_buf + 1, sizeof(raw));
    ok = ok && raw == 7 && field_size<1, char, int, double>() == 13;
  }

  // 槽位类型校验：在 int 槽位上取 double 必须抛 type_not_safe。
  {
    auto it = range.begin();
    bool threw = false;
    try {
      (void)static_cast<double&>(*it);
    } catch (typename aligned_iterator_storage<int, double, char>::exception& e) {
      threw = e.code() ==
              aligned_iterator_storage<int, double, char>::error::type_not_safe;
    }
    ok = ok && threw;
  }

  // ---- 加 const 的读取：int 槽位可以当 const int& 用（反向不被接受）----
  {
    auto it = range.begin();
    int const& ci = static_cast<int const&>(*it);
    std::cout << "const view: " << ci << '\n';
    ok = ok && ci == 10;
  }

  // ---- 引用槽位：写 int& / double const&，值层面仍要 std::ref / std::cref ----
  {
    int a = 1;
    double d = 2.0;
    alignas(std::max_align_t) std::byte ref_buf[64]{};
    type_safe_aligned_range<0, int&, double const&> rr{ref_buf};
    type_safe_aligned::initialize(rr, std::ref(a), std::cref(d));

    auto it = rr.begin();
    static_cast<std::reference_wrapper<int>&>(*it).get() = 5;
    ++it;
    double const& back =
        static_cast<std::reference_wrapper<double const>&>(*it).get();
    std::cout << "reference slots: " << a << ' ' << back << '\n';
    ok = ok && a == 5 && back == 2.0;

    // 流运算符：代理解出来的 reference_wrapper 会隐式转回 T& 读写原对象。
    std::ostringstream os;
    auto out_it = rr.begin();
    os << *out_it;
    ++out_it;
    os << ' ' << *out_it;
    std::cout << "reference stream: " << os.str() << '\n';
    ok = ok && os.str() == "5 2";

    // 含 const 引用的类型列表不能整体 >>（const double& 槽位不可写），
    // 用单槽 range 验证写回路径：wrapper 隐式转回 int&，写的是外部对象。
    type_safe_aligned_range<0, int&> rr_write{ref_buf};
    std::istringstream is("9");
    is >> *rr_write.begin();
    std::cout << "reference read back: " << a << ' ' << d << '\n';
    ok = ok && a == 9 && d == 2.0;

    // 槽代理本身也接受引用列表：aligned_iterator_storage<int&> 与
    // aligned_iterator_storage<std::reference_wrapper<int>> 是同一套判据。
    aligned_iterator_storage<int&> proxy{ref_buf, 0};
    ok = ok && proxy.template safe<std::reference_wrapper<int>>() &&
         static_cast<std::reference_wrapper<int>&>(proxy).get() == a;

    // 均指回外部对象，析构不会消失
    for (auto rit = rr.rbegin(); rit != rr.rend(); ++rit) {
      rit.destroy();
    }
    ok = ok && a == 9;
  }

  // ---- 用受检代理写回 ----
  {
    auto it = range.begin();
    ++it;
    *it = 2.5;
    std::ostringstream os;
    os << *it;
    std::cout << "overwritten double: " << os.str() << '\n';
    ok = ok && (os.str() == "2.5");
  }

  // ---- range-for 正向遍历 ----
  {
    std::size_t count = 0;
    for (auto& slot : range) {
      (void)slot;
      ++count;
    }
    std::cout << "range-for slots: " << count << '\n';
    ok = ok && count == 3;
  }

  // ---- 反向迭代器的槽位号（在销毁之前读，保证对象仍然有效） ----
  {
    auto it = range.rbegin();
    std::ostringstream os;
    os << *it;  // 最后一个元素：char
    std::cout << "rbegin value: " << os.str() << '\n';
    ok = ok && (os.str() == "k");
  }

  // ---- 逆序销毁 ----
  {
    std::size_t destroyed = 0;
    for (auto it = range.rbegin(); it != range.rend(); ++it) {
      it.destroy();
      ++destroyed;
    }
    std::cout << "destroyed: " << destroyed << '\n';
    ok = ok && destroyed == 3;
  }

  // ---- 输入：operator>> ----
  {
    alignas(big_align) std::byte buffer2[64]{};
    type_safe_aligned_range<0, int, double, char> r2{buffer2};
    type_safe_aligned::initialize(r2, 0, 0.0, '\0');
    std::istringstream is("7 1.5 q");
    for (auto it = r2.begin(); it != r2.end(); ++it) {
      is >> *it;
    }
    bool values_ok = true;
    {
      auto it = r2.begin();
      int a = static_cast<int&>(*it);
      ++it;
      double b = static_cast<double&>(*it);
      ++it;
      char c = static_cast<char&>(*it);
      values_ok = a == 7 && b == 1.5 && c == 'q';
    }
    for (auto it = r2.rbegin(); it != r2.rend(); ++it) it.destroy();
    std::cout << "operator>> values: " << (values_ok ? "ok" : "bad") << '\n';
    ok = ok && values_ok;
  }

  // ---- 异常安全：构造中途抛异常时按逆序销毁已构造的部分 ----
  {
    alignas(64) std::byte buffer3[64]{};
    type_safe_aligned_range<0, tracked, int, boom> r3{buffer3};
    bool threw = false;
    try {
      type_safe_aligned::initialize(r3, tracked{11}, 22, 33);
    } catch (std::runtime_error const&) {
      threw = true;
    }
    ok = ok && threw && tracked::alive == 0 && tracked::destroyed > 0;
    // int 是平凡析构，无法计数；这里只验证 tracked 被清理、且没有继续构造。
    std::cout << "exception cleanup: alive=" << tracked::alive
              << " destroyed=" << tracked::destroyed << '\n';
  }

  return ok;
}

}  // namespace type_safe_tests

int main() {
  const bool ok = type_safe_tests::run();
  std::cout << (ok ? "PASS" : "FAIL") << '\n';
  return ok ? 0 : 1;
}
