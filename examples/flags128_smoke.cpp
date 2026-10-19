#include "meta_objects.hpp"

using namespace meta_objects;
using namespace meta_objects::meta_op_bits;

using empty = flags_operator<0>;
using upper = empty::set<u64, 64, 0xFEDCBA9876543210ull>;
using both = upper::set<u64, 0, 0x123456789ABCDEFull>;
static_assert(flags_operator_type<both>);
static_assert(both::get<u64, 64> == 0xFEDCBA9876543210ull);
static_assert(both::get<u64, 0> == 0x123456789ABCDEFull);
static_assert(both::deact<u64, 0>::value == upper::value);
static_assert(both::opDeact<1>::get<u64, 64> == upper::get<u64, 64>);
static_assert(empty::set<u16, 60, 0xABCD>::get<u16, 60> == 0xABCD);
static_assert(empty::set<bool, 127, true>::test<(u128{1} << 127)>);
static_assert(empty::opNot::count == 128);
static_assert(flags_operator<~u128{0}>::inc<u64, 0>::value == u128{0, ~0ull});
static_assert(flags_operator<~0ull>::add_at<0>::value == u128{0, 1});
static_assert(flags_operator<u128{0, 1}>::sub_at<0>::value == u128{~0ull});
static_assert(both::set<u128, 0, u128{1, 2}>::get<u128, 0> == u128{1, 2});
static_assert(both::reset::is_empty);
static_assert(meta_invoke<deact_f<1>, both>::value.high == both::value.high);

constexpr auto top_bit = u128{1} << 127;
using top = empty::opSet<top_bit>;
static_assert(top::has<top_bit> && top::has_any<top_bit>);
static_assert(top::opToggle<top_bit>::is_empty);
static_assert(top::opToggle_if<top_bit, false>::value == top::value);
static_assert(top::select<true, 1, top_bit>::value == 1);
static_assert(top::select<false, 1, top_bit>::value == top_bit);
static_assert(top::opOr<1>::opAnd<top_bit>::value == top_bit);
static_assert(top::opXor<top_bit>::is_empty);
static_assert(top::eq<top_bit> && top::ne<1>);
static_assert(top::keep_only<1>::is_empty);
static_assert(top::merge<both>::intersect<top>::value == top_bit);
static_assert(top::merge<both>::diff<both>::is_empty);
static_assert(top::diff<u64_int_t<1>>::value == top_bit);
static_assert(empty::add_at<127>::value == top_bit);
static_assert(top::sub_at<127>::is_empty);
static_assert(top::dec<bool, 127>::is_empty);
static_assert(empty::set_if<true, u64, 64, 7>::get<u64, 64> == 7);
static_assert(empty::set_if<false, u64, 64, 7>::is_empty);
static_assert(top::set_if_else<false, u64, 64, 3, 5>::get<u64, 64> == 5);
static_assert(top::deact_if<false, bool, 127>::value == top_bit);
static_assert(top::deact_if<true, bool, 127>::is_empty);
static_assert(empty::field_or<u16, 60, 0xABCD>::field_and<u16, 60, 0xFF00>
                  ::field_xor<u16, 60, 0xFFFF>::get<u16, 60> == 0x54FF);
static_assert(top::field_any<u64, 64> && top::field_none<u64, 0>);
static_assert(top::field_eq<bool, 127, true>);
static_assert(top::update<u64, 64, [](u64 value) { return value >> 63; }>
                  ::get<u64, 64> == 1);
static_assert(meta_invoke<set_f<top_bit>, empty>::value == top_bit);
static_assert(meta_invoke<set_if_f<top_bit, true>, empty>::value == top_bit);
static_assert(meta_invoke<set_if_f<top_bit, false>, empty>::value == 0);
static_assert(meta_invoke<deact_if_f<top_bit, true>, top>::value == 0);
static_assert(meta_invoke<deact_if_f<top_bit, false>, top>::value == top_bit);
static_assert(top{} == top{});
static_assert(static_cast<u128>(top{}) == top_bit);

struct increment_upper {
  template<class Flags, class...>
  using apply = typename Flags::template inc<u64, 64>;
};
using state = meta_states_object<both::value, increment_upper>;
using next = meta_invoke<state>;
static_assert(meta_states_object_t<next>);
static_assert(next::flags::get<u64, 64> == upper::get<u64, 64> + 1);
static_assert(next::meta_set<both>::value == both::value);

int main() {}