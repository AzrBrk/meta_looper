#include "meta_ios.hpp"

#include <iostream>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <coroutine>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_states_looper_details;
using namespace meta_op_bits;




template<class Type>
// 仅用于打印：从编译器函数签名中提取完整类型名，不参与 TMP 计算。
constexpr std::string_view type_name() {
#if defined(__clang__) || defined(__GNUC__)
    constexpr std::string_view signature = __PRETTY_FUNCTION__;
    constexpr auto begin = signature.find("Type = ") + 7;
    constexpr auto separator = signature.find(';', begin);
    constexpr auto end = separator == std::string_view::npos
        ? signature.rfind(']') : separator;
#elif defined(_MSC_VER)
    constexpr std::string_view signature = __FUNCSIG__;
    constexpr auto begin = signature.find("type_name<") + 10;
    constexpr auto end = signature.rfind(">(void)");
#else
    return "type name unavailable";
#endif
#if defined(__clang__) || defined(__GNUC__) || defined(_MSC_VER)
    return signature.substr(begin, end - begin);
#endif
}

template<std::size_t Steps>
struct stream_timer{
    template<class stage_t, class Flags, class obj>
    struct apply{
        static constexpr u128 value = 
            (meta_states::user_fn::index_of<Flags>() == Steps) ? Flags::value&~G_CONTINUE : Flags::value;
    };
};

template<std::size_t I, std::size_t count = 1>
struct ignore_at{
    template<class stage_t, class Flags, class obj>
    struct apply{
        static constexpr u128 value = 
            []{
                if constexpr (meta_states::user_fn::index_of<Flags>() == I) {
                    //at timer1 set suppression to G_RECEIVE for count times
                    using suppress_receive = set_suppression_timer<Flags, 1, G_RECEIVE, count>;
                    using suppress_callback = set_suppression_timer<suppress_receive, 0, G_NOT_SKIP, count>;
                    return suppress_receive::value;
                }
                else{
                    return Flags::value;
                }
            }();
    };
};

#include <tuple>
#include <bitset>


int main() {
    using list = std::tuple<int, double, char, long, u64>;
    list tp{10, 2.33, 'k', 43, 65ull};
    auto t = meta_states::run<meta_ios::stream_states_co<ignore_at<1,3>>, meta_ostream<std::tuple<>>, meta_bistream<list>>
    ::for_each([](auto states, auto &&...args){
    using current = typename decltype(states)::to;
    std::cout << type_name<meta_produce_t<current>>() << std::endl;
    //输入流缓存
    std::cout << type_name<typename decltype(states)::cache>() << std::endl;
    //注意这里flags的低16位会比index多一，这是因为编译期先算完了才把状态给回调
    std::cout << std::bitset<64>(states.flags().low) << std::endl;
    if constexpr (exp_size<current> != 2){
        return meta_states::skip();
    }
    else{
        return meta_produce_t<current>{std::move(args)...};
    }
   }, std::get<0>(tp), std::get<4>(tp));

   std::cout << std::get<0>(t) <<',' << std::get<1>(t) << std::endl;
}