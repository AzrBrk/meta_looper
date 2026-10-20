// 把 chunk 预算压到 2，专门测段边界上的 for_each_forward 返回值：
// 第 2、3 轮 skip，第 4 轮 yield，最后一个观察值必须是 4，而不是段内的 1。
#define META_STATES_LOOPER_BUDGET 2

#include "meta_ios.hpp"

#include <iostream>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;

namespace boundary_tests {

template <bool IsPreRec, class Flags, class Object>
struct skip_mid_step {
  using type = Flags;
};
template <class Flags, class Object>
struct skip_mid_step<true, Flags, Object> {
  static constexpr std::size_t index =
      Flags::template get<u16, position_shift>;
  using type = std::conditional_t<(index == 2 || index == 3), skip_t, yield_t>;
};

struct skip_mid_fn {
  template <class Window, class Flags, class Object>
  using apply = typename skip_mid_step<Window::value == pre_rec_exec, Flags,
                                      Object>::type;
};

using loop_t =
    meta_states::run<meta_ios::stream_states_co<skip_mid_fn>, meta_iterator,
                     index_sequence_istream<5>>;

constexpr int last_forward() {
  return loop_t::for_each_forward([](auto, int value) { return value; }, 0, 1,
                                  2, 3, 4);
}
static_assert(last_forward() == 4);

constexpr int calls() {
  int n = 0;
  loop_t::for_each([&](auto) { ++n; });
  return n;
}
static_assert(calls() == 3);
static_assert(loop_t::type::to_t::value == 4);

}  // namespace boundary_tests

int main() {
  std::cout << "chunk boundary forward: ok\n";
  return 0;
}
