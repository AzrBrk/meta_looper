#pragma once

#include "meta_states.hpp"
#include "meta_iostream.hpp"

namespace meta_ios_details {

inline constexpr meta_states::u8 no_generator_state = 2;
inline constexpr meta_states::u128 stream_owned_mask =
    meta_states_looper_details::meta_states_condition_funtion_detail::scheduler_owned_mask |
    (meta_states::u128{0x0F} << meta_states::user_shift);

struct unchanged {
    template<class Window, class Flags, class Object>
    using apply = Flags;
};

template<class Fn,
         meta_objects::meta_op_bits::u128 SkipMask =
             meta_states::G_RECEIVE | meta_states::G_NOT_SKIP,
         meta_objects::meta_op_bits::u128 YieldMask =
             meta_states::G_RECEIVE | meta_states::G_NOT_SKIP>
struct stream_fn {
    template<class Window, class Flags, class Object>
    struct apply {
        static constexpr auto controlled = [] {
            using namespace meta_states;
            using namespace meta_objects;
            constexpr auto index = user_fn::index_of<Flags>();
            constexpr bool no_generator =
                (Flags::template get<u8, user_shift> & 0x0F) == no_generator_state;

            // 观察对象可能不是元对象：当 Generator/Receiver 本身就是一个
            // meta_states_object 时，元对象调用协议传给条件的是它的 flags
            // （两个 MSO 互相调用传 flags）。此时没有流状态可看，也没有
            // initializer/end 哨兵可判 —— 基础控制原样放行，交给用户的
            // condition 自己决定。MSO 作为 gen/rec 位（例如 type_mask）因此
            // 不需要任何特殊处理。
            if constexpr (!meta_objects::meta_object_t<Object>) {
                return Flags::value;
            } else if constexpr (Window::value == pre_gen_exec) {
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
                using raw = meta_invoke<Fn, Window, controlled_flags, Object>;
                // 用户 Fn 可以返回 skip_t / yield_t / stop_t；归一化成 flags 后
                // 再恢复基础控制拥有的字段，用户门位/计时器和高位保持可写。
                using normalized =
                    meta_states_looper_details::normalize_decision<
                        raw, controlled_flags, SkipMask, YieldMask>;
                return meta_states_looper_details::restore_owned_flags<
                    stream_owned_mask, controlled_flags, normalized>::value;
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

// pipe 的公开入口在 meta_ios 里（meta_pipe / all_to / each_to / run / protocols）。
// 放在最后 include：meta_stream.hpp 自己也需要上面这个 stream_states_co，
// 两边靠 #pragma once 互相可见。
#include "meta_stream.hpp"
