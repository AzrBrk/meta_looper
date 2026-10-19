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
    if (shift >= 128) return {};
    if (shift == 0) return value;
    if (shift >= 64) return {0, value.low << (shift - 64)};
    return {value.low << shift,
            (value.high << shift) | (value.low >> (64 - shift))};
  }
  friend constexpr u128 operator>>(u128 value, std::size_t shift) noexcept {
    if (shift >= 128) return {};
    if (shift == 0) return value;
    if (shift >= 64) return {value.high >> (shift - 64), 0};
    return {(value.low >> shift) | (value.high << (64 - shift)),
            value.high >> shift};
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