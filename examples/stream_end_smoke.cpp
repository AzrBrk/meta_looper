#include "meta_ios.hpp"

using namespace meta_states;
using namespace meta_iostream;
using meta_objects::meta_produce_t;

template<std::size_t I>
struct my_int : end_of_stream<(I == 0)>, std::integral_constant<std::size_t, I>
{};

namespace stream_end_tests {
    struct custom_end {};
    struct ended : end_of_stream<true, custom_end> {};
    struct active : end_of_stream<false> { using result = int; };
    struct unmarked { using result = double; };
    struct invalid_marker { bool end; using end_type = custom_end; };
    struct projection {
        template<class State, class...>
        using apply = typename State::result;
    };
    struct argument_projection {
        template<class State, class Argument>
        using apply = Argument;
    };
    static_assert(end_of_stream_t<my_int<0>>);
    static_assert(end_of_stream_t<my_int<3>>);
    static_assert(!end_of_stream_t<unmarked>);
    static_assert(!end_of_stream_t<invalid_marker>);
    static_assert(std::is_same_v<meta_invoke<end_of_ret<projection>, ended>, custom_end>);
    static_assert(std::is_same_v<meta_invoke<end_of_ret<projection>, active>, int>);
    static_assert(std::is_same_v<meta_invoke<end_of_ret<projection>, unmarked>, double>);
    static_assert(std::is_same_v<meta_invoke<end_of_ret<argument_projection>, active, char>, char>);
    static_assert(std::is_same_v<meta_invoke<end_of_ret<argument_projection>, ended>, custom_end>);
    static_assert(std::is_same_v<meta_produce_t<index_sequence_istream<0>>, end_of_list>);
    using single = index_sequence_istream<1>;
    static_assert(std::is_same_v<meta_produce_t<single>, std::integral_constant<std::size_t, 0>>);
    static_assert(std::is_same_v<meta_produce_t<meta_invoke<single>>, end_of_list>);
    using infinite = meta_index_istream<3>;
    static_assert(meta_produce_t<infinite>::value == 3);
    static_assert(meta_produce_t<meta_invoke<infinite>>::value == 4);
}

int main() {}