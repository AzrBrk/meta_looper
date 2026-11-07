#pragma once

#include "meta_ios.hpp"

#include <utility>

namespace print_detail {
using namespace meta_states;
using namespace meta_objects;
using namespace exp_utilities;

inline constexpr u128 ready = u128{1} << 64;
inline constexpr u128 delivered = u128{1} << 65;

struct condition {
    template<class Window, class Flags, class Object>
    struct apply {
        static constexpr u128 value = [] {
            u128 flags = Flags::value;
            if constexpr (Window::value == pre_gen_exec) {
                flags = flags & ~u128{G_NOT_SKIP | G_CONSUME};
                if (flags & (ready | delivered)) flags = flags & ~u128{G_ADVANCE};
                else flags = flags | u128{G_ADVANCE};
            } else if constexpr (Window::value == post_gen_exec) {
                // Inspect the newly generated character before the receiver sees it.
                if constexpr (std::is_same_v<meta_produce_t<Object>, exp_char<'#'>> ||
                              std::is_same_v<meta_produce_t<Object>, exp_char<'\0'>>) {
                    if (!(flags & delivered)) flags = flags | ready;
                }
            } else if constexpr (Window::value == pre_rec_exec) {
                if constexpr (Flags::template test<delivered>) {
                    return (flags & ~delivered) | u128{G_INITIALIZE};
                } else if constexpr (Flags::template test<ready> &&
                                     !Flags::template test<G_ADVANCE> &&
                                     initialized_meta_object_t<Object>) {
                    using output_flags = flags_operator<
                        (Flags::value & ~ready) | delivered | u128{G_NOT_SKIP | G_CONSUME}>;
                    return set_suppression_timer<output_flags, 0, G_RECEIVE, 1>::value;
                }
            }
            return flags;
        }();
    };
};
}

template<exp_utilities::static_str s>
void print(auto& os, auto&&... args) {
    using namespace exp_utilities;
    constexpr std::size_t placeholders = [] {
        std::size_t count = 0;
        for (char c : s.str) count += c == '#';
        return count;
    }();
    static_assert(placeholders == sizeof...(args), "print: placeholder/argument count mismatch");

    meta_states::run<meta_ios::stream_states_co<print_detail::condition>,
        meta_iostream::meta_ostream<>, meta_iostream::meta_istream<str_to_list<s>>>
        ::for_each_forward([&](auto str_stream, auto&& arg) {
            using fragment = typename decltype(str_stream.object())::pop_back;
            using terminated = typename fragment::template push_back<exp_char<'\0'>>;
            os << to_exp_char_t<terminated>{} << std::forward<decltype(arg)>(arg);
        }, std::forward<decltype(args)>(args)..., "");
}
