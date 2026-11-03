#include "../meta_stream_control.hpp"
#include "../meta_iostream.hpp"
#include "../meta_segment_bank.hpp"

#include <cassert>

using namespace meta_objects;
using namespace meta_states;
namespace windows = meta_states_looper_details::meta_states_condition_funtion_detail;

constexpr auto seed = [] {
  segment_bank<16> bank{};
  bank.write(5, 0x123456789ABCDEF0ull);
  bank.write(15, ~u64{0});
  bank.import_u128(u128{9, 42});
  return bank;
}();
static_assert(seed.export_u128() == u128{9, 42});
static_assert(seed.read(5) == 0x123456789ABCDEF0ull);
static_assert(seed.read(15) == ~u64{0});
static_assert(bank_u128_flags<seed>::value == u128{9, 42});

template<class Bank>
concept can_export = requires(Bank bank) { bank.export_u128(); };
static_assert(!can_export<segment_bank<1>>);

using control = windows::bank_user_flags_control_o<16>;
using seeded_control = control::meta_set<bank_flags_operator<seed>>;
using condition = meta_ios::stream_states_co<>;

template<u8 Stage, bool Received>
consteval bool check_control() {
  using bank_flags = seeded_control::flags::update_segment<
      0, u64{0xFF} << windows::stage_shift, u64{Stage} << windows::stage_shift>;
  using staged_bank = seeded_control::meta_set<bank_flags>;
  using staged_old = windows::user_flags_control_o::meta_set<
      flags_operator<u128{Stage} << windows::stage_shift>>;
  using receiver = meta_object_initialize<meta_iostream::meta_aligned_iterator<>>;
  using bank_result = meta_object_invoke<staged_bank, condition, receiver, std::bool_constant<Received>>;
  using old_result = meta_object_invoke<staged_old, condition, receiver, std::bool_constant<Received>>;
  static_assert(bank_result::value.export_u128() == old_result::value);
  static_assert(bank_result::value.read(5) == seed.read(5));
  static_assert(bank_result::value.read(15) == seed.read(15));
  return true;
}

static_assert(check_control<pre_gen_exec, false>());
static_assert(check_control<post_gen_exec, false>());
static_assert(check_control<pre_rec_exec, false>());
static_assert(check_control<post_rec_exec, false>());
static_assert(check_control<post_rec_exec, true>());

template<u8 Stage>
consteval bool check_window() {
  using old_result = meta_states_looper_details::enter_window<Stage, condition>;
  using bank_result = meta_states_looper_details::enter_window<Stage, condition, seeded_control>;
  static_assert(std::same_as<old_result, bank_result>);
  return true;
}
static_assert(check_window<pre_gen_exec>());
static_assert(check_window<post_gen_exec>());
static_assert(check_window<pre_rec_exec>());
static_assert(check_window<post_rec_exec>());

struct identity {
  template<class Flags, class...>
  using apply = Flags;
};
using high_condition = meta_states_object<u128{condition_seed | G_FINISH, ~u64{0}}, identity>;
using high_old = meta_states_looper_details::enter_window<pre_rec_exec, high_condition>;
using high_bank = meta_states_looper_details::enter_window<pre_rec_exec, high_condition, seeded_control>;
static_assert(std::same_as<high_old, high_bank>);
static_assert(high_bank::value.high == ~u64{0});

int main() {
  auto bank = seed;
  bank.import_u128(u128{~u64{0}, 123});
  assert((bank.export_u128() == u128{~u64{0}, 123}));
  assert(bank.read(15) == ~u64{0});
}