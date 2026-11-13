#define META_STATES_LOOPER_BUDGET 2
#include "meta_ios.hpp"
#include "args_at.hpp"

#include <cassert>

using namespace meta_objects;
using namespace meta_states;
using namespace meta_iostream;
using namespace exp_utilities;

struct initializer {
    template<no_arguments>
    using initialize = int;
    template<class Current, class...>
    using apply = double;
};
struct pointer_result {
    template<class T>
    using apply = T*;
};
using changed = meta_object<double, initializer>;
static_assert(std::same_as<meta_object_initialize<changed>, meta_object<int, initializer>>);
using projected = meta_ret_object<double, initializer, pointer_result>;
static_assert(std::same_as<meta_produce_t<meta_object_initialize<projected>>, int*>);
using timed = meta_timer_object<3, double, initializer, meta_always_continue>;
static_assert(std::same_as<meta_object_initialize<timed>,
                          meta_timer_object<3, int, initializer, meta_always_continue>>);
static_assert(std::same_as<meta_object_initialize<meta_iterator>, meta_iterator>);

using accumulated = meta_invoke<meta_ostream<>, int>;
using pending = meta_object_initialize<accumulated>;
static_assert(uninitialized_meta_object_t<pending>);
static_assert(std::same_as<meta_produce_t<pending>, meta_uninitialized>);
static_assert(std::same_as<meta_invoke<pending, double>, meta_invoke<meta_ostream<>, double>>);
static_assert(std::same_as<meta_invoke<pending, double>::type, exp_list<double>>);
static_assert(std::same_as<meta_invoke<meta_ostream<exp_list<int>>, char>::type,
                          exp_list<int, char>>);

template<u8 At>
struct reset_at {
    template<class Window, class Flags, class Object>
    struct apply {
        static constexpr auto value = [] {
            if constexpr (Window::value == At)
                return (Flags::value | u128{G_INITIALIZE}) & ~u128{G_CALL_RECEIVER | G_CALL_GENERATOR};
            else return Flags::value;
        }();
    };
};
using reset_receiver = meta_states_looper_details::meta_states_looper_step<
    make_condition<reset_at<pre_rec_exec>>, accumulated, meta_repeat_istream<char>>;
static_assert(uninitialized_meta_object_t<typename reset_receiver::receiver>);
static_assert(reset_receiver::condition::flags::template test<initialization_pending_mask>);
static_assert(!reset_receiver::condition::flags::template test<G_INITIALIZE | receiver_init_failed_mask>);
static_assert(reset_receiver::continue_);

using reset_source = meta_states_looper_details::meta_states_looper_step<
    make_condition<reset_at<pre_gen_exec>>, meta_iterator, changed>;
static_assert(std::same_as<reset_source::generator, meta_object<int, initializer>>);

using pipe_reset = meta_stream_details::pipe_node_step<
    make_condition<reset_at<pre_rec_exec>>, accumulated, meta_repeat_istream<char>,
    false, false, false>;
static_assert(uninitialized_meta_object_t<typename pipe_reset::rec>);
static_assert(pipe_reset::con::flags::template test<initialization_pending_mask>);
static_assert(!pipe_reset::just_died && !pipe_reset::emitted);
using idle_reset_condition = make_condition<reset_at<pre_rec_exec>,
    u128{condition_seed | G_INITIALIZE}>;
using idle_reset = meta_stream_details::pipe_node_step<
    idle_reset_condition, accumulated, meta_aligned_iterator<>, false, false, false>;
static_assert(uninitialized_meta_object_t<typename idle_reset::rec>);
static_assert(idle_reset::con::flags::template test<initialization_pending_mask>);
static_assert(!idle_reset::con::flags::template test<G_INITIALIZE>);

struct observe_odd {
    template<class Window, class Flags, class Object>
    struct apply {
        static constexpr auto value = [] {
            if constexpr (Window::value == pre_rec_exec) {
                if constexpr (user_fn::index_of<Flags>() % 2 == 0)
                    return Flags::value & ~u128{G_NOT_SKIP | G_CONSUME};
                else return Flags::value | u128{G_NOT_SKIP | G_CONSUME};
            } else return Flags::value;
        }();
    };
};
using selected = run<meta_ios::stream_states_co<observe_odd>, meta_iterator,
                     index_sequence_istream<7>>;
static_assert([] {
    int total = 0;
    int a = 10, b = 20, c = 30;
    auto& result = selected::for_each_forward([&](auto view, int& value) -> int& {
        total += value;
        assert(view.index() == 1 || view.index() == 3 || view.index() == 5);
        return value;
    }, a, b, c);
    return &result == &c && total == 60;
}());

struct retain_argument {
    template<class Window, class Flags, class Object>
    using apply = typename Flags::template opDeact<G_CONSUME>;
};
using retaining = run<meta_ios::stream_states_co<retain_argument>, meta_iterator,
                      index_sequence_istream<5>>;
static_assert([] {
    int count = 0;
    retaining::for_each_forward([&](auto, int value) { count += value; }, 2);
    return count == 10;
}());

struct suppress_consumption {
    template<class Window, class Flags, class Object>
    using apply = set_suppression_timer<Flags, 0, G_CONSUME, 1>;
};
using suppressed = run<meta_ios::stream_states_co<suppress_consumption>,
                       meta_iterator, index_sequence_istream<3>>;
static_assert([] {
    int total = 0;
    suppressed::for_each_forward([&](auto, int value) { total += value; }, 4);
    return total == 12;
}());

struct retain_main {
    template<class Flags, class Output>
    using apply = flags_operator<
        meta_invoke<meta_ios::pipe_main_fn, Flags, Output>::value & ~u128{G_CONSUME}>;
};
using pipe_retaining = meta_ios::meta_pipe<index_sequence_istream<60>>
    ::all_to<meta_iterator>::run<void, void,
        meta_ios::main_con<meta_states_object<condition_seed, retain_main>>>;

int main() {
    assert(args_at<2>(1, 2, 3, 4) == 3);
    int count = 0;
    pipe_retaining::for_each_forward([&](auto, int value) { count += value; }, 2);
    assert(count == 120);
}
