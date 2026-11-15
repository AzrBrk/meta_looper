// stream_states_co × meta_timer_object：让计时器的 ::timer 同步参与流结束判定。
//
// 约定（meta_stream_control.hpp）：
//   1. 生成器或接收器中最多一个可以是 meta_timer_object，两个都是会 static_assert；
//   2. 该对象的 ::timer 为 false（计数归零 → false_type，break_f 命中 → meta_break_signal）
//      时流结束；
//   3. 判定发生在窗口①③（推进 / 接收之前）：最后一次 apply 的产物和回调都保留，
//      也避免 meta_timer_object<0, ...>::apply 把 times - 1 算成 SIZE_MAX、倒计时复活；
//   4. 高 64 位最高字节归 stream_states_co 所有，位 0 = "本窗口的对象是计时器"，
//      用户 Fn 写进去会被恢复掉。
#include "meta_ios.hpp"

#include <cstddef>
#include <iostream>
#include <type_traits>

using namespace meta_states;
using namespace meta_objects;
using namespace meta_iostream;
using exp_utilities::exp_list;

namespace stream_timer_tests {

using counter_0 = std::integral_constant<std::size_t, 0>;
using counter_1 = std::integral_constant<std::size_t, 1>;
using counter_2 = std::integral_constant<std::size_t, 2>;

// 无参初始化、每轮 +1：payload 就是"已经推进了几次"，方便核对剩余次数。
struct counter_init {
    template<meta_objects::no_arguments>
    using initialize = counter_0;
    template<class Index, class...>
    using apply = std::integral_constant<std::size_t, Index::value + 1>;
};
using counter_object = meta_object<counter_init>;

// ---------------------------------------------------------------------------
// 1) 计时器当生成器：恰好 N 轮，最后一轮产物不丢
// ---------------------------------------------------------------------------
using counted_source = to_timer<counter_object, 3>;
using counted_loop = meta_states::run<meta_ios::stream_states_co<>, meta_ostream<>,
                                     counted_source>;
static_assert(initialized_meta_object_t<typename counted_loop::type::to>);
static_assert(std::is_same_v<typename counted_loop::type::to_t,
                             exp_list<counter_0, counter_1, counter_2>>);
static_assert([] {
    std::size_t calls = 0;
    counted_loop::for_each([&](auto) { ++calls; });
    return calls == 3;
}());

// 计数 0：第一轮就结束，一个回调都没有（也不能把 times - 1 算出来）。
using zero_source = to_timer<counter_object, 0>;
using zero_loop = meta_states::run<meta_ios::stream_states_co<>, meta_iterator, zero_source>;
static_assert([] {
    std::size_t calls = 0;
    zero_loop::for_each([&](auto) { ++calls; });
    return calls == 0;
}());

// break_f 命中：计时器冻结在 break_signal 上，流按 ::timer 结束，不会无限重复同一轮。
struct break_at_two {
    template<class Index>
    using apply = std::bool_constant<(Index::value >= 2)>;
};
using breaking_source = meta_timer_object<5, counter_0, counter_init, break_at_two>;
using breaking_loop = meta_states::run<meta_ios::stream_states_co<>, meta_ostream<>,
                                      breaking_source>;
// 首轮吃"已初始化生成器"的延迟（不推进），之后 3 次推进里第 3 次 break 命中。
static_assert([] {
    std::size_t calls = 0;
    breaking_loop::for_each([&](auto) { ++calls; });
    return calls == 3;
}());
static_assert(std::is_same_v<typename breaking_loop::type::to_t,
                             exp_list<counter_0, counter_1, counter_2>>);

// ---------------------------------------------------------------------------
// 2) 计时器当接收器：无限源由接收器计时器掐断
// ---------------------------------------------------------------------------
using timed_receiver = to_timer<meta_ostream<>, 3>;
using receiver_loop = meta_states::run<meta_ios::stream_states_co<>, timed_receiver,
                                      meta_index_istream<0>>;
static_assert(std::is_same_v<typename receiver_loop::type::to_t,
                             exp_list<counter_0, counter_1, counter_2>>);
static_assert([] {
    std::size_t calls = 0;
    receiver_loop::for_each([&](auto) { ++calls; });
    return calls == 3;
}());

// ---------------------------------------------------------------------------
// 3) 保留字节：窗口①标记生成器、窗口③读它并换成接收器，用户 Fn 改不动
// ---------------------------------------------------------------------------
inline constexpr u64 reserved = meta_ios_details::stream_reserved_mask;
inline constexpr u64 timer_bit = meta_ios_details::timer_stream_mask;
inline constexpr u64 pre_rec_stage = u64{pre_rec_exec} << user_shift;

using stream_condition = meta_ios::stream_states_co<>;
using generator_window = meta_object_invoke<stream_condition, counted_source>;
static_assert((generator_window::flags::value.high & reserved) == timer_bit);
using plain_generator_window = meta_object_invoke<stream_condition, counter_object>;
static_assert((plain_generator_window::flags::value.high & reserved) == 0);

// 窗口③：接收器不是计时器 → 标记归零，流继续。
using at_pre_rec = typename stream_condition::flags::template set<u8, user_shift,
                                                                  pre_rec_exec>;
using pre_rec_condition = typename stream_condition::template meta_set<at_pre_rec>;
using receiver_window = meta_object_invoke<pre_rec_condition, meta_iterator>;
static_assert((receiver_window::flags::value.high & reserved) == 0);
static_assert(receiver_window::flags::template test<G_CONTINUE>);

// 窗口③：接收器是计时器且生成器不是 → 标记置位。
using timer_receiver_window = meta_object_invoke<pre_rec_condition, timed_receiver>;
static_assert((timer_receiver_window::flags::value.high & reserved) == timer_bit);
static_assert(timer_receiver_window::flags::template test<G_CONTINUE>);

// 计时器用完：窗口①/③直接清 G_CONTINUE（不再推进 / 接收）。
using exhausted_timer = meta_timer_object<0, exp_list<>, identity_fn, meta_always_continue>;
static_assert(!exhausted_timer::timer::value);
using exhausted_generator = meta_object_invoke<stream_condition, exhausted_timer>;
static_assert(!exhausted_generator::flags::template test<G_CONTINUE>);
using exhausted_receiver = meta_object_invoke<pre_rec_condition, exhausted_timer>;
static_assert(!exhausted_receiver::flags::template test<G_CONTINUE>);

// 用户 Fn 往保留字节里写东西：被恢复成流控制自己的值。
struct poke_reserved {
    template<class Window, class Flags, class Object>
    using apply = flags_operator<u128{Flags::value.low,
                                      Flags::value.high | reserved}>;
};
using poked_timer = meta_object_invoke<meta_ios::stream_states_co<poke_reserved>,
                                       counted_source>;
static_assert((poked_timer::flags::value.high & reserved) == timer_bit);
using poked_plain = meta_object_invoke<meta_ios::stream_states_co<poke_reserved>,
                                       counter_object>;
static_assert((poked_plain::flags::value.high & reserved) == 0);

// ---------------------------------------------------------------------------
// 运行时复核
// ---------------------------------------------------------------------------
template<class Loop, class... Expected>
bool check(const char* name, std::size_t expected_calls, exp_list<Expected...>) {
    std::size_t calls = 0;
    bool types_ok = true;
    Loop::for_each([&](auto view) {
        using stream = decltype(view);
        (void)view;
        ++calls;
        types_ok = types_ok && (stream::states::value.high & reserved) != 0xFFFFull;
    });
    using actual = typename Loop::type::to_t;
    const bool passed = types_ok && calls == expected_calls &&
                        std::is_same_v<actual, exp_list<Expected...>>;
    std::cout << (passed ? "PASS: " : "FAIL: ") << name
              << " (callbacks: " << calls << ")\n";
    return passed;
}

bool run() {
    unsigned failed = 0;
    std::cout << "\nstream_states_co timer tests:\n";
    failed += !check<counted_loop>("timer generator stops at its count", 3,
                                   exp_list<counter_0, counter_1, counter_2>{});
    failed += !check<receiver_loop>("timer receiver caps an infinite source", 3,
                                    exp_list<counter_0, counter_1, counter_2>{});
    failed += !check<breaking_loop>("timer generator stops when break_f hits", 3,
                                    exp_list<counter_0, counter_1, counter_2>{});
    std::cout << "Failed cases: " << failed << '\n';
    return failed == 0;
}

}  // namespace stream_timer_tests

int main() {
    return stream_timer_tests::run() ? 0 : 1;
}
