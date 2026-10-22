#pragma once

#include "meta_stream.hpp"

namespace meta_stream_algorithms_details {
using namespace meta_objects;
using namespace meta_states;

template<class Fn>
struct transform_fn {
  template<class State, class Input>
  using apply = meta_invoke<Fn, Input>;
};

template<class Pred>
struct filter_fn {
  template<class Window, class Flags, class Object>
  struct apply {
    static constexpr u128 value = [] {
      if constexpr (Window::value == post_gen_exec) {
        using input = typename decltype([] {
          if constexpr (meta_object_t<Object>) {
            return std::type_identity<meta_produce_t<Object>>{};
          } else {
            return std::type_identity<Object>{};
          }
        }())::type;
        constexpr bool keep = meta_invoke<Pred, input>::value;
        constexpr u128 mask = G_RECEIVE | G_NOT_SKIP |
                              meta_stream_details::pipe_emit_bit;
        return keep ? Flags::value | mask : Flags::value & ~mask;
      } else {
        return Flags::value;
      }
    }();
  };
};

struct take_tag {};

template<class Receiver, u128 Control>
struct take_state {
  using take_protocol = take_tag;
  using receiver = Receiver;
  static constexpr u128 control = Control;
};

struct take_fn {
  template<class State, class Input>
  struct result {
    static_assert(State::control.high != 0,
                  "take receiver exhausted: use take_con to stop before receiving");
    using next = meta_invoke<typename State::receiver, Input>;
    using type = take_state<next, u128{State::control.low + 1,
                                      State::control.high - 1}>;
  };
  template<class State, class Input>
  using apply = typename result<State, Input>::type;
};

struct take_ret {
  template<class State>
  using apply = meta_produce_t<typename State::receiver>;
};

struct take_condition_fn {
  template<class Window, class Flags, class Object>
  struct apply {
    static constexpr u128 value = [] {
      if constexpr (Window::value == pre_rec_exec ||
                    Window::value == post_rec_exec) {
        static_assert(requires {
          typename Object::type::take_protocol;
          Object::type::control;
        }, "take_con requires a take receiver");
        if constexpr (Object::type::control.high == 0) {
          return Flags::value & ~u128{G_CONTINUE};
        }
      }
      return Flags::value;
    }();
  };
};
}  // namespace meta_stream_algorithms_details

namespace meta_ios {
template<meta_function_t Fn>
using transform = meta_objects::meta_object<
    exp_utilities::literal_types::no_exist_type,
    meta_stream_algorithms_details::transform_fn<Fn>>;

template<meta_function_t Pred, bool Advance = false>
using filter_con = node_con<meta_stream_algorithms_details::filter_fn<Pred>, Advance>;

template<class Initial, meta_function_t Fn>
using fold = meta_objects::meta_object<Initial, Fn>;

template<class List = exp_utilities::exp_list<>>
using collect = meta_iostream::meta_ostream<List>;

template<std::size_t Count,
         meta_objects::initialized_meta_object_t Receiver = meta_iostream::meta_iterator>
using take = meta_objects::meta_ret_object<
    meta_stream_algorithms_details::take_state<Receiver, meta_objects::u128{0, Count}>,
    meta_stream_algorithms_details::take_fn,
    meta_stream_algorithms_details::take_ret>;

template<bool Advance = false>
using take_con = node_con<meta_stream_algorithms_details::take_condition_fn, Advance>;
}  // namespace meta_ios
