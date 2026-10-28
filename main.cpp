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
                    return suppress_callback::value;
                }
                else{
                    return Flags::value;
                }
            }();
    };
};

#include <tuple>
#include "type_safe_aligned_iterator.hpp"
namespace ts = type_safe_aligned;

struct X{int a; double b; char c; std::string s;};


int main() {
   

   std::byte data[ts::field_size<int, double , char, std::string>()];

   ts::type_safe_aligned_range<int, double ,char, std::string> range{data};

   ts::initialize(range, 10, 2.33, 'k', std::string("hello"));

   for(auto& e : range)
   {
    std::cout << e << std::endl;
   }

   X *x = reinterpret_cast<X*>(data);

   std::cout << x->a <<' '<< x->b <<' '<< x->c <<' '<< x->s <<std::endl;

   for(auto it = range.rbegin(); it != range.rend(); ++it){
    std::cout << *it << std::endl;
    it.destroy();
   }
   
   
}