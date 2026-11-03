#include "segmented_flags_prototype.hpp"

#include <cstdio>
#include <limits>

using segmented_flags_prototype::segment_bank;
using segmented_flags_prototype::segment_window;
using word_type = std::uint64_t;

template<class Window, std::size_t Index>
concept can_select = requires(Window& window) { window.template select<Index>(); };

static_assert(std::same_as<decltype(segment_window<4>::value), word_type>);
static_assert(can_select<segment_window<4>, 3>);
static_assert(!can_select<segment_window<4>, 4>);

constexpr bool check_switching() {
  segment_bank<4> source{{11, 22, 33, 44}};
  segment_window window(source);
  if (window.value != 11 || window.active_segment() != 0) return false;
  window.value = word_type{1} << 63;
  window.select<0>();
  if (window.value != (word_type{1} << 63)) return false;
  if (source.segments[0] != 11) return false;
  window.select<3>();
  if (source.segments[0] != (word_type{1} << 63)) return false;
  if (window.value != 44 || window.active_segment() != 3) return false;
  window.value = ~word_type{0};
  window.select<1>();
  if (source.segments[3] != ~word_type{0} || window.value != 22) return false;
  window.value ^= 7;
  window.commit();
  if (source.segments[1] != (22 ^ 7)) return false;
  window.select<0>();
  if (window.value != (word_type{1} << 63)) return false;
  window.value >>= 63;
  const auto saved = window.snapshot();
  return saved == source && saved.segments[0] == 1 &&
         saved.segments[2] == 33 && saved.segments[3] == ~word_type{0};
}

static_assert(check_switching());

constexpr bool check_single_segment() {
  segment_bank<1> source{};
  segment_window window(source);
  window.value = ~word_type{0};
  window.select<0>();
  window.value &= ~(word_type{1} << 63);
  return window.snapshot().segments[0] == (std::numeric_limits<word_type>::max() >> 1);
}

static_assert(check_single_segment());

constexpr auto saved_state = [] {
  segment_bank<16> source{};
  segment_window window(source);
  window.select<15>();
  window.value = word_type{1} << 63;
  window.select<0>();
  window.value = 42;
  return window.snapshot();
}();

template<auto State>
struct state_constant {
  static constexpr auto value = State;
};

static_assert(state_constant<saved_state>::value.segments[15] == (word_type{1} << 63));
static_assert(state_constant<saved_state>::value.segments[0] == 42);
static_assert(state_constant<saved_state>::value.segments[8] == 0);

constexpr bool check_reference(unsigned rounds) {
  segment_bank<4> source{{0x123456789ABCDEFull, 0xFEDCBA9876543210ull,
                          ~word_type{0}, word_type{1} << 63}};
  auto reference = source;
  segment_window window(source);
  word_type random = 0x987654321ABCDEFuLL;
  for (unsigned round = 0; round < rounds; ++round) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    const auto index = static_cast<std::size_t>(random & 3);
    switch (index) {
      case 0: window.select<0>(); break;
      case 1: window.select<1>(); break;
      case 2: window.select<2>(); break;
      case 3: window.select<3>(); break;
    }
    if (window.value != reference.segments[index]) return false;
    const auto shift = static_cast<unsigned>((random >> 8) & 63);
    window.value ^= random;
    window.value = (window.value << shift) | (window.value >> ((64 - shift) & 63));
    window.value &= ~(word_type{1} << ((random >> 16) & 63));
    reference.segments[index] ^= random;
    reference.segments[index] = (reference.segments[index] << shift) |
        (reference.segments[index] >> ((64 - shift) & 63));
    reference.segments[index] &= ~(word_type{1} << ((random >> 16) & 63));
    if (window.value != reference.segments[index]) return false;
  }
  return window.snapshot() == reference;
}

static_assert(check_reference(256));

int main() {
  const bool passed = check_switching() && check_single_segment() && check_reference(100000);
  std::puts(passed ? "PASS: segmented switching, snapshots, NTTP, 100000 reference steps"
                   : "FAIL: segmented switching");
  return passed ? 0 : 1;
}