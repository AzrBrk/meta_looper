// Migration check for type_safe_aligned_iterator.hpp (two-word / u128-free).
// Verifies field_size, type_mask_series (now two u64), and the runtime walkers
// initialize / get<I> / destroy_range.
#include "type_safe_aligned_iterator.hpp"

#include <cassert>
#include <cstddef>
#include <string>
#include <type_traits>

using namespace type_safe_aligned;

// ---- field_size: total aligned size (no u128, no old looper) ----
static_assert(field_size<0, int, double, char>() == 24);          // 17 rounded to 8
static_assert(field_size<1, int, double, char>() == 13);          // packed, no tail pad

// ---- type_mask_series: the match bitmap as two u64 halves ----
static_assert(type_mask_series<int, int, double, int, char>.low == 0b101);
static_assert(type_mask_series<int, int, double, int, char>.high == 0);
static_assert(type_mask_series<double, int, double, int, char>.low == 0b010);
static_assert(type_mask_series<char, int, double, int, char>.low == 0b1000);
static_assert(type_mask_series<int const, int, double, const int>.low == 0b101);
static_assert(type_mask_series<int, int, double, const int>.low == 0b001);

// ---- runtime: initialize / get<I> / destroy_range ----
int main() {
  std::byte buffer[64]{};
  {
    type_safe_aligned_range<0, int, double, char> range(buffer);
    type_safe_aligned::initialize(range, 42, 3.14, 'x');
    assert(get<0>(range) == 42);
    assert(get<1>(range) == 3.14);
    assert(get<2>(range) == 'x');
    type_safe_aligned::destroy_range(range);
  }

  // References: slots normalize to std::reference_wrapper.
  {
    int a = 7;
    std::byte refbuf[32]{};
    type_safe_aligned_range<0, int&, double const&> range(refbuf);
    const double d = 2.5;
    type_safe_aligned::initialize(range, std::ref(a), std::cref(d));
    assert(get<0>(range) == 7);
    assert(get<1>(range) == 2.5);
    a = 9;
    assert(get<0>(range) == 9);   // the reference slot sees the change
    type_safe_aligned::destroy_range(range);
  }
  return 0;
}
