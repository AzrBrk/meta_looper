#include "args_at.hpp"

#include <cassert>
#include <string>
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
    static_assert(std::is_same_v<decltype(args_at<0>(std::declval<int&>())), int&>);
    static_assert(std::is_same_v<decltype(args_at<0>(std::declval<const int&>())), const int&>);
    static_assert(std::is_same_v<decltype(args_at<0>(std::declval<int&&>())), int&&>);
    static_assert(std::is_same_v<decltype(args_at<0>(std::declval<const int&&>())), const int&&>);
    static_assert(std::is_same_v<decltype(args_at<2>(1, 2.33, std::declval<std::string&&>())), std::string&&>);

    void check_identity() {
        int original = 42;
        auto& lvalue_result = args_at<1>('a', original, 3.0);
        auto&& rvalue_result = args_at<1>('a', std::move(original), 3.0);
        static_assert(std::is_same_v<decltype(lvalue_result), int&>);
        static_assert(std::is_same_v<decltype(rvalue_result), int&&>);
        assert(&lvalue_result == &original);
        assert(&rvalue_result == &original);
        lvalue_result = 7;
        assert(original == 7);
    }
}

int main() {
    args_at_tests::check_identity();
}