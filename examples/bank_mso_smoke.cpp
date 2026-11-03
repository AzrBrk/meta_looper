#include "../meta_objects.hpp"
#include "../meta_segment_bank.hpp"
#include "../meta_stream.hpp"

#include <cassert>
#include <stdexcept>

using namespace meta_objects;

struct identity {
  template<class Current, class...>
  using apply = Current;
};

static_assert(std::same_as<meta_states_object<7, identity>,
                           meta_states_object<u128{7}, identity>>);
static_assert(std::same_as<meta_states_object<true, identity>,
                           meta_states_object<u128{1}, identity>>);
static_assert(std::same_as<meta_states_object<-1, identity>,
                           meta_states_object<u128{~u64{0}}, identity>>);

constexpr auto seed = [] {
  segment_bank<16> state{};
  state.write(0, 42);
  state.write(15, u64{1} << 63);
  state.update(3, 0xFF, 0x1234);
  return state;
}();
static_assert(seed.read(3) == 0x34);
static_assert(seed.with_segment<2>(99).read(2) == 99);

struct write_segment {
  template<class Flags, class Input>
  struct apply {
    static constexpr auto value = [] {
      auto state = Flags::value;
      constexpr auto index = meta_produce_t<Input>::value % Flags::segment_count;
      state.write(index, state.read(index) ^ 7);
      return state;
    }();
  };
};

using state = meta_states_object<seed, write_segment>;
using input = meta_object<std::integral_constant<std::size_t, 3>, identity>;
using next = meta_object_invoke<state, input>;
static_assert(meta_states_object_t<state>);
static_assert(flags_operator_type<state::flags>);
static_assert(next::value.read(3) == (0x34 ^ 7));
static_assert(next::value.read(15) == (u64{1} << 63));
static_assert(std::same_as<meta_invoke<state, input>, next>);

using edited = state::flags::set_segment<0, 100>::update_segment<3, 0xF, 5>;
using reset_state = state::meta_set<edited>;
static_assert(reset_state::value.read(0) == 100);
static_assert(reset_state::value.read(3) == 0x35);
static_assert(edited::reset::value == segment_bank<16>{});

using receiver = meta_object<void, identity>;
struct capture {
  template<class Current, class Input>
  using apply = Input;
};
using consumer = meta_object<void, capture>;
static_assert(std::same_as<meta_produce_t<state>, state::flags>);
static_assert(std::same_as<meta_produce_t<meta_object_invoke<consumer, state>>, state::flags>);
static_assert(std::same_as<meta_produce_t<meta_transfer_object<state, receiver>>, state::flags>);
static_assert(std::same_as<meta_transfer_object<next, state>, next>);

template<class Expected>
struct observer {
  template<class Flags, class Object>
  struct apply {
    static_assert(std::same_as<Object, Expected>);
    static constexpr auto value = Flags::value;
  };
};
using legacy = meta_states_object<7, identity>;
using bank_observer = meta_states_object<seed, observer<legacy>>;
using legacy_observer = meta_states_object<7, observer<state>>;
static_assert(meta_object_invoke<bank_observer, legacy>::value == seed);
static_assert(meta_object_invoke<legacy_observer, state>::value == 7);

template<class Object, class Flags>
concept can_reset = requires { typename Object::template meta_set<Flags>; };
static_assert(!can_reset<state, bank_flags_operator<segment_bank<2>{}>>);
static_assert(!can_reset<state, flags_operator<7>>);
static_assert(!can_reset<legacy, state::flags>);

struct wrong_capacity {
  template<class...>
  using apply = bank_flags_operator<segment_bank<2>{}>;
};
template<class Object>
concept can_apply = requires { typename Object::template apply<>; };
static_assert(!can_apply<meta_states_object<seed, wrong_capacity>>);
static_assert(can_apply<meta_states_object<seed, identity>>);
static_assert(!can_apply<meta_states_object<7, wrong_capacity>>);

using accumulated = meta_ios::meta_pipe<meta_iostream::index_sequence_istream<65>>
    ::run<state>;
constexpr auto accumulated_reference = [] {
  auto bank = seed;
  for (std::size_t index = 0; index < 65; ++index) {
    const auto segment = index % bank.segment_count;
    bank.write(segment, bank.read(segment) ^ 7);
  }
  return bank;
}();
static_assert(meta_produce_t<typename accumulated::type::to>::value == accumulated_reference);

int main() {
  segment_bank<2> state{};
  state.write(1, 99);
  assert(state.read(1) == 99);
  state.update(1, 0xF, 5);
  assert(state.read(1) == 101);
  bool read_failed = false;
  bool write_failed = false;
  try { (void)state.read(2); } catch (const std::out_of_range&) { read_failed = true; }
  try { state.write(2, 1); } catch (const std::out_of_range&) { write_failed = true; }
  assert(read_failed && write_failed);
}