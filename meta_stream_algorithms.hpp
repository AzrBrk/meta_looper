#pragma once

#include <type_traits>

#include "meta_states.hpp"
#include "meta_stream.hpp"

namespace meta_stream_algorithms_details {
using namespace meta_objects;
using namespace meta_states;

template<class Receiver>
concept payload_receiver_t = initialized_meta_object_t<Receiver> &&
                             !meta_states_object_t<Receiver>;

template<class Fn>
struct transform_fn {
  template<class State, class Input>
  using apply = meta_invoke<Fn, Input>;
};

// filter: the receiver keeps its control as a single u64 word (0 = dropped,
// 1 = kept) instead of the old u128. When the predicate fails the receiver is
// not updated; filter_condition_fn then clears PIPE_EMIT in post_rec so the
// swallowed value never reaches downstream.
template<class Receiver, u64 Control>
struct filter_state {
  using receiver = Receiver;
  static constexpr u64 control = Control;
};

template<class Pred>
struct filter_receive_fn {
  template<class State, class Input>
  using apply = typename decltype([] {
    if constexpr (meta_invoke<Pred, Input>::value) {
      return std::type_identity<
          filter_state<meta_invoke<typename State::receiver, Input>, u64{1}>>{};
    } else {
      return std::type_identity<filter_state<typename State::receiver, u64{0}>>{};
    }
  }())::type;
};

struct filter_ret {
  template<class State>
  using apply = meta_produce_t<typename State::receiver>;
};

// A Fn (not an MSO): in post_rec_exec the receive has already updated control
// (0 = dropped, 1 = kept), so gate the pipe emit bit on it. keep re-arms EMIT,
// drop clears it; the value either flows downstream or is swallowed. OBS is
// left to the default: this node's own observe only matters if it is the output
// node, and the emit bit is what actually filters a middle node.
struct filter_condition_fn {
  template<u8 Window, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    u64 w = meta_stream64::ops::yield(U);
    if constexpr (Window == post_rec_exec) {
      constexpr bool keep = Obj::type::control != 0;
      const u64 user = meta_core64::uw::user_v(w);
      const u64 emit = meta_stream_details::pipe_emit_bit;
      w = meta_core64::uw::with_user(
          w, (user & ~emit) |
                 meta_core64::select64(keep, emit, meta_core64::u64{0}));
    }
    return w;
  }();
};

struct take_tag {};

// take: control is a single u64 = remaining receives (the old u128 .low/.high
// consumed/remaining pair collapses to one word).
template<class Receiver, u64 Control>
struct take_state {
  using take_protocol = take_tag;
  using receiver = Receiver;
  static constexpr u64 control = Control;
};

struct take_fn {
  template<class State, class Input>
  struct result {
    static_assert(State::control != 0,
                  "take receiver exhausted: use take_con to stop before receiving");
    using next = meta_invoke<typename State::receiver, Input>;
    using type = take_state<next, u64{State::control - 1}>;
  };
  template<class State, class Input>
  using apply = typename result<State, Input>::type;
};

struct take_ret {
  template<class State>
  using apply = meta_produce_t<typename State::receiver>;
};

struct take_condition_fn {
  template<u8 Window, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    if constexpr (Window == pre_rec_exec) {
      static_assert(requires {
        typename Obj::type::take_protocol;
        Obj::type::control;
      }, "take_con requires a take receiver");
      // Stop BEFORE receiving once the count is exhausted (control == 0).
      // Clearing CONT here, not in post_rec, keeps the round that received the
      // Nth value observable (off-by-one: the CONT-clearing round is not
      // observed, so stop one round later than the last received round).
      if constexpr (Obj::type::control == 0) {
        return meta_stream64::ops::stop(meta_stream64::ops::yield(U));
      }
    }
    return meta_stream64::ops::yield(U);
  }();
};
}  // namespace meta_stream_algorithms_details

namespace meta_ios {
template<meta_function_t Fn>
using transform = meta_objects::meta_object<
    exp_utilities::literal_types::no_exist_type,
    meta_stream_algorithms_details::transform_fn<Fn>>;

template<meta_function_t Pred,
         meta_stream_algorithms_details::payload_receiver_t Receiver = meta_iostream::meta_iterator>
using filter = meta_objects::meta_ret_object<
    meta_stream_algorithms_details::filter_state<Receiver, meta_objects::u64{0}>,
    meta_stream_algorithms_details::filter_receive_fn<Pred>,
    meta_stream_algorithms_details::filter_ret>;

template<bool Advance = false>
using filter_con = node_con<meta_stream_algorithms_details::filter_condition_fn, Advance>;

template<class Initial, meta_function_t Fn>
using fold = meta_objects::meta_object<Initial, Fn>;

template<class List = exp_utilities::exp_list<>>
using collect = typename meta_iostream::meta_ostream<List>::template meta_set<List>;

template<std::size_t Count,
         meta_stream_algorithms_details::payload_receiver_t Receiver = meta_iostream::meta_iterator>
using take = meta_objects::meta_ret_object<
    meta_stream_algorithms_details::take_state<Receiver, meta_objects::u64{Count}>,
    meta_stream_algorithms_details::take_fn,
    meta_stream_algorithms_details::take_ret>;

template<bool Advance = false>
using take_con = node_con<meta_stream_algorithms_details::take_condition_fn, Advance>;
}  // namespace meta_ios
