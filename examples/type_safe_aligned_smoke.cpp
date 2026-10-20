#include "type_safe_aligned_iterator.hpp"

#include <cassert>
#include <stdexcept>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>

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
static_assert(field_size<int, double, char>() == 24);
static_assert(field_size<char>() == 1);
static_assert(field_size<double, char>() == 16);
static_assert(field_size<char, int, double, big_align>() == 96);

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

// 偏移列表（与 pipe 的静态断言互相印证）。
using offsets = aligned_offset_stream<int, double, char>;
static_assert(exp_utilities::exp_size<offsets> == 3);

bool run() {
  bool ok = true;

  // ---- 构造 / 取用 / 输出 ----
  alignas(big_align) std::byte buffer[64]{};
  using range_t = type_safe_aligned_range<int, double, char>;
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
    type_safe_aligned_range<int, double, char> r2{buffer2};
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
    type_safe_aligned_range<tracked, int, boom> r3{buffer3};
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
