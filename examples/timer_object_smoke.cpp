// meta_timer_object 的 ::timer 语义（stream_states_co 的流结束判定直接依赖它）：
//
//   timer = std::integral_constant<bool, times > 0>      计数还在；
//         = std::false_type                              计数归零；
//         = meta_break_signal（继承 std::false_type）     break_f 命中。
//
// 关键性质：计数归零后**不能再 apply** —— apply 走的是 times - 1，
// 在 size_t 上会下溢成 SIZE_MAX，倒计时直接复活。所以流控制必须在
// 推进 / 接收之前读 ::timer，而不是事后检查。
#include "meta_ios.hpp"

#include <iostream>

using namespace meta_objects;
using exp_utilities::exp_list;

namespace timer_object_tests {

using meta_iostream::meta_jostream;
using counter_0 = std::integral_constant<std::size_t, 0>;

// 已初始化的计时器：计数 3，payload 是 exp_list<>，每次 apply 追加一个类型。
using timer3 = to_timer<meta_jostream<exp_list<>>, 3>;
static_assert(meta_timer_object_t<timer3>);
static_assert(exp_utilities::has_type<timer3>);
static_assert(timer3::timer::value);
static_assert(std::is_same_v<meta_produce_t<timer3>, exp_list<>>);

using timer2 = meta_invoke<timer3, int>;
static_assert(std::is_same_v<meta_produce_t<timer2>, exp_list<int>>);
static_assert(timer2::timer::value);

using timer1 = meta_invoke<timer2, char>;
static_assert(timer1::timer::value);

using timer0 = meta_invoke<timer1, double>;
static_assert(std::is_same_v<meta_produce_t<timer0>, exp_list<int, char, double>>);
static_assert(!timer0::timer::value, "计数归零后 timer 必须是 false_type");

// 归零后再 apply：times - 1 下溢，timer 重新变成 true —— 这就是流必须在 apply
// 之前判死的原因。
using revived = meta_invoke<timer0, long>;
static_assert(revived::timer::value);

// break_f 命中：timer 变成 meta_break_signal（继承 std::false_type，::value == false）。
struct break_on_two {
    template<class List>
    using apply = std::bool_constant<exp_utilities::exp_size<List> == 2>;
};
using broken = break_if<meta_timer_object<2, exp_list<int, char>, identity_fn>, break_on_two>;
static_assert(std::is_same_v<typename broken::timer,
                             meta_timer_object_details::meta_break_signal>);
static_assert(!broken::timer::value);
static_assert(!meta_timer_object_t<meta_jostream<exp_list<>>>);

// 未初始化形式（只有 initializer）：计数为 0 时 apply 原样返回自己（冻结），
// 计数还在时第一次 apply 用参数建立 payload 并把计数减一。
struct init_from_input {
    // 只接受真实输入：不加这条约束，initialize<no_arguments_> 也存在，
    // 对象会被判成"可无参初始化"，payload 变成 no_arguments_。
    template<class Input>
        requires (!meta_objects::no_arguments<Input>)
    using initialize = Input;
    template<class Current, class...>
    using apply = Current;
};
using uninitialized = to_timer<meta_object<init_from_input>, 2>;
static_assert(!exp_utilities::has_type<uninitialized>);
static_assert(uninitialized::timer::value);
using initialized = meta_invoke<uninitialized, int>;
static_assert(std::is_same_v<meta_produce_t<initialized>, int>);
static_assert(initialized::timer::value);
using exhausted_uninitialized = to_timer<meta_object<init_from_input>, 0>;
static_assert(!exhausted_uninitialized::timer::value);
static_assert(std::is_same_v<meta_invoke<exhausted_uninitialized, int>,
                             exhausted_uninitialized>);

// 运行时：计时器生成器在 stream_states_co 下推进 3 次后结束。
struct counter_init {
    template<meta_objects::no_arguments>
    using initialize = counter_0;
    template<class Index, class...>
    using apply = std::integral_constant<std::size_t, Index::value + 1>;
};

bool run() {
    using counted = meta_states::run<meta_ios::stream_states_co<>,
                                    meta_iostream::meta_iterator,
                                    to_timer<meta_object<counter_init>, 3>>;
    std::size_t calls = 0;
    counted::for_each([&](auto) { ++calls; });
    const bool passed = calls == 3;
    std::cout << (passed ? "PASS: " : "FAIL: ")
              << "timer generator advances 3 times (callbacks: " << calls << ")\n";
    return passed;
}

}  // namespace timer_object_tests

int main() {
    return timer_object_tests::run() ? 0 : 1;
}
