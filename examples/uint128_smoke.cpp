#include "meta_uint128.hpp"

using meta_objects::meta_op_bits::u128;

constexpr u128 all = ~u128{0};
static_assert((u128{1} << 64) == u128{0, 1});
static_assert((u128{1} << 127) == u128{0, 1ull << 63});
static_assert((all >> 64) == u128{~0ull});
static_assert((u128{1} << 128) == u128{0});
static_assert((all >> 128) == u128{0});
static_assert((all << 0) == all);
static_assert((all >> 0) == all);
static_assert((u128{~0ull} + 1) == u128{0, 1});
static_assert((u128{0, 1} - 1) == u128{~0ull});
static_assert(all + 1 == u128{0});
static_assert(u128{0} - 1 == all);
static_assert((u128{0x1234, 0x5678} & u128{0, ~0ull}) == u128{0, 0x5678});
static_assert(static_cast<bool>(u128{0, 1}));

template<u128 Value>
struct constant {
  static constexpr auto value = Value;
};
static_assert(constant<7>::value == u128{7});
static_assert(constant<u128{0, 7}>::value.high == 7);

constexpr bool check_shifts() {
  constexpr u128 pattern{0xFEDCBA9876543210ull, 0x123456789ABCDEFull};
  for (std::size_t shift = 0; shift <= 129; ++shift) {
    for (std::size_t bit = 0; bit < 128; ++bit) {
      const auto left_bit = static_cast<bool>(((pattern << shift) >> bit) & 1);
      const auto right_bit = static_cast<bool>(((pattern >> shift) >> bit) & 1);
      const auto expected_left = bit >= shift &&
          static_cast<bool>((pattern >> (bit - shift)) & 1);
      const auto expected_right = bit + shift < 128 &&
          static_cast<bool>((pattern >> (bit + shift)) & 1);
      if (left_bit != expected_left || right_bit != expected_right) return false;
    }
  }
  return true;
}
static_assert(check_shifts());

int main() {}