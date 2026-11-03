#include "../meta_stream_control.hpp"
#include "../meta_iostream.hpp"

#include <cstdio>

namespace communication_tests {
using namespace meta_objects;
using namespace meta_states;
using namespace meta_iostream;
namespace windows = meta_states_looper_details::meta_states_condition_funtion_detail;

constexpr u64 initial_segment = 0xFEDCBA9876543210ull;
constexpr u64 untouched_segment = 0x123456789ABCDEF0ull;

struct write_offset {
  template<class Flags, class Object>
  struct apply {
    using produced = meta_produce_t<Object>;
    static_assert(std::same_as<std::remove_cv_t<decltype(produced::value)>, std::size_t>);
    static_assert(produced::value <= 0xFF);
    static constexpr u8 offset = static_cast<u8>(produced::value);
    static constexpr auto index = user_fn::index_of<Flags>();
    static_assert(index < sizeof(u64));
    static constexpr auto value = [] {
      auto flags = Flags::value;
      constexpr auto shift = index * 8;
      flags.high = (flags.high & ~(u64{0xFF} << shift)) | (u64{offset} << shift);
      return flags;
    }();
  };
};

struct condition {
  template<class Window, class Flags, class Object>
  struct apply {
    static constexpr auto value = [] {
      if constexpr (Window::value == post_rec_exec) {
        constexpr auto state = [] {
          segment_bank<4> bank{};
          bank.import_u128(Flags::value);
          bank.write(3, untouched_segment);
          return bank;
        }();
        using banker = typename windows::bank_user_flags_control_o<4>::template meta_set<bank_flags_operator<state>>;
        using mapped_flags = bank_u128_flags<state>;
        using writer = meta_states_object<mapped_flags::value, write_offset>;
        using written = meta_object_invoke<writer, Object>;
        using staged_flags = typename banker::flags::template update_segment<
            0, u64{0xFF} << windows::stage_shift, u64{post_rec_exec} << windows::stage_shift>;
        using staged_banker = typename banker::template meta_set<staged_flags>;
        using committed = meta_object_invoke<staged_banker, written>;
        static_assert(committed::value.read(1) == written::flags::value.high);
        static_assert(committed::value.read(2) == 0);
        static_assert(committed::value.read(3) == untouched_segment);
        using projection = meta_object<bank_u128_flags<committed::value>, identity_fn>;
        using returned = meta_transfer_object<projection, written>;
        static_assert(returned::flags::value.high == written::flags::value.high);
        return u128{Flags::value.low, returned::flags::value.high};
      } else {
        return Flags::value;
      }
    }();
  };
};

using stream_condition = meta_ios::stream_states_co<condition>;
using seeded_condition = stream_condition::meta_set<flags_operator<
    u128{stream_condition::value.low, initial_segment}>>;
using execution = meta_states::run<seeded_condition, meta_aligned_iterator<>,
    meta_istream<exp_list<int, double, char>>>;

using first = meta_invoke<meta_aligned_iterator<>, int>;
using second = meta_invoke<first, double>;
using third = meta_invoke<second, char>;
constexpr u64 expected = (initial_segment & ~u64{0xFFFFFF}) |
    u64{static_cast<u8>(meta_produce_t<first>::value)} |
    (u64{static_cast<u8>(meta_produce_t<second>::value)} << 8) |
    (u64{static_cast<u8>(meta_produce_t<third>::value)} << 16);
static_assert(execution::type::states::value.high == expected);

constexpr bool callbacks() {
  unsigned calls = 0;
  bool passed = true;
  execution::for_each([&](auto view) {
    using stream = decltype(view);
    constexpr auto offset = meta_produce_t<typename stream::to>::value;
    constexpr auto stored = (stream::states::value.high >> (view.index() * 8)) & 0xFF;
    static_assert(stored == static_cast<u8>(offset));
    static_assert((stream::states::value.high & ~u64{0xFFFFFF}) ==
                  (initial_segment & ~u64{0xFFFFFF}));
    passed = passed && view.index() == calls;
    ++calls;
  });
  return passed && calls == 3;
}
static_assert(callbacks());
}

int main() {
  const bool passed = communication_tests::callbacks();
  std::printf("%s: fixed segment 1 round-trip, packed offsets = 0x%016llX\n",
      passed ? "PASS" : "FAIL",
      static_cast<unsigned long long>(communication_tests::execution::type::states::value.high));
  return passed ? 0 : 1;
}