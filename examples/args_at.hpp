#pragma once

#include "meta_ios.hpp"

#include <cstddef>
#include <utility>

namespace stream_control {
    template <std::size_t I, std::size_t count = 1>
    struct exactly_at {
        template <class stage_t, class flags, class obj>
        struct apply {
            static constexpr auto value = [] {
                using selected = typename flags::template select<
                    meta_states::user_fn::index_of<flags>() == I, meta_states::G_NOT_SKIP, 0>;
                return selected::template opDeact<
                    meta_states::user_fn::index_of<flags>() == I + count ? meta_states::G_CONTINUE : 0>::value;
            }();
        };
    };
}

template<std::size_t I, std::size_t count = 1>
using exactly_at_co = meta_ios::stream_states_co<stream_control::exactly_at<I, count>>;

template<std::size_t I, class... Tys>
decltype(auto) args_at(Tys&&... args) {
    return meta_states_looper_details::meta_states_for<
        exactly_at_co<I>, meta_iostream::index_sequence_istream<sizeof...(Tys)>>
    ::for_each_forward(
        [](auto, auto&& value) -> decltype(auto) {
            return std::forward<decltype(value)>(value);
        }, std::forward<Tys>(args)...
    );
}