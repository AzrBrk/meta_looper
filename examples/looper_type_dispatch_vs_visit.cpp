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

// Run exactly `times` rounds: stop once POS reaches times. The old spelling
// used user_fn::set_suppression to hold OBS off for times-1 rounds; the two-word
// model expresses "stop after N rounds" directly (OBS only matters for for_each,
// not for the meta_ostream collection below).
template<std::size_t times>
struct stream_timer{
    template<meta_core64::u8 Window, meta_core64::u64 L, meta_core64::u64 U,
             class Obj>
    static constexpr meta_core64::u64 value = []{
        meta_core64::u64 w = meta_stream64::ops::yield(U);
        if constexpr (Window == pre_rec_exec) {
            if (meta_core64::lw::pos_v(L) >= times) {
                w = meta_stream64::ops::stop(w);
            }
        }
        return w;
    }();
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
