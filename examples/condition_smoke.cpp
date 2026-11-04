// 不依赖任何流类型的冒烟测试：只用 meta_states 这一层。
//   g++ -std=c++23 -O0 -I. -o smoke examples/condition_smoke.cpp
#include "meta_states.hpp"

using namespace meta_states;
using namespace meta_objects;

// 窗口函数：按窗口号特化，没写的窗口继承 ret_origin 直通（原样返回 flags）
template <u8 stage>
struct my_exec : ret_origin<stage> {};

template <>
struct my_exec<pre_gen_exec> {  // 窗口①：拿生成器
  template <this_ref flags, class gen>
  struct apply {
    static constexpr auto value = flags::value;
  };
};

template <>
struct my_exec<post_gen_exec> {  // 窗口②：拿到推进后的生成器
  template <this_ref flags, class gen>
  struct apply {
    static constexpr auto value = flags::value;
  };
};

using cond = make_condition<states_executor<my_exec>>;

// 条件就是一个合法的 meta_states_object，起始 flags 是 condition_seed
static_assert(meta_states_object_t<cond>);
static_assert(cond::value == condition_seed);

// ===== 计时器 =====
// 给槽 0 装表：压制 G_NOT_SKIP 三轮
using seeded = flags_operator<condition_seed>;
using armed = set_suppression_timer<seeded, 0, G_NOT_SKIP, 3>;
static_assert(timer_target<armed, 0> == G_NOT_SKIP);
static_assert(timer_count<armed, 0> == 3);
static_assert(!effective<armed>::template test<G_NOT_SKIP>);  // looper 判定时看不见它
static_assert(armed::template test<G_NOT_SKIP>);              // 用户 flags 里仍然原样保留

// ===== 自动分槽：user_fn::set_suppression<flags, bit, times>() =====
// 两个槽都空 → 落 0 号槽
using auto0 = flags_operator<user_fn::set_suppression<seeded, G_NOT_SKIP, 3>()>;
static_assert(timer_target<auto0, 0> == G_NOT_SKIP);
static_assert(timer_count<auto0, 0> == 3);

// 0 号在计时 → 落 1 号槽
using auto1 = flags_operator<user_fn::set_suppression<auto0, G_ADVANCE, 2>()>;
static_assert(timer_target<auto1, 1> == G_ADVANCE);
static_assert(timer_count<auto1, 1> == 2);

// 同一个目标重复装：不覆盖正在走的表（沿用 set_suppression_timer 的语义）
static_assert(user_fn::set_suppression<auto0, G_NOT_SKIP, 9>() == auto0::value);
// times == 0：不装
static_assert(user_fn::set_suppression<seeded, G_NOT_SKIP, 0>() == seeded::value);

template<std::size_t Slot>
consteval bool check_finish_timer() {
  using requested = flags_operator<u128{condition_seed | G_FINISH, 0x1234}>;
  using finish_timer = set_suppression_timer<requested, Slot, G_FINISH, 3>;
  static_assert(effective<finish_timer>::value == finish_timer::value);
  using no_finish = typename finish_timer::template opDeact<G_FINISH>;
  static_assert(!effective<no_finish>::template test<G_FINISH>);
  static_assert(timer_count<finish_timer, Slot> == 3);
  static_assert(timer_target<finish_timer, Slot> == G_FINISH);
  using ticked = tick_timers<finish_timer>;
  static_assert(timer_count<ticked, Slot> == 2);
  static_assert(effective<ticked>::template test<G_FINISH>);
  using occupied = set_suppression_timer<finish_timer, Slot, G_ADVANCE, 9>;
  static_assert(occupied::value == finish_timer::value);
  using replaced = override_suppression_timer<occupied, Slot, G_FINISH, 7>;
  static_assert(timer_count<replaced, Slot> == 7);
  static_assert(effective<replaced>::value == replaced::value);
  using auto_finish = flags_operator<
      user_fn::set_suppression<finish_timer, G_FINISH, 9>()>;
  static_assert(auto_finish::value == finish_timer::value);

  using other = set_suppression_timer<finish_timer, 1 - Slot, G_NOT_SKIP, 3>;
  static_assert(effective<other>::template test<G_FINISH>);
  static_assert(!effective<other>::template test<G_NOT_SKIP>);
  static_assert(effective<other>::value.high == requested::value.high);
  return true;
}
static_assert(check_finish_timer<0>());
static_assert(check_finish_timer<1>());
using auto_finish = flags_operator<user_fn::set_suppression<
    flags_operator<condition_seed | G_FINISH>, G_FINISH, 3>()>;
static_assert(timer_count<auto_finish, 0> == 3);
static_assert(effective<auto_finish>::template test<G_FINISH>);

static_assert([] {
  namespace bits = meta_loop::meta_states_looper_bits;
  for (auto shift : {timer0_shift, timer1_shift}) {
    for (u64 index = gates_shift; index < 64; ++index) {
      const auto timer = (u64{3} << bits::timer_count_shift) | index;
      const auto raw = u128{gates_mask | (timer << shift), 0x1234};
      const auto target = u64{1} << index;
      const auto expected = target == G_FINISH ? raw : raw & ~u128{target};
      if (bits::details::effective_value(raw) != expected) return false;
    }
  }
  return true;
}());

// ===== flags_operator：字段读写 + 纯位运算加减 =====
static_assert(flags_operator<0x1234>::template get<u8, 8> == 0x12);
static_assert(flags_operator<0x1234>::template set<u8, 8, 0xAB>::value == 0xAB34);
static_assert(flags_operator<0x0100>::sub_at<8, 1>::value == 0x0000);
static_assert(flags_operator<0x00FF>::add_at<8, 1>::value == 0x1FF);  // 进位移到高位

int main() {}
