#include "meta_stream64.hpp"
#include "meta_looper64.hpp"
#include "meta_iostream.hpp"

#include <type_traits>

using namespace meta_core64;
using namespace meta_loop64;
using namespace meta_looper64;
using namespace meta_iostream;
using namespace meta_stream64;

using gen = index_sequence_istream<4>;
using rec = meta_jostream<exp_utilities::exp_list<>>;
using cond = stream_cond64<>;

using L = looper<cond, rec, gen>;

using ic0 = std::integral_constant<std::size_t, 0>;
using ic1 = std::integral_constant<std::size_t, 1>;
using ic2 = std::integral_constant<std::size_t, 2>;
using ic3 = std::integral_constant<std::size_t, 3>;

// ---- 产物与轮次（数据驱动结束，没有预算） ----
consteval bool product_and_rounds() {
  static_assert(L::pos == 5);           // 4 轮产出 + 1 轮 eol 结束
  static_assert(L::calls == 4);         // 4 轮被观察
  using payload = typename L::type::type;
  static_assert(exp_utilities::exp_size<payload> == 4);
  static_assert(std::is_same_v<payload, exp_utilities::exp_list<ic0, ic1, ic2, ic3>>);
  return true;
}
static_assert(product_and_rounds());

// ---- for_each：返回最后一个被观察回调的值 ----
consteval bool last_value() {
  constexpr int last = L::for_each([](auto s) { return static_cast<int>(decltype(s)::index_v); });
  static_assert(last == 3);
  return true;
}
static_assert(last_value());

// ---- for_each：计数与逐轮回调 ----
consteval bool counts_and_fires() {
  int n = 0;
  L::for_each([&](auto) { ++n; });
  return n == 4;
}
static_assert(counts_and_fires());

// ---- for_each：stop 提前结束 ----
consteval int stop_count() {
  int n = 0;
  L::for_each([&](auto s) -> decltype(auto) {
    if constexpr (decltype(s)::index_v == 2) return meta_core64::control::stop();
    else { ++n; return; }
  });
  return n;
}
static_assert(stop_count() == 2);   // index 0,1 各回调一次
using stop_ret = decltype(L::for_each([](auto s) -> decltype(auto) {
  if constexpr (decltype(s)::index_v == 2) return meta_core64::control::stop();
  else return;
}));
static_assert(std::is_same_v<stop_ret, meta_core64::control::stop_t>);

// ---- for_each_forward：逐轮消耗实参，返回最后观察值 ----
consteval bool forward_last() {
  constexpr int v = L::for_each_forward(
      [](auto s, auto arg) { (void)s; return arg; }, 10, 20, 30, 40);
  static_assert(v == 40);              // 4 个实参正好 4 轮，最后一个是 40
  return true;
}
static_assert(forward_last());

// 实参少于轮次：消耗完后提前结束，最后观察值来自被消耗的最后轮
consteval bool forward_short() {
  constexpr int v = L::for_each_forward(
      [](auto s, auto) { return static_cast<int>(decltype(s)::index_v); }, 1, 2);
  static_assert(v == 1);               // 只消费 2 轮，最后观察下标是 1
  return true;
}
static_assert(forward_short());

// ---- 跳过（OBS 关闭的轮不回调，但仍推进） ----
struct skip_one {
  template <u8 W, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    u64 w = meta_stream64::ops::yield(U);
    if constexpr (W == 2) {                       // pre_rec 看 receiver
      if (meta_core64::lw::pos_v(L) == 1) w = meta_stream64::ops::with_obs(w, false);
    }
    return w;
  }();
};

using Lskip = looper<stream_cond64<skip_one>, rec, gen>;

consteval bool skip_in_for_each() {
  constexpr int last = Lskip::for_each([](auto s) {
    return static_cast<int>(decltype(s)::index_v);
  });
  static_assert(last == 3);            // 观察 0,2,3，跳过 1
  int n = 0;
  Lskip::for_each([&](auto) { ++n; });
  return n == 3;
}
static_assert(skip_in_for_each());

consteval bool skip_in_forward() {
  constexpr int v = Lskip::for_each_forward(
      [](auto s, auto arg) { (void)s; return arg; }, 10, 20, 30, 40);
  static_assert(v == 40);              // 第 1 轮跳过但仍消耗，最后仍是 40
  return true;
}
static_assert(skip_in_forward());

int main() { return 0; }
