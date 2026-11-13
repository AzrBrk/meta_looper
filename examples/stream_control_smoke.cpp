#include "meta_ios.hpp"

#include <iostream>
#include <typeindex>
#include <vector>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_states_looper_details;

using stream_states_co = meta_ios::stream_states_co<>;

namespace stream_control_tests {
    template<u8 Mode, u8 Window>
    inline constexpr u128 mode_seed =
        (u128{condition_seed} & ~(u128{0xFF} << user_shift)) |
        (u128{Mode | Window} << user_shift) | u128{0, 0xFEDCBA9876543210ull};

    template<u8 Mode, u8 Window, class Object,
             class Fn = meta_ios_details::unchanged>
    using observe_mode = meta_object_invoke<
        typename meta_ios::stream_states_co<Fn>::template meta_set<
            flags_operator<mode_seed<Mode, Window>>>, Object>;

    struct overwrite_modes {
        template<class Window, class Flags, class Object>
        struct apply {
            static constexpr u128 value =
                (Flags::value ^ (u128{0xFF} << user_shift)) | u128{G_FINISH};
        };
    };

    struct overwrite_flags {
        template<class Window, class Flags, class Object>
        struct apply {
            static constexpr u128 value{~Flags::value.low, 0x8123456789ABCDEFull};
        };
    };

    using overwritten = observe_mode<0b1011, pre_gen_exec, meta_empty_o, overwrite_flags>;
    constexpr u128 before_overwrite{
        (mode_seed<0b1011, pre_gen_exec>.low & ~G_CALL_GENERATOR) |
            (u64{meta_ios_details::no_generator_state} << user_shift),
        mode_seed<0b1011, pre_gen_exec>.high};
    constexpr u64 protected_bits = position_mask |
        (u64{0b1111'0111} << user_shift);
    static_assert(overwritten::flags::value == u128{
        (~before_overwrite.low & ~protected_bits) |
            (before_overwrite.low & protected_bits),
        0x8123456789ABCDEFull});

    template<std::size_t Mode>
    consteval bool check_mode_bits() {
        constexpr u8 no_gen = meta_ios_details::no_generator_state;
        constexpr u128 delay = u128{meta_ios_details::generator_delay_state} << user_shift;
        constexpr u128 failed = receiver_init_failed_mask;
        constexpr bool no_generator = (Mode & no_gen) != 0;
        using source = meta_invoke<meta_istream_list<int>>;
        using exhausted = meta_invoke<meta_istream_list<>>;

        using empty_gen = observe_mode<Mode, pre_gen_exec, meta_empty_o>;
        static_assert(empty_gen::flags::value ==
            ((mode_seed<Mode, pre_gen_exec> & ~u128{G_CALL_GENERATOR}) |
             (u128{no_gen} << user_shift)));
        using initialized_gen = observe_mode<Mode, pre_gen_exec, source>;
        static_assert(initialized_gen::flags::value ==
            ((Mode & meta_ios_details::generator_delay_state) != 0
                ? (mode_seed<Mode, pre_gen_exec> & ~delay) | u128{G_CALL_GENERATOR}
                : (mode_seed<Mode, pre_gen_exec> & ~u128{G_CALL_GENERATOR}) | delay));
        using guarded_gen = observe_mode<Mode, pre_gen_exec, source, overwrite_modes>;
        static_assert(guarded_gen::flags::value ==
                      ((initialized_gen::flags::value ^ initialization_pending_mask) | u128{G_FINISH}));

        using post_gen = observe_mode<Mode, post_gen_exec, source>;
        static_assert(post_gen::flags::value == mode_seed<Mode, post_gen_exec>);
        using ended_gen = observe_mode<Mode, post_gen_exec, exhausted>;
        static_assert(ended_gen::flags::value ==
            (mode_seed<Mode, post_gen_exec> & ~u128{G_CONTINUE}));

        using needs_input = observe_mode<Mode, pre_rec_exec, meta_aligned_iterator<>>;
        static_assert(needs_input::flags::value ==
            ((mode_seed<Mode, pre_rec_exec> &
              ~(no_generator ? u128{G_CONTINUE} : u128{0})) |
             (no_generator ? failed : u128{0})));
        using initialized_rec = observe_mode<Mode, pre_rec_exec, source>;
        static_assert(timer_count<typename initialized_rec::flags, 0> ==
                      (no_generator ? 1 : 0));
        static_assert((initialized_rec::flags::value &
            ~(u128{0xFFFF} << timer0_shift)) == mode_seed<Mode, pre_rec_exec>);

        using ended_rec = observe_mode<Mode, post_rec_exec, exhausted>;
        static_assert(ended_rec::flags::value ==
            (mode_seed<Mode, post_rec_exec> &
             ~(no_generator ? u128{G_CONTINUE} : u128{0})));
        using pending_rec = observe_mode<Mode, post_rec_exec, meta_aligned_iterator<>>;
        static_assert(pending_rec::flags::value == mode_seed<Mode, post_rec_exec>);
        using guarded_rec = observe_mode<Mode, post_rec_exec, meta_aligned_iterator<>,
                                         overwrite_modes>;
        static_assert(guarded_rec::flags::value ==
                      ((pending_rec::flags::value ^ initialization_pending_mask) | u128{G_FINISH}));
        using guarded = observe_mode<Mode, pre_gen_exec, meta_empty_o, overwrite_modes>;
        static_assert(guarded::flags::value ==
            ((empty_gen::flags::value ^ initialization_pending_mask) | u128{G_FINISH}));
        return true;
    }

    static_assert([]<std::size_t... Mode>(std::index_sequence<Mode...>) {
        return (check_mode_bits<Mode>() && ...);
    }(std::make_index_sequence<16>{}));

    using mixed_condition = typename meta_ios::stream_states_co<>::template meta_set<
        flags_operator<mode_seed<0b1001, pre_gen_exec>>>;
    using mixed_source = meta_istream_list<char, double, int>;
    using mixed_loop = meta_states::run<mixed_condition, meta_invoke<mixed_source>>;
    static_assert([] {
        unsigned calls = 0;
        mixed_loop::for_each([&](auto view) {
            using stream = decltype(view);
            static_assert(std::is_same_v<meta_produce_t<typename stream::to>,
                exp_select<view.index(), exp_list<char, double, int>>>);
            static_assert((stream::states::value.low >> user_shift &
                           meta_ios_details::stream_state_mask) ==
                          (view.index() == 0 ? 0b1011 : 0b0011));
            ++calls;
        });
        return calls == 3;
    }());
    static_assert(!mixed_loop::type::states::template test<G_CONTINUE>);
    using mixed_needs_input = meta_states::run<mixed_condition, meta_aligned_iterator<>>;
    static_assert([] {
        unsigned calls = 0;
        mixed_needs_input::for_each([&](auto) { ++calls; });
        return calls == 0;
    }());
    constexpr u128 init_failed_bit = receiver_init_failed_mask;
    constexpr u128 pending_bit = initialization_pending_mask;
    static_assert(mixed_needs_input::type::states::template test<init_failed_bit>);
    static_assert(!mixed_needs_input::type::states::template test<G_CONTINUE>);
    static_assert(mixed_needs_input::type::states::template test<pending_bit>);
    static_assert(!mixed_loop::type::states::template test<init_failed_bit>);
    static_assert(!mixed_loop::type::states::template test<pending_bit>);

    struct hold_generator {
        template<class Window, class Flags, class Object>
        struct apply {
            static_assert(Window::value != post_rec_exec);
            static constexpr auto value = Flags::value & ~u128{G_CALL_GENERATOR};
        };
    };
    using failed_receiver = meta_states::run<meta_ios::stream_states_co<hold_generator>,
                                            meta_iterator, meta_aligned_iterator<>>;
    static_assert(uninitialized_meta_object_t<typename failed_receiver::type::to>);
    static_assert(std::is_same_v<typename failed_receiver::type::to,
                                meta_object_initialize<meta_aligned_iterator<>>>);
    static_assert(failed_receiver::type::states::template test<init_failed_bit>);
    static_assert(!failed_receiver::type::states::template test<G_CONTINUE | pending_bit>);
    static_assert(failed_receiver::type::states::template get<u16, position_shift> == 1);
    static_assert([] {
        unsigned calls = 0;
        failed_receiver::for_each([&](auto) { ++calls; });
        return calls == 0;
    }());
    using generic_failed_receiver = meta_states::run<make_condition<hold_generator>,
                                                    meta_iterator, meta_aligned_iterator<>>;
    static_assert(std::is_same_v<typename generic_failed_receiver::type,
                                typename failed_receiver::type>);
    static_assert([] {
        unsigned calls = 0;
        generic_failed_receiver::for_each([&](auto) { ++calls; });
        return calls == 0;
    }());

    using control_flags = flags_operator<
        u128{post_rec_exec} << meta_states_condition_funtion_detail::stage_shift>;
    using receiver_control = typename user_flags_control_o::template meta_set<control_flags>;
    using received_condition = stream_states_co::meta_set<
        flags_operator<u128{condition_seed} & ~initialization_pending_mask>>;
    using controlled_failure = meta_object_invoke<receiver_control, received_condition,
        meta_object_initialize<meta_aligned_iterator<>>>;
    static_assert(controlled_failure::flags::template test<init_failed_bit>);
    static_assert(!controlled_failure::flags::template test<G_CONTINUE>);
    static_assert(controlled_failure::flags::template get<u16, position_shift> == 1);
    using controlled_idle = meta_object_invoke<receiver_control, stream_states_co,
        meta_aligned_iterator<>>;
    static_assert(!controlled_idle::flags::template test<init_failed_bit>);
    static_assert(controlled_idle::flags::template test<pending_bit>);
    static_assert(controlled_idle::flags::template test<G_CONTINUE>);
    using controlled_success = meta_object_invoke<receiver_control, received_condition,
        meta_iterator>;
    static_assert(!controlled_success::flags::template test<init_failed_bit | pending_bit>);
    static_assert(controlled_success::flags::template test<G_CONTINUE>);

    struct finish_receive {
        template<class Window, class Flags, class Object>
        struct apply {
            static_assert(Window::value != post_rec_exec);
            static constexpr auto value = (Flags::value & ~u128{G_CALL_GENERATOR}) |
                (Window::value == pre_rec_exec ? u128{G_FINISH} : u128{0});
        };
    };
    using finish_seed = set_suppression_timer<flags_operator<condition_seed>, 1, G_NOT_SKIP, 3>;
    using finish_condition = make_condition<finish_receive, finish_seed::value>;
    using early_failure = meta_states::run<finish_condition, meta_iterator, meta_aligned_iterator<>>;
    static_assert(early_failure::type::states::template test<init_failed_bit>);
    static_assert(!early_failure::type::states::template test<G_CONTINUE | G_FINISH | pending_bit>);
    static_assert(early_failure::type::states::template get<u16, position_shift> == 1);
    static_assert(timer_count<typename early_failure::type::states, 1> == 2);
    static_assert([] {
        unsigned calls = 0;
        early_failure::for_each([&](auto) { ++calls; });
        return calls == 0;
    }());

    using failed_pipe = meta_ios::meta_pipe<meta_aligned_iterator<>>
        ::run<meta_iterator, meta_ios::con<meta_ios::node_con<hold_generator, true>>>;
    static_assert(uninitialized_meta_object_t<typename failed_pipe::type::to>);
    static_assert([] {
        unsigned calls = 0;
        failed_pipe::for_each([&](auto) { ++calls; });
        return calls == 0;
    }());

    struct skip_first_receive {
        template<class Window, class Flags, class Object>
        using apply = select_if_t<
            Window::value == pre_rec_exec && user_fn::index_of<Flags>() == 0,
            always_fn<skip_t>, always_fn<yield_t>>;
    };
    using skipped_receiver = meta_states::run<meta_ios::stream_states_co<skip_first_receive>,
                                             meta_aligned_iterator<>, mixed_source>;
    static_assert(initialized_meta_object_t<typename skipped_receiver::type::to>);
    static_assert(!skipped_receiver::type::states::template test<init_failed_bit | pending_bit>);
    static_assert([] {
        unsigned calls = 0;
        skipped_receiver::for_each([&](auto) { ++calls; });
        return calls == 2;
    }());

    using suppressed_flags = set_suppression_timer<flags_operator<condition_seed>,
                                                  0, G_CALL_RECEIVER, 1>;
    using suppressed_condition = typename stream_states_co::template meta_set<suppressed_flags>;
    using suppressed_receiver = meta_states::run<suppressed_condition,
                                                meta_aligned_iterator<>, mixed_source>;
    static_assert(initialized_meta_object_t<typename suppressed_receiver::type::to>);
    static_assert(!suppressed_receiver::type::states::template test<init_failed_bit | pending_bit>);
    static_assert([] {
        unsigned calls = 0;
        suppressed_receiver::for_each([&](auto view) {
            if constexpr (view.index() == 0) {
                static_assert(uninitialized_meta_object_t<typename decltype(view)::to>);
            }
            ++calls;
        });
        return calls == 3;
    }());

    struct observe_idle_receiver {
        template<class Window, class Flags, class Object>
        struct apply {
            static_assert(!Flags::template test<init_failed_bit>);
            // 接收不假设输入：轮 0 上游节点 skip 但它的 rec 是预初始化的
            // （meta_iterator::meta_set<char>），下游照收该快照。
            // 自定义 con 也要自己表态：上游结束（G_FINISH）在 post_rec 清 G_CONTINUE。
            static constexpr bool finish =
                Window::value == post_rec_exec && Flags::template test<G_FINISH>;
            static constexpr u128 value = {
                Flags::value.low & ~(G_CONTINUE & (0 - u64{finish})),
                Flags::value.high};
        };
    };
    using idle_pipe = meta_ios::meta_pipe<mixed_source>
        ::all_to<typename meta_iterator::meta_set<char>,
                 meta_ios::con<meta_ios::node_con<skip_first_receive, true>>>
        ::run<meta_aligned_iterator<>, meta_ios::con<meta_ios::node_con<observe_idle_receiver>>>;
    static_assert(initialized_meta_object_t<typename idle_pipe::type::to>);
    static_assert([] {
        unsigned calls = 0;
        idle_pipe::for_each([&](auto) { ++calls; });
        // 回调只受 G_CONTINUE / G_NOT_SKIP 门控：节点 1 的 G_NOT_SKIP 恒置，
        // 轮 0（skip 轮）也回调，共 3 次；轮 3 上游死亡后不再回调。
        return calls == 3;
    }());

    struct one_round {
        template<class Window, class Flags, class Object>
        using apply = typename Flags::template select<
            user_fn::index_of<Flags>() == 0, G_CONTINUE, 0>;
    };
    using generic_condition = make_condition<one_round, u128{condition_seed} | pending_bit>;
    using generic_loop = meta_states::run<generic_condition, meta_iterator, mixed_source>;
    static_assert(!generic_loop::type::states::template test<pending_bit>);
    using normal_loop = meta_states::run<stream_states_co, meta_aligned_iterator<>, mixed_source>;
    using empty_loop = meta_states::run<stream_states_co, meta_aligned_iterator<>, meta_istream_list<>>;
    static_assert(!normal_loop::type::states::template test<init_failed_bit | pending_bit>);
    static_assert(!empty_loop::type::states::template test<init_failed_bit>);
    static_assert(empty_loop::type::states::template test<pending_bit>);

    constexpr u128 delay_bit = u128{meta_ios_details::generator_delay_state} << user_shift;
    using advance_source = meta_invoke<mixed_source>;
    using advance_seed = flags_operator<mode_seed<0b1100, pre_gen_exec>>;
    using occupied_timer0 = set_suppression_timer<advance_seed, 0, G_NOT_SKIP, 2>;
    using occupied_timers = set_suppression_timer<occupied_timer0, 1, G_CALL_GENERATOR, 3>;
    using delayed_condition = typename stream_states_co::template meta_set<occupied_timers>;
    using delayed = meta_object_invoke<delayed_condition, advance_source>;
    static_assert(delayed::flags::value ==
                  ((occupied_timers::value & ~u128{G_CALL_GENERATOR}) | delay_bit));
    using next_round_flags = typename delayed::flags::template set<u16, position_shift, 1>;
    using resumed = meta_object_invoke<
        typename delayed::template meta_set<next_round_flags>, advance_source>;
    static_assert(resumed::flags::value ==
                  ((next_round_flags::value & ~delay_bit) | u128{G_CALL_GENERATOR}));
    using later_flags = typename resumed::flags::template set<u16, position_shift, 2>;
    using later = meta_object_invoke<
        typename resumed::template meta_set<later_flags>, advance_source>;
    static_assert(later::flags::value == later_flags::value);

    using disabled_flags = flags_operator<advance_seed::value & ~u128{G_CALL_GENERATOR}>;
    using disabled_condition = typename stream_states_co::template meta_set<disabled_flags>;
    using disabled = meta_object_invoke<disabled_condition, advance_source>;
    static_assert(disabled::flags::value == disabled_flags::value);
    using fresh = meta_object_invoke<
        typename stream_states_co::template meta_set<advance_seed>, mixed_source>;
    static_assert(fresh::flags::value == advance_seed::value);
    using other_window_flags = typename delayed::flags
        ::template set<u8, user_shift, post_gen_exec | 0b1101>;
    using other_window = meta_object_invoke<
        typename delayed::template meta_set<other_window_flags>, advance_source>;
    static_assert(other_window::flags::value == other_window_flags::value);

    struct close_advance {
        template<class Window, class Flags, class Object>
        using apply = typename Flags::template opDeact<G_CALL_GENERATOR>;
    };
    using user_closed = meta_object_invoke<
        typename meta_ios::stream_states_co<close_advance>::template meta_set<next_round_flags>,
        advance_source>;
    static_assert(user_closed::flags::value == (resumed::flags::value & ~u128{G_CALL_GENERATOR}));

    struct check_delay_windows {
        template<class Window, class Flags, class Object>
        struct apply {
            static_assert(Flags::template get<u16, timer0_shift> == 0);
            static_assert(Flags::template get<u16, timer1_shift> == 0);
            static_assert(Flags::template test<delay_bit> ==
                          (user_fn::index_of<Flags>() == 0));
            static_assert(Flags::template test<G_CALL_GENERATOR> ==
                          (user_fn::index_of<Flags>() != 0));
            static constexpr auto value = Flags::value;
        };
    };
    using delay_loop = meta_states::run<meta_ios::stream_states_co<check_delay_windows>,
                                       meta_iterator, advance_source>;
    static_assert([] {
        unsigned calls = 0;
        delay_loop::for_each([&](auto view) {
            static_assert(std::is_same_v<meta_produce_t<typename decltype(view)::to>,
                exp_select<view.index(), exp_list<char, double, int>>>);
            ++calls;
        });
        return calls == 3;
    }());

    using busy_condition = typename stream_states_co::template meta_set<occupied_timer0>;
    using busy_loop = meta_states::run<busy_condition, meta_iterator,
        meta_invoke<meta_istream_list<char, double, int, long>>>;
    static_assert([] {
        unsigned calls = 0;
        busy_loop::for_each([&](auto view) {
            static_assert(view.index() >= 2 && view.index() < 4);
            static_assert(std::is_same_v<meta_produce_t<typename decltype(view)::to>,
                exp_select<view.index(), exp_list<char, double, int, long>>>);
            ++calls;
        });
        return calls == 2;
    }());

    using advance_timer = set_suppression_timer<advance_seed, 1, G_CALL_GENERATOR, 2>;
    using timed_condition = typename stream_states_co::template meta_set<advance_timer>;
    using timed_loop = meta_states::run<timed_condition, meta_iterator, advance_source>;
    static_assert([] {
        unsigned calls = 0;
        timed_loop::for_each([&](auto view) {
            static_assert(std::is_same_v<meta_produce_t<typename decltype(view)::to>,
                exp_select<view.index(), exp_list<char, char, double, int>>>);
            ++calls;
        });
        return calls == 4;
    }());

    using initialized_fork = meta_ios::meta_pipe<advance_source>
        ::all_to<meta_iterator>::all_to<meta_iterator>
        ::fork<meta_iterator>::all_to<meta_iterator>::run<>;
    static_assert([] {
        unsigned calls = 0;
        initialized_fork::for_each([&](auto view) {
            using expected = exp_select<view.index(), exp_list<char, double, int>>;
            static_assert(std::is_same_v<decltype(view.main()), expected>);
            static_assert(std::is_same_v<decltype(view.branch()), expected>);
            ++calls;
        });
        return calls == 3;
    }());

    struct select_second {
        template<class Window, class Flags, class Object>
        using apply = typename Flags::template select<
            meta_states::user_fn::index_of<Flags>() == 1, G_NOT_SKIP, 0>;
    };

    struct count_windows {
        template<class Window, class Flags, class Object>
        using apply = typename Flags::template inc<u64, 64>;
    };

    constexpr bool check_composition() {
        using source = meta_istream_list<char, double, int>;
        using selected = meta_states_for<meta_ios::stream_states_co<select_second>, meta_iterator, source>;
        unsigned calls = 0;
        selected::for_each([&]<class Stream>(Stream) {
            static_assert(std::is_same_v<meta_produce_t<typename Stream::to>, double>);
            ++calls;
        });
        return calls == 1;
    }
    static_assert(check_composition());

    using counted = meta_states_for<meta_ios::stream_states_co<count_windows>, meta_istream_list<int>>;
    static_assert(counted::type::states::get<u64, 64> == 7);
    static_assert(!counted::type::states::test<G_CONTINUE>);

    struct data_source_fn {
        template<class Flags, class...>
        using apply = typename Flags::template inc<u64, 64>;
    };
    using data_source = meta_states_object<u128{0, 10}, data_source_fn>;

    struct observe_data_source {
        template<class Window, class Flags, class Object>
        struct apply {
            static constexpr auto value = [] {
                if constexpr (Window::value == pre_gen_exec ||
                              Window::value == post_gen_exec) {
                    static_assert(meta_states_object_t<Object>);
                    static_assert(std::is_same_v<typename Object::function, data_source_fn>);
                    static_assert(Object::flags::value.low == 0);
                }
                return Flags::value &
                    ~(user_fn::index_of<Flags>() >= 3 ? u128{G_CONTINUE} : u128{0});
            }();
        };
    };
    using data_loop = meta_states::run<meta_ios::stream_states_co<observe_data_source>,
                                      meta_iterator, data_source>;
    static_assert(data_loop::type::to_t::value.high == 13);
    static_assert([] {
        unsigned calls = 0;
        data_loop::for_each([&](auto) { ++calls; });
        return calls == 3;
    }());
    // MSO data sources are not stream-list sources: do not suppress their first advance.
    using source_observed = meta_object_invoke<meta_ios::stream_states_co<>,
                                               data_source>;
    static_assert(source_observed::flags::value == u128{condition_seed});

    struct data_receiver_fn {
        template<class Flags, class Object>
        struct apply {
            static_assert(meta_object_t<Object>);
            static constexpr auto value =
                u128{Flags::value.low, Flags::value.high + meta_produce_t<Object>::value};
        };
    };
    struct observe_data_receiver {
        template<class Window, class Flags, class Object>
        struct apply {
            static_assert(Window::value != pre_rec_exec ||
                          meta_states_object_t<Object>);
            static_assert(Window::value != post_rec_exec ||
                          meta_states_object_t<Object>);
            static constexpr auto value = Flags::value;
        };
    };
    using data_receiver = meta_states_object<u128{0, 0}, data_receiver_fn>;
    using receiver_loop = meta_states::run<
        meta_ios::stream_states_co<observe_data_receiver>,
        data_receiver, index_sequence_istream<4>>;
    static_assert(receiver_loop::type::to_t::value.high == 6);
    using receiver_pipe = meta_ios::meta_pipe<index_sequence_istream<4>>::run<
        data_receiver, meta_ios::con<meta_ios::node_con<observe_data_receiver, true>>>;
    static_assert(receiver_pipe::type::to_t::value.high == 6);

    template<class Receiver, class Generator = meta_empty_o, class... Expected>
    bool check(const char* name, exp_list<Expected...>) {
        std::vector<std::type_index> actual;
        using loop = meta_states_for<stream_states_co, Receiver, Generator>;
        loop::for_each([&]<class Stream>(Stream) {
            actual.emplace_back(typeid(meta_produce_t<typename Stream::to>));
        });
        const std::vector<std::type_index> expected{std::type_index(typeid(Expected))...};
        const bool passed = actual == expected;
        std::cout << (passed ? "PASS: " : "FAIL: ") << name
                  << " (callbacks: " << actual.size() << ")\n";
        if (!passed) {
            std::cout << "  expected:";
            for (const auto& type : expected) std::cout << ' ' << type.name();
            std::cout << "\n  actual:  ";
            for (const auto& type : actual) std::cout << ' ' << type.name();
            std::cout << '\n';
        }
        return passed;
    }

    bool run() {
        using empty_stream = meta_istream_list<>;
        using single_stream = meta_istream_list<int>;
        using multiple_stream = meta_istream_list<char, double, int>;
        using initialized_stream = meta_invoke<multiple_stream>;
        using initialized_single = meta_invoke<single_stream>;
        static_assert(uninitialized_meta_object_t<multiple_stream>);
        static_assert(initialized_meta_object_t<initialized_stream>);
        static_assert(initialize_details::has_initializer<typename initialized_stream::function>);

        unsigned failed = 0;
        std::cout << "\nStream control boundary tests:\n";
        failed += !check<empty_stream>("empty receiver, no generator", exp_list<>{});
        failed += !check<meta_iterator, empty_stream>("empty generator", exp_list<>{});
        failed += !check<single_stream>("single receiver, no generator", exp_list<int>{});
        failed += !check<meta_iterator, single_stream>("single generator", exp_list<int>{});
        failed += !check<initialized_stream>("preinitialized receiver", exp_list<char, double, int>{});
        failed += !check<meta_iterator, initialized_stream>("preinitialized generator retaining initializer", exp_list<char, double, int>{});
        failed += !check<meta_iterator, initialized_single>("preinitialized single generator", exp_list<int>{});
        failed += !check<index_sequence_istream<0>>("zero-length index receiver", exp_list<>{});
        failed += !check<meta_aligned_iterator<>>("input-required receiver without generator", exp_list<>{});
        failed += !check<meta_aligned_iterator<>, empty_stream>("input-required receiver with empty generator", exp_list<>{});

        unsigned aligned_calls = 0;
        bool aligned_ok = true;
        meta_states_for<stream_states_co, meta_aligned_iterator<>, multiple_stream>
            ::for_each([&]<class Stream>(Stream) {
                using object = typename Stream::to_t;
                constexpr bool valid_type = std::is_same_v<typename object::type, char> ||
                    std::is_same_v<typename object::type, double> ||
                    std::is_same_v<typename object::type, int>;
                static_assert(valid_type);
                constexpr auto offset = object::value;
                aligned_ok = aligned_ok && (offset % alignof(typename object::type) == 0);
                ++aligned_calls;
            });
        aligned_ok = aligned_ok && aligned_calls == 3;
        failed += !aligned_ok;
        std::cout << (aligned_ok ? "PASS: " : "FAIL: ")
                  << "input-required receiver with generator (callbacks: " << aligned_calls << ")\n";
        std::cout << "Failed cases: " << failed << '\n';
        return failed == 0;
    }
}

int main() {
    return stream_control_tests::run() ? 0 : 1;
}