#include"meta_ios.hpp"
#include "meta_iostream.hpp"
#include "meta_stream.hpp"
#include"type_safe_aligned_iterator.hpp"

#include <type_traits>
#include<variant>


using namespace meta_ios;
namespace tsa = type_safe_aligned;
using namespace exp_utilities;
using namespace meta_states;
using namespace meta_iostream;

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


namespace test{
    template<class TL, std::size_t N>
    using make_TL_N_times = typename run<
        stream_states_co<stream_timer<N>>, 
        meta_ostream<>, 
        meta_repeat_istream<TL>>::type::to_t;
    template<template<typename ...> class TL, typename ...Tys>
    consteval std::size_t field_size_of_list(TL<Tys...>){
        return tsa::field_size<0, Tys...>();
    }
    template<template<typename ...> class TL, typename ...Tys>
    constexpr auto make_tsa_field_type(TL<Tys...>){
        return std::type_identity<tsa::type_safe_aligned_range<0, Tys...>>{};
    }

}

int main(){

}