
#include <iostream>
#include"meta_ios.hpp"
#include<tuple>


using namespace meta_ios;
using namespace exp_utilities;

template<std::size_t times>
struct stream_timer{
    template<class window, class flags, class obj>
    struct apply {
        static constexpr meta_states::u128 value = []{
            if constexpr (meta_states::user_fn::index_of<flags>() >= times){
                return flags::value & ~meta_states::G_CONTINUE;
            }
            else if constexpr (meta_states::user_fn::index_of<flags>() == 0){
                return meta_states::user_fn::set_suppression<
                    flags, meta_states::G_NOT_SKIP, times - 1>();
            }
            else{
                return flags::value;
            }
        }();
    };
};

int main() {

   using list = exp_list<int, double, char, double, std::string>;

   auto iter = meta_states::run<stream_states_co<stream_timer<3>>,
                                meta_iostream::meta_aligned_iterator<8>,
                                meta_iostream::meta_istream<list>>
   ::for_each([](auto states){
    std::cout << "type:    " << typeid(typename decltype(states)::to_t::type).name()
              << " offset: " << states.value() << std::endl;
    return states.object();
   });
}