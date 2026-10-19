#pragma once

#include "meta_states.hpp"
#include "meta_iostream.hpp"

namespace meta_ios_details {

inline constexpr meta_states::u8 no_generator_state = 2;

struct unchanged {
    template<class Window, class Flags, class Object>
    using apply = Flags;
};

template<class Fn>
struct stream_fn {
    template<class Window, class Flags, class Object>
    struct apply {
        static constexpr auto controlled = [] {
            using namespace meta_states;
            using namespace meta_objects;
            constexpr auto index = user_fn::index_of<Flags>();
            constexpr bool no_generator =
                (Flags::template get<u8, user_shift> & 0x0F) == no_generator_state;

            if constexpr (Window::value == pre_gen_exec) {
                if constexpr (meta_empty_object_t<Object>) {
                    using marked = typename Flags::template set<u8, user_shift,
                        static_cast<u8>((Flags::template get<u8, user_shift> & 0xF0) | no_generator_state)>;
                    return marked::template opDeact<G_ADVANCE>::value;
                } else if constexpr (initialized_meta_object_t<Object> && index == 0) {
                    return set_suppression_timer<Flags, 0, G_ADVANCE, 1>::value;
                }
            } else if constexpr (Window::value == post_gen_exec) {
                if constexpr (!meta_empty_object_t<Object> &&
                              exp_utilities::eol<meta_produce_t<Object>>) {
                    return Flags::template opDeact<G_CONTINUE>::value;
                }
            } else if constexpr (Window::value == pre_rec_exec) {
                if constexpr (no_generator && uninitialized_meta_object_t<Object> &&
                              !initialize_details::no_arguments_initializable<typename Object::function>) {
                    return Flags::template opDeact<G_CONTINUE>::value;
                } else if constexpr (no_generator && initialized_meta_object_t<Object> && index == 0) {
                    return set_suppression_timer<Flags, 0, G_RECEIVE, 1>::value;
                }
            } else if constexpr (Window::value == post_rec_exec) {
                if constexpr (no_generator && exp_utilities::eol<meta_produce_t<Object>>) {
                    return Flags::template opDeact<G_CONTINUE>::value;
                }
            }
            return Flags::value;
        }();

        using controlled_flags = meta_objects::flags_operator<controlled>;
        static constexpr auto value = [] {
            if constexpr (controlled_flags::template test<meta_states::G_CONTINUE>) {
                return meta_invoke<Fn, Window, controlled_flags, Object>::value;
            } else {
                return controlled;
            }
        }();
    };
};

}

namespace meta_ios {

template<class Fn = meta_ios_details::unchanged>
using stream_states_co = meta_objects::meta_states_object<
    meta_states::condition_seed, meta_states::state_fn<meta_ios_details::stream_fn<Fn>>>;

}