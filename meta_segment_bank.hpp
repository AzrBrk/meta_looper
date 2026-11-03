#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

#include "meta_uint128.hpp"

namespace meta_objects::meta_op_bits {

template<std::size_t Count, class Word = std::uint64_t>
  requires (Count > 0 && std::same_as<Word, std::uint64_t>)
struct segment_bank {
  using word_type = Word;
  static constexpr std::size_t segment_count = Count;
  Word segments[Count]{};

  constexpr bool operator==(const segment_bank&) const noexcept = default;

  constexpr Word read(std::size_t index) const {
    if (index >= Count) throw std::out_of_range("segment index out of range");
    return segments[index];
  }

  constexpr void write(std::size_t index, Word value) {
    if (index >= Count) throw std::out_of_range("segment index out of range");
    segments[index] = value;
  }

  constexpr void update(std::size_t index, Word mask, Word bits) {
    write(index, (read(index) & ~mask) | (bits & mask));
  }

  constexpr void import_u128(u128 value) noexcept requires (Count >= 2) {
    segments[0] = value.low;
    segments[1] = value.high;
  }

  constexpr u128 export_u128() const noexcept requires (Count >= 2) {
    return {segments[0], segments[1]};
  }

  template<std::size_t Index> requires (Index < Count)
  consteval segment_bank with_segment(Word value) const noexcept {
    auto result = *this;
    result.segments[Index] = value;
    return result;
  }
};

template<class T>
struct is_segment_bank : std::false_type {};

template<std::size_t Count, class Word>
struct is_segment_bank<segment_bank<Count, Word>> : std::true_type {};

template<class T>
inline constexpr bool is_segment_bank_v = is_segment_bank<std::remove_cv_t<T>>::value;

template<auto State> requires is_segment_bank_v<decltype(State)>
struct bank_flags_operator : std::integral_constant<decltype(State), State> {
  using bank_type = std::remove_cv_t<decltype(State)>;
  using word_type = typename bank_type::word_type;
  static constexpr std::size_t segment_count = bank_type::segment_count;

  template<std::size_t Index> requires (Index < segment_count)
  static constexpr word_type segment = State.segments[Index];

  template<std::size_t Index, word_type Value> requires (Index < segment_count)
  using set_segment = bank_flags_operator<State.template with_segment<Index>(Value)>;

  template<std::size_t Index, word_type Mask, word_type Bits>
    requires (Index < segment_count)
  using update_segment = set_segment<Index, (segment<Index> & ~Mask) | (Bits & Mask)>;

  using reset = bank_flags_operator<bank_type{}>;
};

}