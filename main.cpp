#include "meta_ios.hpp"
#include "examples/args_at.hpp"

#include <iostream>
#include <string>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_states_looper_details;

using stream_states_co = meta_ios::stream_states_co<>;

int main(){
    auto i = args_at<2>(1, 2.33, 'l', std::string("hello"));

    using L = exp_list<char, char, char, int>;

    std::cout << "no generator index_istream(needs no initialize): " << std::endl;
    meta_states_for<stream_states_co, index_sequence_istream<4>>
    ::for_each(
        [](auto states){
            std::cout << states.target_type().name() << std::endl;
        }
    );
    std::cout << "has generator index_istream(needs no initialize): " << std::endl;
     meta_states_for<stream_states_co, meta_iterator, index_sequence_istream<4>>
    ::for_each(
        [](auto states){
            std::cout << states.target_type().name() << std::endl;
        }
    );
    std::cout << "no generator meta_istream(needs initialize): " << std::endl;
    meta_states_for<stream_states_co, meta_istream<L>>
    ::for_each(
        [](auto states){
            std::cout << states.target_type().name() << std::endl;
        }
    );
    std::cout << "has generator meta_istream(needs initialize): " << std::endl;
    meta_states_for<stream_states_co, meta_iterator,  meta_istream<L>>
    ::for_each(
        [](auto states){
            std::cout << states.target_type().name() << std::endl;
        }
    );
    
    std::cout << "the meta_aligned_iterator needs a first argument to make its first advance: " << std::endl;
    std::cout << "receiver requires initialize with arguments, but no generator, expect output nothing: " << std::endl;

    meta_states_for<stream_states_co, meta_aligned_iterator>
    ::for_each(
        [](auto states){
            std::cout << states.target_type().name() << std::endl;
        }
    );

    std::cout << "receiver requires initialize with arguments, with generator: " << std::endl;
    meta_states_for<stream_states_co, meta_aligned_iterator, meta_istream<L>>
    ::for_each(
        [](auto states){
            std::cout << states.value() << std::endl;
        }
    );
    return 0;
}