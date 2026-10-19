#include"meta_states.hpp"
#include"meta_iostream.hpp"
#include <cassert>


using namespace meta_states;
using namespace meta_iostream;

template<u8 val>
using u_8t = std::integral_constant<u8, val>;

namespace stream_control{
    using namespace meta_objects;
    using namespace exp_utilities;
    using namespace literal_types;
    using namespace user_fn;

    template<class T>
    constexpr bool eol = list_end<T>;

    struct stream_fn{
        template<class u8t, class flags, class obj>
        struct apply
        {
            static constexpr auto value = flags::value;
        };
    };

    constexpr u8 gen_needs_no_initialized = 0;
    constexpr u8 gen_has_initializer = 1;
    constexpr u8 first_run = 2;
    constexpr u8 no_generator = 3;
    
    template<flags_operator_type flags>
    consteval u8 get_user_low_bits4(){
        constexpr u8 user_field = flags :: template get <u8, user_shift>;
        return user_field & 0b00001111; 
    }
    template<flags_operator_type flags>
    consteval auto clear_user_low_bits4(){
        return flags :: template field_and<u8, user_shift, 0b11110000> :: value;
    }
    template<flags_operator_type flags, u8 states>
    consteval auto set_user_low_bits4(){
        return flags :: template field_or<u8, user_shift, states>::value;
    }
    template<u8 value> using u8_t = std::integral_constant<u8, value>;

    template <std::size_t I, std::size_t count = 1>
    struct exactly_at {
        template <class stage_t, class flags, class obj>
        struct apply {
        static constexpr auto value = [] {
            using selected = typename flags::template select<
                index_of<flags>() == I, G_NOT_SKIP, 0>;
            return selected::template opDeact<
                index_of<flags>() == I + count ? G_CONTINUE : 0>::value;
        }();
        };
        template <class flags, class obj>
        struct apply<u8_t<pre_rec_exec>, flags, obj> {
        static constexpr auto value = [] {
            return (index_of<flags>() == 0 && ! is_uninitialized_meta_object<obj>) ? set_timer<flags, 0, G_RECEIVE, 1>::value
                                        : flags::value;
        }();
        };
    };
    
}//namespace stream_control

template<std::size_t I, std::size_t count = 1> 
using exactly_at_co = meta_states_object<condition_seed, state_fn<stream_control::exactly_at<I, count>>>;

namespace exactly_at_tests {
        constexpr u64 user_state = 0xFEDCBA9876543210ull;
        using seeded = flags_operator<condition_seed>::set<u64, 64, user_state>;
        using at_target = seeded::set<u16, position_shift, 2>;
        using at_end = seeded::set<u16, position_shift, 3>;

        template<class Flags, u8 Stage = pre_gen_exec>
        using result = flags_operator<stream_control::exactly_at<2>::apply<
                u_8t<Stage>, Flags, meta_empty_o>::value>;

        static_assert(result<seeded>::get<u64, 64> == user_state);
        static_assert(!result<seeded>::test<G_NOT_SKIP>);
        static_assert(result<seeded>::test<G_CONTINUE>);
        static_assert(result<at_target>::get<u64, 64> == user_state);
        static_assert(result<at_target>::test<G_NOT_SKIP>);
        static_assert(result<at_end>::get<u64, 64> == user_state);
        static_assert(!result<at_end>::test<G_CONTINUE>);
        static_assert(result<seeded, pre_rec_exec>::get<u64, 64> == user_state);
        static_assert(timer_count<result<seeded, pre_rec_exec>, 0> == 1);
        static_assert(timer_target<result<seeded, pre_rec_exec>, 0> == G_RECEIVE);
        static_assert(result<at_target, pre_rec_exec>::value == at_target::value);
        static_assert(stream_control::clear_user_low_bits4<seeded>().high == user_state);
        static_assert(stream_control::set_user_low_bits4<seeded, 1>().high == user_state);
        static_assert(meta_invoke<exactly_at_co<2>::meta_set<seeded>,
                                    meta_empty_o>::flags::get<u64, 64> == user_state);
}

template<std::size_t I, class ...Tys>
decltype (auto) args_at (Tys&& ...args) {
    return meta_states_looper_details :: meta_states_for<exactly_at_co<I>, index_sequence_istream<sizeof...(Tys)>>
    :: for_each_forward (
        [](auto states, auto&& v)->decltype(auto){
            return std::forward<decltype(v)>(v);
        }, std::forward<Tys>(args)...
    );
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

int main(){
    args_at_tests::check_identity();
    auto i = args_at<2>(1, 2.33, 'l', std::string("hello"));
}