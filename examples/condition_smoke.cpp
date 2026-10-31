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

// ===== flags_operator：字段读写 + 纯位运算加减 =====
static_assert(flags_operator<0x1234>::template get<u8, 8> == 0x12);
static_assert(flags_operator<0x1234>::template set<u8, 8, 0xAB>::value == 0xAB34);
static_assert(flags_operator<0x0100>::sub_at<8, 1>::value == 0x0000);
static_assert(flags_operator<0x00FF>::add_at<8, 1>::value == 0x1FF);  // 进位移到高位

int main() {}
