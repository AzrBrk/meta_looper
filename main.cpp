#include "meta_core64.hpp"
#include"meta_ios.hpp"
#include "meta_iostream.hpp"
#include "meta_objects.hpp"
#include "meta_states.hpp"
#include "meta_stream64.hpp"
#include "meta_stream_control.hpp"
#include <bit>
#include <cstdint>

using namespace meta_ios;
using namespace meta_states;
using namespace meta_iostream;
#include<iostream>

template<std::size_t I, meta_core64::u8 count, meta_core64::u8 bits = meta_core64::gates::gen>
struct ignore_for{
  static_assert(count > 0, "ignore counting must be at least 1");
  static_assert(std::popcount(bits) <= 2, "only two at most 2 bits supported");
  static constexpr meta_core64::u8 left_bit_mask  = ~(meta_core64::u8{1} << std::countr_zero(bits));
  static constexpr meta_core64::u8 right_bit_mask = bits & ~left_bit_mask;
  template<meta_core64::u8 Window, uint64_t L, uint64_t U, class obj>
  static constexpr uint64_t value = []{
    if constexpr (meta_core64::lw::pos_v(L) == I) {
      return set_suppression_timer<bits>(U, count);
    }
    else{
      return U;
    }
  }();
};

int main(){
  static_assert(std::is_same_v<decltype(args_at<1>(1, std::string("hello"), 'k')), std::string&&>);

  std::cout << args_at<1>(1, std::string("hello"), 'k') << std::endl;

  using deleted1and2 = run<stream_states_co<ignore_for<1, 2u>>, 
  meta_ostream<exp_list<>>, 
  meta_istream_list<int, double, char, long>>::type::to_t;

  std::cout <<  typeid(deleted1and2).name() << std::endl;

  
}