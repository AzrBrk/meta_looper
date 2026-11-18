#pragma once

#include "meta_objects.hpp"
#include "meta_core64.hpp"
#include "meta_loop64.hpp"

// Rounds per chunk. Past this budget the loop stops on a resumable state and the
// outer driver opens the next chunk, which keeps one recursion chain from
// exhausting the template instantiation depth. Override it with
// -DMETA_STATES_LOOPER_BUDGET=... before including this header.
#ifndef META_STATES_LOOPER_BUDGET
#define META_STATES_LOOPER_BUDGET 64
#endif

// Decision markers a user function may return. The engine normalizes them into
// real instructions on top of the current user word, never treating the marker
// itself as a word:
//   skip  : keep looping, do not advance / receive this round
//   yield : do this round's receive/advance, keep looping
//   stop  : end the loop (clears CONT)
// The type forms (yield_t, ...) are for TMP `using apply = ...`; the function
// forms (yield(), ...) are convenient in constexpr code and decltype(...).
namespace meta_states_control {
struct skip_t {};
struct yield_t {};
struct stop_t {};

constexpr skip_t skip() noexcept { return {}; }
constexpr yield_t yield() noexcept { return {}; }
constexpr stop_t stop() noexcept { return {}; }
}  // namespace meta_states_control

namespace meta_loop {
namespace meta_states_looper_bits {
// ============================================================================
// Two-word control plane (design/03_state_words.md). Looper-owned state is one
// native u64; u128 is retired from the looper path (D1: class-type u128 bit ops
// are an order of magnitude slower than native u64).
//
//   looper word : GATES(8) WINDOW(8) T0C(8) T1C(8) POS(32)
//   user word   : MAIL(16) USER(48)
//
// Gates are positive (D5); the old G_NOT_SKIP is now `obs`. Timer *count* lives
// in the looper word; the timer *target* is carried as a type (D7), so there is
// no target bit-index in the word anymore.
// ============================================================================
using meta_objects::flags_operator;
using meta_objects::flags_operator_type;
using meta_core64::u8;
using meta_core64::u16;
using meta_core64::u32;
using meta_core64::u64;

namespace gates = meta_core64::gates;
namespace lw     = meta_core64::lw;
namespace uw     = meta_core64::uw;

// Old looper-facing spellings, re-pointed at the positive gates byte (bits 0..7
// of the looper word). The surrounding engine keeps reading these names.
inline constexpr u8 G_CONTINUE       = gates::cont;
inline constexpr u8 G_NOT_SKIP       = gates::obs;    // observe (was "not skip")
inline constexpr u8 G_INITIALIZE     = gates::init;
inline constexpr u8 G_CONSUME        = gates::consume;
inline constexpr u8 G_FORWARD        = gates::fwd;
inline constexpr u8 G_FINISH         = gates::finish;
inline constexpr u8 G_CALL_RECEIVER  = gates::rec;
inline constexpr u8 G_CALL_GENERATOR = gates::gen;

// Every gate bit is looper-controlled; there is no field below the gates byte.
inline constexpr u8 looper_controlled_mask = gates::mask_all;

// A timer may target exactly one positive gate bit (carried as a type).
template<u8 Target>
concept looper_controlled_bit =
    std::has_single_bit(Target) && (Target & looper_controlled_mask) == Target;

namespace details {
// Saturating decrement on a timer count byte (D16: mask-blended, no `?:`).
constexpr u8 tick8(u8 count) noexcept {
  return static_cast<u8>(count - static_cast<u8>(count != 0));
}

// One tick across both timer slots, in place on the looper word.
constexpr u64 tick(u64 word) noexcept {
  return lw::with_t1(lw::with_t0(word, tick8(lw::t0_v(word))),
                     tick8(lw::t1_v(word)));
}

// The gates byte of the looper word.
constexpr u8 gates_of(u64 word) noexcept { return lw::gates_v(word); }

// One AND-and-compare: all set_bits set and all clear_bits clear.
constexpr bool gates_have(u8 gates, u8 set_bits, u8 clear_bits = 0) noexcept {
  return (gates & (set_bits | clear_bits)) == set_bits;
}
}  // namespace details

// Tick both timer slots on the looper word.
constexpr u64 tick_timers_value(u64 word) noexcept { return details::tick(word); }
}  // namespace meta_states_looper_bits

// The looper's default word: every gate on, window 0, no timers, POS 0.
inline constexpr meta_core64::u64 meta_states_looper_default =
    meta_core64::lw::make(meta_core64::gates::defaults, 0, 0, 0, 0);

using meta_states_looper_flags = flags_operator<meta_states_looper_default>;
}  // namespace meta_loop

#include <typeinfo>
namespace meta_states_looper_details {
// for convinience of developing
// let's later delete the directly use of namespace
using namespace meta_loop;
using namespace meta_objects;
using namespace exp_utilities;
using namespace literal_types;
using namespace meta_quote;
using namespace meta_states_looper_bits;
using namespace meta_invoke_protocols;

namespace meta_states_condition_funtion_detail {
// Window numbers. The two-word model passes the window as a template parameter
// to the condition Fn (meta_core64::communicate), so these are plain 0..3
// instead of the old one-hot stage mirror values.
constexpr u8 pre_gen_exec  = 0;
constexpr u8 post_gen_exec = 1;
constexpr u8 pre_rec_exec  = 2;
constexpr u8 post_rec_exec = 3;
}  // namespace meta_states_condition_funtion_detail

using meta_states_condition_funtion_detail::pre_gen_exec;
using meta_states_condition_funtion_detail::post_gen_exec;
using meta_states_condition_funtion_detail::pre_rec_exec;
using meta_states_condition_funtion_detail::post_rec_exec;

// ============================================================================
// The stage view a for_each callback receives. It carries the receiver, the
// generator and the two-word state; projections are computed on demand and hold
// no data.
// ============================================================================
template <meta_object_t Receiver, meta_object_t Generator, class States>
struct meta_pack_stream {
  using from   = Generator;
  using to     = Receiver;
  using to_t   = get_type<to>;
  using from_t = get_type<from>;
  using cache  = meta_produce_t<Generator>;
  using states = States;

  consteval std::size_t index() {
    // POS is bumped at the round tail; minus one is "this round's index".
    return lw::pos_v(states::L) - 1;
  }
  consteval const std::type_info& target_type() {
    return typeid(meta_produce_t<to>);
  }
  consteval auto value() { return value_of<meta_produce_t<to>>; }
  consteval std::size_t left() { return exp_size<from>; }
  constexpr to_t object() const {
    static_assert(requires { to_t{}; }, "can not construct object!");
    return to_t{};
  }
};

template <class States, class Receiver, class Generator, class Driver = void>
using meta_states_feed = meta_pack_stream<Receiver, Generator, States>;

// ============================================================================
// The state carried across chunks: the full two-word state
// (meta_loop64::state<L, U, Rec, Gen, Fn>). L/U carry the control words that
// must survive a chunk boundary.
// ============================================================================
template <class State>
struct meta_states_looper_state {};

// ============================================================================
// One round: the four-window step, built on meta_loop64::step (which runs
// meta_core64::communicate per window). It exposes the same names the chunked
// driver depends on: continue_ / observe_ / consume_ / condition / receiver /
// generator / stage.
// ============================================================================
template <class State, class Driver = void, class... Args>
struct meta_states_looper_step {
  using st = meta_loop64::step<State>;

  static constexpr bool continue_ = st::cont;
  static constexpr bool observe_  = st::observed;
  static constexpr bool consume_  = st::consume;

  using next   = typename st::next;
  using condition = typename next::fn;
  using receiver  = typename next::rec;
  using generator = typename next::gen;

  using stage = meta_pack_stream<typename next::rec, typename next::gen, next>;
};

// ========================================================================
// Chunk budget + value-plane decision tags.
// ========================================================================
inline constexpr std::size_t meta_states_looper_budget = META_STATES_LOOPER_BUDGET;
static_assert(meta_states_looper_budget >= 1,
              "META_STATES_LOOPER_BUDGET must be at least 1");

// for_each tells an observed value from a control decision by the callback's
// return type.
template<class T>
struct is_for_each_skip
    : std::bool_constant<std::is_same_v<T, meta_states_control::skip_t>> {};

template<class T>
struct is_for_each_yield
    : std::bool_constant<std::is_same_v<T, meta_states_control::yield_t>> {};

template<class T>
struct is_for_each_stop
    : std::bool_constant<std::is_same_v<T, meta_states_control::stop_t>> {};

template<class T>
inline constexpr bool is_for_each_decision_v =
    is_for_each_skip<std::remove_cvref_t<T>>::value ||
    is_for_each_yield<std::remove_cvref_t<T>>::value ||
    is_for_each_stop<std::remove_cvref_t<T>>::value;

template<class T>
inline constexpr bool is_for_each_stop_v =
    is_for_each_stop<std::remove_cvref_t<T>>::value;

// ========================================================================
// Flags-driven driver / chunk.
//
// The driver state is one u64 and nothing else: a remaining-round counter and a
// phase. The chunk walk already knows everything else; every bit the driver kept
// "just in case" used to cost a template instantiation per round.
// ========================================================================
namespace driver_control {
//   bits 0..7  remaining (u8): rounds left in this chunk
//   bits 8..15 phase (u8):     run / more / done
inline constexpr std::size_t remaining_shift = 0;   // u8
inline constexpr std::size_t phase_shift = 8;       // u8
inline constexpr u64 remaining_mask = u64{0xFF} << remaining_shift;
inline constexpr u64 phase_mask = u64{0xFF} << phase_shift;

inline constexpr u8 phase_run = 0;
inline constexpr u8 phase_more = 1;
inline constexpr u8 phase_done = 2;

inline constexpr u8 window_round = 0;
inline constexpr u8 window_more = 1;
inline constexpr u8 window_decide = 3;

template<class Step, u8 Window, class Decision = void>
struct input {
  using step = Step;
  using decision = Decision;
  static constexpr u8 window = Window;
};

// One round: tick the budget (saturating at zero) and derive the phase from
// "may the loop go on" and "was the budget empty".
constexpr u64 round_value(u64 flags, bool cont) noexcept {
  const u8 remaining = static_cast<u8>(flags & remaining_mask);
  const u8 next = static_cast<u8>(remaining - (remaining != 0));
  const u8 phase = !cont ? phase_done
                  : remaining == 0 ? phase_more
                                   : phase_run;
  return (flags & ~(remaining_mask | phase_mask)) |
         (u64{next} << remaining_shift) |
         (u64{phase} << phase_shift);
}

// Hand over to the next chunk: refill the budget, back to run.
constexpr u64 more_value(u64 flags, u8 seed_remaining) noexcept {
  return (flags & ~(remaining_mask | phase_mask)) |
         (u64{seed_remaining} << remaining_shift) |
         (u64{phase_run} << phase_shift);
}

// A decision can only end the loop; no other driver bit changes.
constexpr u64 decide_value(u64 flags, bool stop) noexcept {
  return stop
      ? ((flags & ~phase_mask) | (u64{phase_done} << phase_shift))
      : flags;
}
}  // namespace driver_control

template<std::size_t Budget>
struct default_driver_policy {
  static_assert(Budget >= 1, "driver budget must be at least 1");
  static_assert(Budget <= 256, "driver remaining is u8; budget must be <= 256");
  static constexpr u8 seed_remaining = static_cast<u8>(Budget - 1);

  template<class Flags, class Input>
  struct round {
    static constexpr u64 value = driver_control::round_value(
        static_cast<u64>(Flags::value), Input::step::continue_);
    using type = flags_operator<u64{value}>;
  };

  template<class Flags, class Input>
  struct more {
    static constexpr u64 value =
        driver_control::more_value(static_cast<u64>(Flags::value), seed_remaining);
    using type = flags_operator<u64{value}>;
  };

  template<class Flags, class Input>
  struct decide {
    using decision = typename Input::decision;
    static constexpr bool stop =
        std::is_same_v<decision, meta_states_control::stop_t>;
    static constexpr u64 value =
        driver_control::decide_value(static_cast<u64>(Flags::value), stop);
    using type = flags_operator<u64{value}>;
  };
};

template<class Policy>
struct driver_fn {
  template<class Flags, class Input>
  struct apply {
    static constexpr u64 value = [] {
      if constexpr (Input::window == driver_control::window_round) {
        return Policy::template round<Flags, Input>::value;
      } else if constexpr (Input::window == driver_control::window_more) {
        return Policy::template more<Flags, Input>::value;
      } else {
        return Policy::template decide<Flags, Input>::value;
      }
    }();
  };
};

template<std::size_t Budget, class Policy = default_driver_policy<Budget>>
using flags_driver_seed_flags =
    flags_operator<u64{static_cast<u8>(Policy::seed_remaining)}
                   << driver_control::remaining_shift>;

template<std::size_t Budget, class Policy = default_driver_policy<Budget>>
using flags_driver_mso = meta_states_object<
    flags_driver_seed_flags<Budget, Policy>::value, driver_fn<Policy>>;

template<class Driver, class State, class... Args>
struct flags_chunk;
template<class Driver, class State, class... Args>
struct flags_driver;

template<class Driver, class S, class... Args>
struct flags_chunk<Driver, meta_states_looper_state<S>, Args...> {
  using step = meta_states_looper_step<S, void, Args...>;
  using round_driver = meta_invoke<Driver,
      driver_control::input<step, driver_control::window_round>>;

  static consteval u8 phase_fn() {
    return round_driver::flags::template get<u8, driver_control::phase_shift>;
  }

  using step_next_state = meta_states_looper_state<typename step::next>;
  using rest = flags_chunk<round_driver, step_next_state, Args...>;

  static consteval bool has_more_fn() {
    if constexpr (phase_fn() == driver_control::phase_more) {
      return true;
    } else if constexpr (phase_fn() == driver_control::phase_run) {
      return rest::has_more_fn();
    } else {
      return false;
    }
  }

  // Keep segment-end types behind accessors: eager aliases would expand the
  // whole segment even when a probe stops at this round.
  static consteval auto next_driver_fn() {
    if constexpr (phase_fn() == driver_control::phase_run) {
      return rest::next_driver_fn();
    } else {
      using step_next_driver = meta_invoke<round_driver,
          driver_control::input<step, driver_control::window_more>>;
      return std::type_identity<step_next_driver>{};
    }
  }

  static consteval auto next_state_fn() {
    if constexpr (phase_fn() == driver_control::phase_run) {
      return rest::next_state_fn();
    } else {
      return std::type_identity<step_next_state>{};
    }
  }

  static consteval auto feed_fn() {
    if constexpr (phase_fn() == driver_control::phase_done) {
      return std::type_identity<typename step::stage>{};
    } else if constexpr (phase_fn() == driver_control::phase_more) {
      return std::type_identity<void>{};
    } else {
      return rest::feed_fn();
    }
  }

  template<bool AcrossChunks = false>
  static consteval bool observes_fn() {
    if constexpr (step::observe_) {
      return true;
    } else if constexpr (phase_fn() != driver_control::phase_done &&
                         phase_fn() != driver_control::phase_more) {
      return rest::template observes_fn<AcrossChunks>();
    } else if constexpr (AcrossChunks &&
                         phase_fn() == driver_control::phase_more) {
      using next = flags_chunk<
          typename decltype(next_driver_fn())::type, step_next_state, Args...>;
      return next::template observes_fn<true>();
    } else {
      return false;
    }
  }

  template<std::size_t N, bool AcrossChunks = false>
  static consteval bool observes_within_fn() {
    if constexpr (N == 0) {
      return false;
    } else if constexpr (step::observe_) {
      return true;
    } else if constexpr (N == 1 && step::consume_) {
      return false;
    } else if constexpr (phase_fn() == driver_control::phase_run) {
      return rest::template observes_within_fn<N - step::consume_, AcrossChunks>();
    } else if constexpr (AcrossChunks &&
                         phase_fn() == driver_control::phase_more) {
      using next = flags_chunk<
          typename decltype(next_driver_fn())::type, step_next_state, Args...>;
      return next::template observes_within_fn<N - step::consume_, true>();
    } else {
      return false;
    }
  }

  template<class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (phase_fn() != driver_control::phase_run) {
      if constexpr (step::observe_) {
        return std::invoke(f, typename step::stage{}, std::forward<A>(a)...);
      } else {
        return;
      }
    } else {
      if constexpr (step::observe_) {
        using return_type = std::invoke_result_t<F&, typename step::stage, A...>;
        if constexpr (is_for_each_decision_v<return_type>) {
          if constexpr (is_for_each_stop_v<return_type>) {
            return std::invoke(f, typename step::stage{}, std::forward<A>(a)...);
          } else if constexpr (rest::observes_fn()) {
            (void)std::invoke(f, typename step::stage{}, std::forward<A>(a)...);
            return rest::for_each(std::forward<F>(f), std::forward<A>(a)...);
          } else {
            return std::invoke(f, typename step::stage{}, std::forward<A>(a)...);
          }
        } else if constexpr (rest::observes_fn()) {
          if constexpr (std::is_void_v<return_type>) {
            std::invoke(f, typename step::stage{}, std::forward<A>(a)...);
          } else {
            (void)std::invoke(f, typename step::stage{}, std::forward<A>(a)...);
          }
          return rest::for_each(std::forward<F>(f), std::forward<A>(a)...);
        } else {
          return std::invoke(f, typename step::stage{}, std::forward<A>(a)...);
        }
      } else {
        if constexpr (rest::observes_fn()) {
          return rest::for_each(std::forward<F>(f), std::forward<A>(a)...);
        } else {
          return;
        }
      }
    }
  }

  // Void-plane walk: the callback returns void, so no round's value is the
  // result. There is nothing to look ahead for, so observes_fn /
  // future_within_args stay out of the instantiation chain entirely.
  template<class F, class... A>
  static constexpr void for_each_void(F&& f, A&&... a) {
    if constexpr (step::observe_) {
      static_assert(std::is_void_v<std::invoke_result_t<
                        F&, typename step::stage, A...>>,
                    "for_each_void: the callback must return void; "
                    "use for_each when it returns a value or a decision");
      std::invoke(f, typename step::stage{}, std::forward<A>(a)...);
    }
    if constexpr (phase_fn() != driver_control::phase_done) {
      using next = typename decltype(forward_next_type())::type;
      next::for_each_void(std::forward<F>(f), std::forward<A>(a)...);
    }
  }

  // Consuming walk without the argument-count lookahead: one argument per
  // consuming round, stop when the arguments or the rounds run out.
  template<class F, class First, class... A>
  static constexpr void for_each_forward_void(F&& f, First&& first, A&&... a) {
    if constexpr (step::observe_) {
      static_assert(std::is_void_v<std::invoke_result_t<
                        F&, typename step::stage, First>>,
                    "for_each_forward_void: the callback must return void");
      std::invoke(f, typename step::stage{}, std::forward<First>(first));
    }
    if constexpr (phase_fn() != driver_control::phase_done &&
                  !(step::consume_ && sizeof...(A) == 0)) {
      using next = typename decltype(forward_next_type())::type;
      if constexpr (step::consume_)
        next::for_each_forward_void(std::forward<F>(f), std::forward<A>(a)...);
      else
        next::for_each_forward_void(std::forward<F>(f),
                                    std::forward<First>(first),
                                    std::forward<A>(a)...);
    }
  }

  static consteval auto forward_next_type() {
    if constexpr (phase_fn() == driver_control::phase_more)
      return std::type_identity<flags_driver<
          typename decltype(next_driver_fn())::type, step_next_state, Args...>>{};
    else return std::type_identity<rest>{};
  }

  template<std::size_t N>
  static consteval bool forward_observes() {
    return observes_within_fn<N, true>();
  }

  // Probe the next chunk directly; materializing its execution driver would also
  // request the eager final result type.
  template<std::size_t N>
  static consteval bool future_within_args() {
    if constexpr (N == 0) return false;
    else if constexpr (phase_fn() == driver_control::phase_more) {
      using next = flags_chunk<
          typename decltype(next_driver_fn())::type, step_next_state, Args...>;
      return next::template forward_observes<N>();
    } else {
      return rest::template forward_observes<N>();
    }
  }

  template<class F, class First, class... A>
  static constexpr decltype(auto) forward_tail(F&& f, First&& first, A&&... a) {
    using forward_next = typename decltype(forward_next_type())::type;
    if constexpr (step::consume_)
      return forward_next::for_each_forward(std::forward<F>(f), std::forward<A>(a)...);
    else return forward_next::for_each_forward(
        std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
  }

  template<class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first, A&&... a) {
    constexpr bool future = [] {
      if constexpr (phase_fn() == driver_control::phase_done) return false;
      else if constexpr (step::observe_) {
        if constexpr (is_for_each_stop_v<
            std::invoke_result_t<F&, typename step::stage, First>>) return false;
        else return future_within_args<sizeof...(A) + !step::consume_>();
      }
      else return future_within_args<sizeof...(A) + !step::consume_>();
    }();
    if constexpr (step::observe_) {
      using result = std::invoke_result_t<F&, typename step::stage, First>;
      if constexpr (is_for_each_stop_v<result> || !future) {
        return std::invoke(f, typename step::stage{}, std::forward<First>(first));
      } else {
        (void)std::invoke(f, typename step::stage{}, std::forward<First>(first));
        return forward_tail(std::forward<F>(f), std::forward<First>(first),
                            std::forward<A>(a)...);
      }
    } else if constexpr (future) {
      return forward_tail(std::forward<F>(f), std::forward<First>(first),
                          std::forward<A>(a)...);
    }
  }
};

template<class Driver, class State, class... Args>
struct flags_driver {
  using chunk = flags_chunk<Driver, State, Args...>;

  template<std::size_t N>
  static consteval bool forward_observes() {
    return chunk::template forward_observes<N>();
  }

  template<class Decision>
  using decide = meta_invoke<Driver,
      driver_control::input<typename chunk::step, driver_control::window_decide,
                            Decision>>;

  static consteval auto result_type() {
    if constexpr (chunk::has_more_fn()) {
      return std::type_identity<typename flags_driver<
          typename decltype(chunk::next_driver_fn())::type,
          typename decltype(chunk::next_state_fn())::type, Args...>::type>{};
    } else {
      return chunk::feed_fn();
    }
  }
  using type = typename decltype(result_type())::type;

  static consteval bool observes_fn() {
    return chunk::template observes_fn<true>();
  }

  template<class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (chunk::has_more_fn()) {
      using chunk_return = decltype(chunk::for_each(
          std::forward<F>(f), std::forward<A>(a)...));
      if constexpr (is_for_each_stop_v<chunk_return>) {
        return chunk::for_each(std::forward<F>(f), std::forward<A>(a)...);
      } else {
        using next_chunk = flags_chunk<
            typename decltype(chunk::next_driver_fn())::type,
            typename decltype(chunk::next_state_fn())::type, Args...>;
        if constexpr (next_chunk::template observes_fn<true>()) {
          using next = flags_driver<
              typename decltype(chunk::next_driver_fn())::type,
              typename decltype(chunk::next_state_fn())::type, Args...>;
          if constexpr (chunk::observes_fn()) {
            (void)chunk::for_each(std::forward<F>(f), std::forward<A>(a)...);
          }
          return next::for_each(std::forward<F>(f), std::forward<A>(a)...);
        } else {
          if constexpr (chunk::observes_fn()) {
            return chunk::for_each(std::forward<F>(f), std::forward<A>(a)...);
          } else {
            return;
          }
        }
      }
    } else {
      if constexpr (chunk::observes_fn()) {
        return chunk::for_each(std::forward<F>(f), std::forward<A>(a)...);
      } else {
        return;
      }
    }
  }

  template<class F, class... A>
  static constexpr void for_each_void(F&& f, A&&... a) {
    chunk::for_each_void(std::forward<F>(f), std::forward<A>(a)...);
  }

  template<class F, class First, class... A>
  static constexpr void for_each_forward_void(F&& f, First&& first, A&&... a) {
    chunk::for_each_forward_void(std::forward<F>(f), std::forward<First>(first),
                                 std::forward<A>(a)...);
  }

  template<class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first, A&&... a) {
    return chunk::for_each_forward(std::forward<F>(f), std::forward<First>(first),
                                   std::forward<A>(a)...);
  }
};

// ========================================================================
// Public entries; the shapes are unchanged.
// ========================================================================
template<class Condition, meta_object_t Receiver,
         meta_object_t Generator = meta_empty_o,
         class DriverPolicy = default_driver_policy<meta_states_looper_budget>,
         u64 SeedUser = 0>
struct meta_states_looper {
  using main_condition = Condition;

  template<class... Args>
  struct apply {
    using driver = flags_driver<
        flags_driver_mso<meta_states_looper_budget, DriverPolicy>,
        meta_states_looper_state<
            meta_loop64::state<meta_states_looper_default, SeedUser, Receiver,
                               Generator, Condition>>,
        Args...>;
    using type = typename driver::type;

    template<class Decision>
    using decide = typename driver::template decide<Decision>;

    template<class F, class... A>
    static constexpr decltype(auto) for_each(F&& f, A&&... a) {
      return driver::for_each(std::forward<F>(f), std::forward<A>(a)...);
    }

    template<class F, class First, class... A>
    static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                     A&&... a) {
      return driver::for_each_forward(
          std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
    }

    template<class F, class... A>
    static constexpr void for_each_void(F&& f, A&&... a) {
      driver::for_each_void(std::forward<F>(f), std::forward<A>(a)...);
    }

    template<class F, class First, class... A>
    static constexpr void for_each_forward_void(F&& f, First&& first,
                                                A&&... a) {
      driver::for_each_forward_void(
          std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
    }
  };

  template<class Decision, class... Args>
  using decide = typename meta_invoke<meta_states_looper, Args...>
      ::template decide<Decision>;
};

template <class Condition, class Receiver, class Generator, class... ARG_Tys>
using meta_states_looper_t = typename meta_invoke<
    meta_states_looper<Condition, Receiver, Generator>, ARG_Tys...>::type;

template <class Condition, class Receiver, class Generator, class DriverPolicy,
          class... ARG_Tys>
using meta_states_looper_with_policy_t = typename meta_invoke<
    meta_states_looper<Condition, Receiver, Generator, DriverPolicy>,
    ARG_Tys...>::type;

// The meta_for counterpart of meta_looper: the condition is a window Fn and the
// Generator defaults to meta_empty_o (the "no generator" spelling).
template <class Con, meta_object_t To, meta_object_t From = meta_empty_o,
          class... Args>
using meta_states_for = meta_invoke<meta_states_looper<Con, To, From>, Args...>;

template <class Con, meta_object_t To, meta_object_t From,
          class DriverPolicy, class... Args>
using meta_states_for_with_policy =
    meta_invoke<meta_states_looper<Con, To, From, DriverPolicy>, Args...>;
}  // namespace meta_states_looper_details
