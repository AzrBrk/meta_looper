// Cross-chunk args_at: a 2-round budget forces args_at to span several chunks.
#define META_STATES_LOOPER_BUDGET 2
#include "meta_states.hpp"

#include <type_traits>

using namespace meta_states;

// 6 args, observe index 5 -> the walk crosses 3 chunks of 2 rounds each.
static_assert(std::is_same_v<decltype(args_at<5>(1, 2, 3, 4, 5, 'x')), char&&>);

consteval bool cross_chunk_value() {
  static_assert(args_at<0>(10, 20, 30, 40, 50, 60) == 10);
  static_assert(args_at<2>(10, 20, 30, 40, 50, 60) == 30);
  static_assert(args_at<5>(10, 20, 30, 40, 50, 60) == 60);
  return true;
}
static_assert(cross_chunk_value());

int main() { return 0; }
