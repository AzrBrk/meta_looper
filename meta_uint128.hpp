#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace meta_objects::meta_op_bits {

struct u128 {
  std::uint64_t low = 0;
  std::uint64_t high = 0;

  constexpr u128() noexcept = default;
  constexpr u128(std::uint64_t low_value, std::uint64_t high_value = 0) noexcept
      : low(low_value), high(high_value) {}

  template<std::integral Integer>
  explicit constexpr operator Integer() const noexcept {
    if constexpr (std::same_as<Integer, bool>) {
      return low != 0 || high != 0;
    } else {
      return static_cast<Integer>(low);
    }
  }

  constexpr bool operator==(const u128&) const noexcept = default;

  friend constexpr u128 operator~(u128 value) noexcept {
    return {~value.low, ~value.high};
  }
  friend constexpr u128 operator|(u128 left, u128 right) noexcept {
    return {left.low | right.low, left.high | right.high};
  }
  friend constexpr u128 operator&(u128 left, u128 right) noexcept {
    return {left.low & right.low, left.high & right.high};
  }
  friend constexpr u128 operator^(u128 left, u128 right) noexcept {
    return {left.low ^ right.low, left.high ^ right.high};
  }
  friend constexpr u128 operator<<(u128 value, std::size_t shift) noexcept {
    const std::uint64_t s = shift & 63;
    const std::uint64_t over = static_cast<std::uint64_t>(shift >> 7);
    const std::uint64_t keep = ((over | (0 - over)) >> 63) - 1;
    const std::uint64_t cross = 0 - ((static_cast<std::uint64_t>(shift) >> 6) & 1);
    const std::uint64_t low = value.low << s;
    // (x >> 1) >> (63 - s) avoids the undefined x >> 64 when s == 0.
    const std::uint64_t high = (value.high << s) | ((value.low >> 1) >> (63 - s));
    return {low & ~cross & keep, ((high & ~cross) | (low & cross)) & keep};
  }
  friend constexpr u128 operator>>(u128 value, std::size_t shift) noexcept {
    const std::uint64_t s = shift & 63;
    const std::uint64_t over = static_cast<std::uint64_t>(shift >> 7);
    const std::uint64_t keep = ((over | (0 - over)) >> 63) - 1;
    const std::uint64_t cross = 0 - ((static_cast<std::uint64_t>(shift) >> 6) & 1);
    const std::uint64_t high = value.high >> s;
    const std::uint64_t low = (value.low >> s) | ((value.high << 1) << (63 - s));
    return {((low & ~cross) | (high & cross)) & keep, high & ~cross & keep};
  }
  friend constexpr u128 operator+(u128 left, u128 right) noexcept {
    const auto low_sum = left.low + right.low;
    return {low_sum, left.high + right.high + (low_sum < left.low)};
  }
  friend constexpr u128 operator-(u128 left, u128 right) noexcept {
    return {left.low - right.low,
            left.high - right.high - (left.low < right.low)};
  }
};

}