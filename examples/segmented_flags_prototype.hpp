#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace segmented_flags_prototype {

template<std::size_t Count, std::unsigned_integral Word = std::uint64_t>
  requires (Count > 0 && !std::same_as<Word, bool>)
struct segment_bank {
  Word segments[Count]{};

  constexpr bool operator==(const segment_bank&) const noexcept = default;
};

template<std::size_t Count, std::unsigned_integral Word = std::uint64_t>
  requires (Count > 0 && !std::same_as<Word, bool>)
class segment_window {
  segment_bank<Count, Word>& source_;
  std::size_t active_ = 0;

public:
  Word value;

  explicit constexpr segment_window(segment_bank<Count, Word>& source) noexcept
      : source_(source), value(source.segments[0]) {}

  segment_window(const segment_window&) = delete;
  segment_window& operator=(const segment_window&) = delete;

  constexpr std::size_t active_segment() const noexcept { return active_; }

  constexpr void commit() noexcept { source_.segments[active_] = value; }

  template<std::size_t Index> requires (Index < Count)
  constexpr void select() noexcept {
    if (active_ != Index) {
      commit();
      value = source_.segments[Index];
      active_ = Index;
    }
  }

  constexpr segment_bank<Count, Word> snapshot() noexcept {
    commit();
    return source_;
  }
};

}