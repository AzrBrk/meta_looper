#include "args_at.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

using namespace meta_states;
using namespace meta_objects;
using namespace meta_states::user_fn;

template<u8 val>
using u8_t = std::integral_constant<u8, val>;

namespace exactly_at_tests {
        constexpr u64 user_state = 0xFEDCBA9876543210ull;
        using seeded = flags_operator<condition_seed>::set<u64, 64, user_state>;
        using at_target = seeded::set<u16, position_shift, 2>;
        using at_end = seeded::set<u16, position_shift, 3>;

        template<class Flags, u8 Stage = pre_gen_exec>
        using result = flags_operator<stream_control::exactly_at<2>::apply<
                u8_t<Stage>, Flags, meta_empty_o>::value>;

        static_assert(result<seeded>::get<u64, 64> == user_state);
        static_assert(!result<seeded>::test<G_NOT_SKIP>);
        static_assert(result<seeded>::test<G_CONTINUE>);
        static_assert(result<at_target>::get<u64, 64> == user_state);
        static_assert(result<at_target>::test<G_NOT_SKIP>);
        static_assert(result<at_end>::get<u64, 64> == user_state);
        static_assert(!result<at_end>::test<G_CONTINUE>);
        static_assert(result<seeded, pre_rec_exec>::get<u64, 64> == user_state);
        static_assert(timer_count<result<seeded, pre_rec_exec>, 0> == 0);
        static_assert(result<at_target, pre_rec_exec>::test<G_NOT_SKIP>);
        static_assert(meta_invoke<exactly_at_co<2>::meta_set<seeded>,
                                    meta_empty_o>::flags::get<u64, 64> == user_state);
}

namespace args_at_tests {
    static_assert(std::is_same_v<decltype(args_at<1>('a', std::declval<int&>(), 3.0)), int&>);
    static_assert(std::is_same_v<decltype(args_at<1>('a', std::declval<const int&>(), 3.0)), const int&>);
    static_assert(std::is_same_v<decltype(args_at<1>('a', std::declval<int&&>(), 3.0)), int&&>);
    static_assert(std::is_same_v<decltype(args_at<1>('a', std::declval<const int&&>(), 3.0)), const int&&>);
    static_assert(std::is_same_v<decltype(args_at<2>(1, 2.33, std::declval<std::string&&>())), std::string&&>);

    template<class First, class... Rest>
    consteval bool distinct_types() {
        if constexpr (sizeof...(Rest) == 0) {
            return true;
        } else {
            return ((!std::is_same_v<std::remove_cvref_t<First>,
                                     std::remove_cvref_t<Rest>>) && ...) &&
                   distinct_types<Rest...>();
        }
    }

    template<std::size_t I, class... Args>
    void check_position(Args&&... args) {
        static_assert(distinct_types<Args...>());
        using expected = decltype(std::get<I>(
            std::forward_as_tuple(std::forward<Args>(args)...)));
        static_assert(std::is_same_v<
            decltype(args_at<I>(std::forward<Args>(args)...)), expected>);
        auto&& selected = args_at<I>(std::forward<Args>(args)...);
        auto&& reference = std::get<I>(
            std::forward_as_tuple(std::forward<Args>(args)...));
        assert(&selected == &reference);
        assert(selected == reference);
    }

    template<class... Args, std::size_t... I>
    void check_positions(std::index_sequence<I...>, Args&&... args) {
        (check_position<I>(std::forward<Args>(args)...), ...);
    }

    template<std::size_t... I>
    void check_chunk_boundary(std::index_sequence<I...>) {
        // 每个 integral_constant 都是不同类型，覆盖默认 64 轮预算的两侧。
        check_position<0>(std::integral_constant<std::size_t, I>{}...);
        check_position<62>(std::integral_constant<std::size_t, I>{}...);
        check_position<63>(std::integral_constant<std::size_t, I>{}...);
        check_position<64>(std::integral_constant<std::size_t, I>{}...);
        check_position<65>(std::integral_constant<std::size_t, I>{}...);
    }

    void check_identity() {
        int original = 42;
        char before = 'a';
        double after = 3.0;
        auto& lvalue_result = args_at<1>(before, original, after);
        auto&& rvalue_result = args_at<1>(before, std::move(original), after);
        static_assert(std::is_same_v<decltype(lvalue_result), int&>);
        static_assert(std::is_same_v<decltype(rvalue_result), int&&>);
        assert(&lvalue_result == &original);
        assert(&rvalue_result == &original);
        lvalue_result = 7;
        assert(original == 7);

        const double fixed = 3.5;
        char letter = 'z';
        std::string text = "heterogeneous";
        auto owned = std::make_unique<int>(99);
        using positions = std::make_index_sequence<5>;
        check_positions(positions{}, original, fixed, letter, text, owned);
        check_positions(positions{}, std::move(original), std::move(fixed),
                        std::move(letter), std::move(text), std::move(owned));
        check_positions(positions{}, 17, 2.5, 'x', std::string("temporary"),
                        std::make_unique<int>(23));

        auto* address = owned.get();
        auto moved = args_at<3>(original, fixed, text, std::move(owned), letter);
        assert(moved.get() == address);
        assert(*moved == 99);
        assert(!owned);

        assert(args_at<0>(17, 2.5, 'x', std::string("last")) == 17);
        assert(args_at<1>(17, 2.5, 'x', std::string("last")) == 2.5);
        assert(args_at<2>(17, 2.5, 'x', std::string("last")) == 'x');
        assert(args_at<3>(17, 2.5, 'x', std::string("last")) == "last");
        check_chunk_boundary(std::make_index_sequence<66>{});
    }
}

int main() {
    args_at_tests::check_identity();
}