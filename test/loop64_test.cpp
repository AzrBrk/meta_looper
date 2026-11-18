#include "meta_stream64.hpp"
#include "meta_loop64.hpp"
#include "meta_iostream.hpp"

using namespace meta_core64;
using namespace meta_loop64;
using namespace meta_iostream;
using namespace meta_stream64;

// 默认"想做什么"：正常 yield，不动 USER
struct keep_going {
  template <u8 W, u64 L, u64 U, class Obj>
  static constexpr u64 value = ops::yield(U);
};

// 第 1 轮之后停止
struct stop_after_one {
  template <u8 W, u64 L, u64 U, class Obj>
  static constexpr u64 value =
      (lw::pos_v(L) >= 1) ? ops::stop(U) : ops::yield(U);
};

using gen3 = index_sequence_istream<3>;
using S0 = state<lw::make(gates::defaults, 0, 0, 0, 0), 0, meta_iterator, gen3,
                  stream_cond64<keep_going>>;

// ---- 第 0 轮：generator delay（GEN 掉），第 1 轮恢复 ----
consteval bool delay_round() {
  using st0 = step<S0>;
  static_assert((st0::g0 & gates::gen) == 0);           // 首轮不推进
  static_assert((st0::g0 & gates::cont) != 0);
  using st1 = step<typename st0::next>;
  static_assert((st1::g0 & gates::gen) != 0);           // 第二轮恢复
  return true;
}
static_assert(delay_round());

// ---- 真正跑循环：3 轮流结束 ----
consteval bool runs_three_rounds() {
  using R = run<S0, 10>;
  static_assert(R::calls == 3);                         // 3 次回调
  // 4 轮执行：第 0 轮延迟不推进，第 1/2 轮产出 0/1，第 3 轮推进后 eol 结束
  static_assert(lw::pos_v(R::state::L) == 4);
  static_assert((lw::gates_v(R::state::L) & gates::cont) == 0);  // 流已结束
  return true;
}
static_assert(runs_three_rounds());

// ---- 用户 stop：只观察到 1 轮 ----
consteval bool user_stop() {
  using S = state<lw::make(gates::defaults, 0, 0, 0, 0), 0, meta_iterator, gen3,
                  stream_cond64<stop_after_one>>;
  using R = run<S, 10>;
  static_assert(R::calls == 1);
  static_assert(lw::pos_v(R::state::L) == 2);           // 第 2 轮执行后停止
  return true;
}
static_assert(user_stop());

// ---- 预算耗尽：不会越过 Rounds ----
consteval bool budget_stops() {
  using R = run<S0, 2>;
  static_assert(R::calls == 2);
  static_assert(R::rounds_left == 0);
  return true;
}
static_assert(budget_stops());

int main() { return 0; }
