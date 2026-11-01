#include "meta_ios.hpp"

#include <array>

namespace execution_contract_tests {
using namespace meta_states;
using namespace meta_iostream;
namespace engine = meta_states_looper_details;

constexpr unsigned ordinal(u8 window) {
  switch (window) {
    case pre_gen_exec: return 1;
    case post_gen_exec: return 2;
    case pre_rec_exec: return 3;
    case post_rec_exec: return 4;
  }
  return 0;
}

struct trace {
  template<class Window, class Flags, class Object>
  struct apply {
    static constexpr auto value = u128{
        Flags::value.low &
            ~(Flags::template get<u16, position_shift> >= 2 ? G_CONTINUE : u64{0}),
        (Flags::value.high << 4) | ordinal(Window::value)};
  };
};

using loop = run<make_condition<trace>, meta_iterator, meta_index_istream<0>>;
// A closed CONTINUE does not by itself skip remaining condition windows.
static_assert(loop::type::states::value.high == 0x123412341234);
static_assert(loop::type::states::template get<u16, position_shift> == 3);
static_assert([] {
  unsigned calls = 0;
  loop::for_each([&](auto) { ++calls; });
  return calls == 2;
}());

template<u8 FinishAt>
struct finish_trace {
  template<class Window, class Flags, class Object>
  struct apply {
    static constexpr auto value = u128{
        Flags::value.low | (Window::value == FinishAt ? G_FINISH : u64{0}),
        (Flags::value.high << 4) | ordinal(Window::value)};
  };
};

// A readable sequential reference model, independent of the kernel's masks.
consteval auto reference(u8 finish_at) {
  struct result {
    u64 windows = 0;
    bool advanced = false;
    bool received = false;
  } out;
  for (u8 window : {pre_gen_exec, post_gen_exec, pre_rec_exec, post_rec_exec}) {
    out.windows = out.windows * 16 + ordinal(window);
    if (window == pre_gen_exec && finish_at != pre_gen_exec) out.advanced = true;
    // pre_rec FINISH skips post_rec, but does not undo the receive operation.
    if (window == pre_rec_exec) out.received = true;
    if (window == finish_at) break;
  }
  return out;
}

template<u8 FinishAt>
consteval bool check_finish() {
  using step = engine::meta_states_looper_step<
      make_condition<finish_trace<FinishAt>>, meta_iterator, meta_index_istream<0>>;
  constexpr auto expected = reference(FinishAt);
  static_assert(step::condition::flags::value.high == expected.windows);
  static_assert(step::_advance_ == expected.advanced);
  static_assert(step::_receive_ == expected.received);
  static_assert(step::observe_);  // FINISH alone is not a callback suppression.
  static_assert(!step::condition::flags::template test<G_FINISH>);
  static_assert(step::condition::flags::template get<u16, position_shift> == 1);
  return true;
}
static_assert(check_finish<pre_gen_exec>());
static_assert(check_finish<post_gen_exec>());
static_assert(check_finish<pre_rec_exec>());
static_assert(check_finish<post_rec_exec>());
}  // namespace execution_contract_tests

int main() {}
