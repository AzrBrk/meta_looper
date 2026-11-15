#pragma once

// ============================================================================
// meta_stream: a compile-time pipeline (meta_pipe) built on the looper's single-step logic.
//
// The old meta_stream wrapped (To, From) in one meta object that maintained stream_op
// bits, changed_pred and index accounting itself, so control logic lived on the object.
// The current design instead:
//
//   * the pipe itself is the looper: the state is a flat slot set folded once per round;
//   * each stage is a node (rec, con) where con only controls that one stage and never
//     contributes to the final product;
//   * one node's single-step result becomes the next stage's generator (no generator slot
//     between stages);
//   * a node step reuses the low-level window primitives and handles input availability
//     plus emit/flush on its own, keeping the four windows, timers, position and settle
//     semantics identical;
//   * main is the first con and owns global state: the high 64 bits are the node
//     liveness bitmap, the low 64 bits are the pipe's own position, and it decides
//     whether the whole pipe continues.
//
// Internal shape (output -> input; build<...> writes it in reverse, like ranges):
//
//   meta_pipe_impl<
//       main,
//       recN, conN,          // last node (the final product)
//       ...
//       rec0, con0,          // first node (next to the generator)
//       gen>
//
// Every round folds from the generator toward the output:
//
//   node0: input = generator (the default con keeps G_CALL_GENERATOR on and advances it)
//   nodek: input = the previous node's rec' (the default con keeps G_CALL_GENERATOR off;
//          middle stages have no generator)
//   each node's rec' is the next stage's generator.
//
// Every node con carries two pipe-reserved bits in its flags, both in the high 64. They
// never clash with main's bitmap because each object carries its own flags:
//
//   PIPE_EMIT  (bit 64): whether a value received this round is passed downstream (filter clears it)
//   PIPE_FLUSH (bit 65): whether the current rec is flushed downstream on the round the node dies
//                        (wait_for_end: receive only while alive, emit the final value on death)
//
// Termination rules:
//   * a non-output node dying still passes its final result downstream this round, and the pipe stops after it;
//   * an output node dying clears its liveness bit and is never called again; main decides on the bitmap;
//   * by default main stops once every node is dead (bitmap == 0).
//
// User API (exported through meta_ios):
//
//   meta_pipe<gen>
//     ::all_to<rec>                       // default con
//     ::all_to<rec, con<C>>               // explicit con (con comes last)
//     ::all_to<rec, con<wait_for_end_con<>>>  // receive only, flush on death
//     ::run<rec>                          // final stage plus the default main
//     ::run<rec, con<C>, main_con<M>>
//     ::run<...>::for_each / ::for_each<Ps...>
//     ::run<...>::type                    // pipe_view: to / to_t / from / states
// ============================================================================

#include "meta_stream_control.hpp"
#include "meta_iostream.hpp"

namespace meta_ios {

// Explicit marker: con always follows rec.
template <class C>
struct con {};
template <class M>
struct main_con {};

}  // namespace meta_ios

namespace meta_stream_details {
using namespace exp_utilities;
using namespace literal_types;
using namespace meta_objects;
using namespace meta_states;
using namespace meta_states_looper_details;
using namespace meta_invoke_protocols;

// ============================================================================
// 1) Pipe flag conventions
// ============================================================================
//
// The high 64 bits of every node con's flags belong to the pipe:
//   bit 64 = PIPE_EMIT，bit 65 = PIPE_FLUSH。
// main's high 64 bits are the node liveness bitmap (bit k = node k is alive, counted from
// the input end). main is not a node con, so the two use separate flags and never collide.

inline constexpr u128 pipe_emit_bit = u128{1} << 64;
inline constexpr u128 pipe_flush_bit = u128{1} << 65;
// Engine-owned high-64 segments: bits 64/65 = emit/flush; 66..127 belong to the con latch.
inline constexpr u128 pipe_engine_high_mask = pipe_emit_bit | pipe_flush_bit;
inline constexpr std::size_t pipe_mask_shift = 64;

// Default con behaviour: G_FINISH (upstream finished) means "stop here".
//   pre_rec_exec clears G_CALL_RECEIVER (upstream is gone, no closing receive);
//   post_rec_exec clears G_CONTINUE (stop at the end of this round).
// The engine never decides a node's death; it only translates "upstream finished" into a
// G_FINISH event. The default node writes "upstream finished, so I stop" into its own con.
// The user Fn runs first and G_FINISH is honoured afterwards; with the default Fn
// (unchanged) the composition is a pure stop_on_finish. A node_con with an explicit
// custom Fn does not get this layer: whether it observes G_FINISH and stops is its own
// business, which is exactly how drain works - it keeps receiving flushed products after
// the upstream died, following G_CALL_RECEIVER.
template <class Fn>
struct stop_on_finish_or_fn {
  template <class Window, class Flags, class Object>
  struct apply {
    using inner = meta_invoke<Fn, Window, Flags, Object>;
    static constexpr bool finish =
        inner::template test<meta_states::G_FINISH>;
    static constexpr bool pre_rec_stop =
        Window::value == pre_rec_exec && finish;
    static constexpr bool post_rec_stop =
        Window::value == post_rec_exec && finish;
    static constexpr u128 value = {
        inner::value.low &
            ~(meta_states::G_CALL_RECEIVER & (0 - u64{pre_rec_stop})) &
            ~(meta_states::G_CONTINUE & (0 - u64{post_rec_stop})),
        inner::value.high};
  };
};

// Starting flags of the default node con:
//   ADVANCE on:  node 0 (it advances the generator)
//   ADVANCE off: every other node (no generator in between, only a receiver)
//   EMIT on, FLUSH off
inline constexpr u128 node_seed_advance =
    u128{meta_states::condition_seed} | pipe_emit_bit;
inline constexpr u128 node_seed_static =
    u128{meta_states::condition_seed & ~meta_states::G_CALL_GENERATOR} | pipe_emit_bit;
// wait_for_end: receive only while alive; the engine flushes on the round the node dies.
inline constexpr u128 node_seed_wait =
    u128{meta_states::condition_seed & ~meta_states::G_CALL_GENERATOR} | pipe_flush_bit;
inline constexpr u128 node_seed_wait_advance =
    u128{meta_states::condition_seed} | pipe_flush_bit;

// Pipe-specific stream protocol (fork stream_fn). The only difference from the shared
// stream_fn is window (2): the generator eol check clears G_CONTINUE only when the
// generator advanced this round (G_CALL_GENERATOR set) or started empty (position == 0).
// A branch's input-most node has advance off and merely borrows the main source element;
// source exhaustion reaches the con as a G_FINISH event (SourceDied), and the stream
// protocol must not kill the branch on its own - liveness belongs to the con gates alone.
template <class Fn,
          meta_objects::meta_op_bits::u128 SkipMask =
              meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP,
          meta_objects::meta_op_bits::u128 YieldMask =
              meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP>
struct pipe_stream_fn {
  template <class Window, class Flags, class Object>
  struct apply {
    static constexpr auto controlled = [] {
      constexpr auto index = user_fn::index_of<Flags>();
      constexpr u128 flags = Flags::value;
      constexpr bool no_generator =
          ((Flags::value.low >> user_shift) & meta_ios_details::no_generator_state) != 0;

// An MSO observer receives the whole object; a data MSO's flags do not follow the stream
// layout, so neither the first-round delay nor the end-of-stream test is derived for it -
// the user condition decides.
      if constexpr (meta_states_object_t<Object> || !meta_object_t<Object>) {
        return flags;
      } else if constexpr (Window::value == pre_gen_exec) {
        if constexpr (meta_empty_object_t<Object>) {
          return (flags & ~u128{G_CALL_GENERATOR}) |
              (u128{meta_ios_details::no_generator_state} << user_shift);
        } else {
          constexpr u128 delay =
              u128{meta_ios_details::generator_delay_state} << user_shift;
          if ((flags.low & delay.low) != 0) {
            return (flags & ~delay) | u128{G_CALL_GENERATOR};
          }
          if constexpr (initialized_meta_object_t<Object> && index == 0) {
// Restore only the advance gate this wrapper closed; never open pipe middle nodes or branches.
            if ((flags.low & G_CALL_GENERATOR) != 0) {
              return (flags & ~u128{G_CALL_GENERATOR}) | delay;
            }
          }
        }
      } else if constexpr (Window::value == post_gen_exec) {
        if constexpr (!meta_empty_object_t<Object> &&
                      exp_utilities::eol<meta_produce_t<Object>> &&
                      (((flags.low & G_CALL_GENERATOR) != 0) || index == 0)) {
          return flags & ~u128{G_CONTINUE};
        }
      } else if constexpr (Window::value == pre_rec_exec) {
        if constexpr (no_generator && uninitialized_meta_object_t<Object> &&
                      !initialize_details::no_arguments_initializable<typename Object::function>) {
          return (flags & ~u128{G_CONTINUE}) | receiver_init_failed_mask;
        } else if constexpr (no_generator && initialized_meta_object_t<Object> && index == 0) {
          return meta_loop::meta_states_looper_bits::details::arm_timer_value(
              flags, timer0_shift,
              (u16{1} << bits::timer_count_shift) | std::countr_zero(G_CALL_RECEIVER),
              false);
        }
      } else if constexpr (Window::value == post_rec_exec) {
        if constexpr (no_generator && exp_utilities::eol<meta_produce_t<Object>>) {
          return flags & ~u128{G_CONTINUE};
        }
      }
      return flags;
    }();

    using controlled_flags = meta_objects::flags_operator<controlled>;
    static constexpr auto value = [] {
      if constexpr ((controlled.low & meta_states::G_CONTINUE) != 0) {
        using raw = meta_invoke<Fn, Window, controlled_flags, Object>;
// The user Fn may return skip_t / yield_t / stop_t; normalize them to flags, then restore
// the fields owned by the base control. User gates, timers and the high bits stay writable.
        return meta_states_looper_details::restore_owned_value(
            meta_ios_details::stream_owned_mask, controlled,
            meta_states_looper_details::decision_value<raw>(
                controlled, SkipMask, YieldMask));
      } else {
        return controlled;
      }
    }();
  };
};

// Builds a pipe node con with the pipe stream protocol (pipe_stream_fn); skip/yield
// normalization must also handle PIPE_EMIT (node 0 additionally carries ADVANCE).
template <class Fn, u128 Flags>
using pipe_stream_co = meta_states_object<
    Flags, meta_states::state_fn<pipe_stream_fn<
              Fn,
              meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP | pipe_emit_bit,
              meta_states::G_CALL_RECEIVER | meta_states::G_NOT_SKIP |
                  pipe_emit_bit>>>;

template <bool Advance>
using default_node_con = pipe_stream_co<
    stop_on_finish_or_fn<meta_ios_details::unchanged>,
    Advance ? node_seed_advance : node_seed_static>;

// User-defined node con: Advance = true advances the input by default (node 0), otherwise
// it does not. Fn selection: default or explicit unchanged -> implicit stop_on_finish
// (G_FINISH clears G_CONTINUE at post_rec); an explicit custom Fn takes over completely and
// does not compose stop_on_finish - a con that keeps draining after upstream death simply
// ignores G_FINISH.
template <class Fn, bool Advance>
struct node_con_fn {
  using type = Fn;
};
template <bool Advance>
struct node_con_fn<meta_ios_details::unchanged, Advance> {
  using type = stop_on_finish_or_fn<meta_ios_details::unchanged>;
};

template <class Fn = meta_ios_details::unchanged, bool Advance = false>
using node_con = pipe_stream_co<
    typename node_con_fn<Fn, Advance>::type,
    Advance ? node_seed_advance : node_seed_static>;

// wait_for_end: usable in a middle stage (ADVANCE off, EMIT off, FLUSH on). On node 0 set
// Advance to true. When upstream finishes, stop_on_finish kills the node and the engine
// flushes the final product that round.
template <class Fn = meta_ios_details::unchanged, bool Advance = false>
using wait_for_end_con =
    pipe_stream_co<stop_on_finish_or_fn<Fn>,
                   Advance ? node_seed_wait_advance : node_seed_wait>;

// Node 0: wait_for_end while also advancing the generator.
template <class Fn = meta_ios_details::unchanged>
using wait_for_end_advance_con =
    pipe_stream_co<stop_on_finish_or_fn<Fn>, node_seed_wait_advance>;

// Default main con: continue while any node is alive, stop once all are dead.
struct pipe_main_fn {
  template <class Flags, class Rec>
  struct apply {
    static constexpr u64 mask = Flags::value.high;
// Clears G_CONTINUE when mask == 0 (every node dead). An all-ones / all-zeros mask replaces
// the conditional: G_CONTINUE & (0 - (mask == 0)).
    static constexpr u128 value = {
        Flags::value.low & ~(meta_states::G_CONTINUE & (0 - u64{mask == 0})),
        Flags::value.high};
  };
};

using pipe_main_co =
    meta_states_object<u128{meta_states::condition_seed}, pipe_main_fn>;

// ============================================================================
// 1b) fork: one branch and the main line consume the same source element (same index frame).
// ============================================================================
//
// The main line occupies the low part of the liveness bitmap and the branch follows at a
// compile-time offset; total nodes <= 64. The branch never participates in the continue
// decision: the main line alone keeps the pipe running, and branch liveness only shows up in
// the merged product and the bitmap.

// Keeps the two end receivers; pipe_view exposes .main() / .branch() through the produce protocol.
template <class Main, class Branch>
struct pipe_fork {
  using main_t = Main;
  using branch_t = Branch;
};

template <class T>
struct fork_parts {
  using main_t = T;
  using branch_t = void;
  static constexpr bool forked = false;
};
template <class M, class B>
struct fork_parts<pipe_fork<M, B>> {
  using main_t = M;
  using branch_t = B;
  static constexpr bool forked = true;
};

// branch_size_of / branch_rec are defined after output_node (they need it).

// ============================================================================
// 2) Nodes and node lists
// ============================================================================
template <class Con, class Rec>
struct pipe_node {
  using con = Con;
  using rec = Rec;
};

template <class... Nodes>
struct pipe_nodes {
  static constexpr std::size_t size = sizeof...(Nodes);
};

template <class Node, class List>
struct prepend_node;
template <class Node, class... Nodes>
struct prepend_node<Node, pipe_nodes<Nodes...>> {
  using type = pipe_nodes<Node, Nodes...>;
};

// The node list runs output -> input; head is the final-product node.
template <class Nodes>
struct output_node;
template <class Head, class... Rest>
struct output_node<pipe_nodes<Head, Rest...>> {
  using rec = typename Head::rec;
};

// Output node con: callback gating (G_NOT_SKIP / G_CONTINUE) reads its flags.
template <class Nodes>
struct output_con;
template <class Head, class... Rest>
struct output_con<pipe_nodes<Head, Rest...>> {
  using type = typename Head::con;
};

template <class Nodes, class Gen>
struct output_prev;
template <class Head, class Gen>
struct output_prev<pipe_nodes<Head>, Gen> {
  using type = Gen;
};
template <class Head, class Second, class... Rest, class Gen>
struct output_prev<pipe_nodes<Head, Second, Rest...>, Gen> {
  using type = typename Second::rec;
};

// Branch is the branch node list (pipe_nodes<...>) or void when there is no fork.
// These two traits must be defined after output_node.
template <class T>
struct branch_size_of {
  static constexpr std::size_t value = 0;
};
template <class T>
  requires (!std::is_void_v<T>)
struct branch_size_of<T> {
  static constexpr std::size_t value = T::size;
};

template <class T>
struct branch_rec {
  using type = void;
};
template <class T>
  requires (!std::is_void_v<T>)
struct branch_rec<T> {
  using type = typename output_node<T>::rec;
};

template <std::size_t Count>
inline constexpr u64 initial_alive_mask =
    Count >= 64 ? ~u64{0} : ((u64{1} << Count) - 1);

// Small shift-safe helper: n == 64 saturates instead of invoking 1 << 64 / x << 64 UB.
constexpr u64 low_mask(std::size_t n) noexcept {
  return n >= 64 ? ~u64{0} : ((u64{1} << n) - 1);
}
constexpr u64 mask_shr(u64 v, std::size_t n) noexcept {
  return n >= 64 ? u64{0} : (v >> n);
}
constexpr u64 mask_shl(u64 v, std::size_t n) noexcept {
  return n >= 64 ? u64{0} : (v << n);
}

// Looper bit helpers (gates byte, gate mask queries) are reused directly by the pipe layer.
namespace lb = meta_loop::meta_states_looper_bits::details;

// ============================================================================
// 2b) Pipe fork: strip the looper's window / settle accounting out of the pipe
// ============================================================================
//
// The looper's G_FINISH is a "round complete" trigger consumed at settle (early accounting
// plus stage reset). Inside the pipe it is only an event bit: the engine translates
// "upstream finished" into a G_FINISH seed for the con, and death depends solely on whether
// the con clears G_CONTINUE. The pipe therefore cannot reuse the looper's user_flags_control
// settle, which would trigger round accounting and stage rollback early.
//
// High-bit plan: bits 64/65 = pipe_emit_bit / pipe_flush_bit (read by the engine, written by
// skip/yield normalization); bits 66..127 = the con's own latch (pipe_latch_bit<N>).
namespace pfd = meta_states_condition_funtion_detail;

// Con latch segment: N = 0..61 maps to bits 66..127. The restore owned mask excludes the
// high bits, so latches written by a con survive across windows.
template <std::size_t N>
  requires (N < 62)
inline constexpr u128 pipe_latch_bit = u128{1} << (66 + N);

// Scheduler bits the pipe owns: window number plus position. The whole high 64
// (emit/flush/latch) belongs to the con: restore only takes those two fields from before and
// accepts the con's output for every other bit.
inline constexpr u128 pipe_owned_mask = u128{position_mask} | pfd::stage_mask;

// fork receiver_low: a receiver initialization failure is the only case where the looper
// decides for the upstream (the object is gone, so it must stop); keep it verbatim.
constexpr u64 pipe_receiver_low(u64 low, bool failed) noexcept {
  const u64 enabled = 0 - u64{failed};
  return (low & ~(G_CONTINUE & enabled)) |
         ((u64{pfd::receiver_init_failed_state} << user_shift) & enabled);
}

constexpr u128 pipe_receiver_value(u128 flags, bool failed) noexcept {
  flags.low = pipe_receiver_low(flags.low, failed);
  return flags;
}

// fork settle_low. It differs from the looper in exactly three points:
//   1) G_FINISH is not part of the finished test: the event bit survives verbatim and is rewritten by the next seed;
//   2) accounting (position + 1) and timer ticks happen only at the round tail (post_rec); G_FINISH never triggers them early;
//   3) the stage advances unconditionally: next_stage already wraps 0x80 back to 0x10, so no finished branch is needed.
constexpr u64 pipe_settle_low(u64 low, u8 window,
                              bool receiver_failed = false) noexcept {
  low = pipe_receiver_low(low, window == post_rec_exec && receiver_failed);
  constexpr u64 stage_mask_low = u64{pfd::stage_index_mask} << user_stage_shift;
const u64 round_end = 0 - u64{window == post_rec_exec};  // round end = all ones, otherwise zero
  const u64 counted = (low & ~position_mask) | ((low + 1) & position_mask);
  const u64 kept = (lb::tick_low(counted) & round_end) | (low & ~round_end);
  return (kept & ~stage_mask_low) |
         (u64{pfd::next_stage(window)} << user_stage_shift);
}

constexpr u128 pipe_settle_value(u128 flags, u8 window,
                                 bool receiver_failed = false) noexcept {
  flags.low = pipe_settle_low(flags.low, window, receiver_failed);
  return flags;
}

// Engine translation (branch-agnostic): upstream finished -> G_FINISH event. G_CALL_RECEIVER
// belongs to the con, so the engine never touches it: "upstream produced nothing" is a
// physical fact of this round, expressed by the receive decision (_receive_) compounded with
// HasValue, and it is not written into the con flags. The con's stance on G_CALL_RECEIVER
// (when to open, when to close) persists across rounds; the engine only rewrites the
// G_FINISH event bit.
constexpr u128 pipe_seed_value(u128 flags, bool UpstreamDied) noexcept {
  const u64 finish = G_FINISH & (0 - u64{UpstreamDied});
  return {(flags.low & ~G_FINISH) | finish, flags.high};
}

// fork invoke_window: keeps meta_object_invoke<staged, Obj> plus restore_owned_value, with
// the pipe's own owned mask and the pipe settle.
template <u8 Stage, class Cond_o, class Obj>
struct pipe_invoke_window_impl {
  static constexpr u128 staged_value =
      pfd::with_stage_value(Cond_o::flags::value, Stage);
  static consteval auto result_type() {
    if constexpr (Stage == post_rec_exec &&
                  pfd::initialization_failed(staged_value,
                                             uninitialized_meta_object_t<Obj>)) {
      return std::type_identity<typename Cond_o::template meta_set<
          flags_operator<pipe_settle_value(staged_value, Stage, true)>>>{};
    } else {
      using staged =
          typename Cond_o::template meta_set<flags_operator<staged_value>>;
      using called = meta_object_invoke<staged, Obj>;
      constexpr u128 value = pipe_settle_value(
          restore_owned_value(pipe_owned_mask, staged_value,
                              called::flags::value),
          Stage);
      return std::type_identity<
          typename called::template meta_set<flags_operator<value>>>{};
    }
  }
  using type = typename decltype(result_type())::type;
};

template <u8 Stage, class Cond_o, class Obj>
using pipe_invoke_window =
    typename pipe_invoke_window_impl<Stage, Cond_o, Obj>::type;

// fork enter_window_step: inside the pipe G_FINISH no longer trims the following windows
// (the event bit must reach post_rec for stop_on_finish); Gate uses G_CONTINUE instead - once
// the master gate is cleared the remaining windows of the round take a lightweight branch
// (Fn is never called again), equivalent behaviour with fewer instantiations.
template <bool Gate, u8 Stage, class Cond_o, class Obj>
struct pipe_enter_window_step_impl {
  static consteval auto result_type() {
    if constexpr (!Gate) {
      if constexpr (Stage == post_rec_exec &&
                    uninitialized_meta_object_t<Obj>) {
        if constexpr (pfd::initialization_failed(Cond_o::flags::value, true)) {
          return std::type_identity<typename Cond_o::template meta_set<
              flags_operator<
                  pipe_receiver_value(Cond_o::flags::value, true)>>>{};
        } else return std::type_identity<Cond_o>{};
      } else return std::type_identity<Cond_o>{};
    } else {
      return std::type_identity<pipe_invoke_window<Stage, Cond_o, Obj>>{};
    }
  }
  using type = typename decltype(result_type())::type;
};

template <bool Gate, u8 Stage, class Cond_o, class Obj>
using pipe_enter_window_step =
    typename pipe_enter_window_step_impl<Gate, Stage, Cond_o, Obj>::type;

// ============================================================================
// 3) One step: shared window primitives, with its own input / emit / flush scheduling
// ============================================================================
//
// Input:
//   HasValue      whether upstream really produced a value this round (false when a filter swallowed it)
//   UpstreamDied  whether upstream died this round (this node dies too, but may receive one last flush)
// Output:
//   con / rec     new node state
//   emitted       whether this node emitted downstream this round
//   just_died     whether this node died this round
template <bool GenValid, class Con, class Rec, class Gen, bool HasValue,
          bool UpstreamDied, bool IsSource, class... Args>
struct pipe_node_step_impl;

// Upstream produced nothing: the four windows do not run (window (1) needs a generator that does not exist yet).
// Only one round is settled: the engine seeds the event bit and the con has its say in window (4) (stop_on_finish watches G_FINISH).
template <class Con, class Rec, class Gen, bool HasValue, bool UpstreamDied,
          bool IsSource, class... Args>
struct pipe_node_step_impl<false, Con, Rec, Gen, HasValue, UpstreamDied, IsSource,
                           Args...> {
  static constexpr bool initialize = gated_bits<G_CONTINUE | G_INITIALIZE>(
      lb::effective_value(Con::flags::value));
  static constexpr u128 executed_flags =
      pfd::after_execution(Con::flags::value, initialize, false);
// Engine translation: upstream finished -> G_FINISH event (G_CALL_RECEIVER belongs to the con).
  static constexpr u128 seeded_value =
      pipe_seed_value(executed_flags, UpstreamDied);
  using seeded_con =
      typename Con::template meta_set<flags_operator<seeded_value>>;
  using rec = std::conditional_t<initialize, meta_object_initialize<Rec>, Rec>;
// Window (4) settlement: the con sees G_FINISH at post_rec and decides whether to stop; a receiver
// initialization failure is the only exception (the receiver_failed case of pipe_invoke_window).
  using con = pipe_invoke_window<post_rec_exec, seeded_con, rec>;
  using gen_out = Gen;
  static constexpr bool _receive_ = false;
// Death depends solely on whether the con cleared G_CONTINUE.
  static constexpr bool just_died = !con::flags::template test<G_CONTINUE>;
  static constexpr bool emit_flag = con::flags::template test<pipe_emit_bit>;
  static constexpr bool flush_flag = con::flags::template test<pipe_flush_bit>;
  static constexpr bool has_produce = exp_utilities::has_type<rec>;
  static constexpr bool emitted = just_died && flush_flag && has_produce;
};
// A normal step: the four-window logic.
template <class Con, class Rec, class Gen, bool HasValue, bool UpstreamDied,
          bool IsSource, class... Args>
struct pipe_node_step_impl<true, Con, Rec, Gen, HasValue, UpstreamDied,
                           IsSource, Args...> {
// ---- seed: the engine translates upstream state (before window (1)) ----
  static constexpr u128 seeded_value =
      pipe_seed_value(Con::flags::value, UpstreamDied);
  using seeded_con =
      typename Con::template meta_set<flags_operator<seeded_value>>;

// ---- Window (1) pre_gen_exec ----
  using pre_gen_exec_stage_o = pipe_invoke_window<pre_gen_exec, seeded_con, Gen>;
  static constexpr u128 pre_gen_judge =
      lb::effective_value(pre_gen_exec_stage_o::flags::value);
  static constexpr u8 pre_gen_gates = lb::gates_of(pre_gen_judge);

  static constexpr bool _entered_ = pre_gen_gates & lb::gate_continue;
// G_FINISH no longer takes part in engine decisions: it is only an event bit for the con (the seed handles it).
  static constexpr bool _advance_ = lb::gates_have(
      pre_gen_gates, lb::gate_continue | lb::gate_advance);
  static constexpr bool _initialize_gen_ = IsSource && lb::gates_have(
      pre_gen_gates, lb::gate_continue | lb::gate_initialize);

// Node 0 has ADVANCE on by default, so it really advances the generator;
// middle nodes have ADVANCE off, so invoke_if<false> does not call upstream (it was already updated).
  static consteval auto generator_type() {
    if constexpr (_initialize_gen_) return std::type_identity<meta_object_initialize<Gen>>{};
    else return std::type_identity<meta_invoke<invoke_if<_advance_>, Gen, Args...>>{};
  }
  using generator_stage_o = typename decltype(generator_type())::type;
  using after_gen_condition = typename pre_gen_exec_stage_o::template meta_set<
      flags_operator<pfd::after_execution(
          pre_gen_exec_stage_o::flags::value, _initialize_gen_, false)>>;

// ---- Window (2) post_gen_exec: G_FINISH does not trim windows (the event bit travels to post_rec),
//      but once G_CONTINUE is cleared the remaining windows take the lightweight branch (Fn is never called again). ----
  using post_gen_exec_stage_o = pipe_enter_window_step<
      _entered_, post_gen_exec, after_gen_condition, generator_stage_o>;
  static constexpr u128 post_gen_judge =
      lb::effective_value(post_gen_exec_stage_o::flags::value);

// ---- Window (3) pre_rec_exec ----
  static constexpr bool _post_gen_gate_ =
      lb::gates_of(post_gen_judge) & lb::gate_continue;
  using pre_rec_exec_stage_o = pipe_enter_window_step<
      _post_gen_gate_, pre_rec_exec, post_gen_exec_stage_o, Rec>;
  static constexpr u128 pre_rec_judge =
      lb::effective_value(pre_rec_exec_stage_o::flags::value);
  static constexpr u8 pre_rec_gates = lb::gates_of(pre_rec_judge);

// Receiving depends only on the con gates (G_CONTINUE / G_CALL_RECEIVER). The pipe never assumes an
// input: if G_CALL_RECEIVER is set it invokes with the upstream rec produce - whatever that is (a real
// value, the end_of_list marker, an uninitialized object), as decided by the meta_object_invoke protocol
// and the con Fn; the receiver itself may also be a generator, and the protocol defines that behaviour.
  static constexpr bool _receive_ =
      _entered_ &&
      lb::gates_have(pre_rec_gates, lb::gate_continue | lb::gate_receive);
  static constexpr bool _initialize_rec_ =
      _entered_ &&
      lb::gates_have(pre_rec_gates, lb::gate_continue | lb::gate_initialize);

  static consteval auto receiver_type() {
    if constexpr (_initialize_rec_) return std::type_identity<meta_object_initialize<Rec>>{};
    else return std::type_identity<
        meta_invoke<invoke_object_if<_receive_>, Rec, generator_stage_o>>{};
  }
  using result_stage_o = typename decltype(receiver_type())::type;
  using after_rec_condition = typename pre_rec_exec_stage_o::template meta_set<
      flags_operator<pfd::after_execution(
          pre_rec_exec_stage_o::flags::value, _initialize_rec_, _receive_)>>;

// ---- Window (4) post_rec_exec ----
  static constexpr bool _pre_rec_gate_ = pre_rec_gates & lb::gate_continue;
  using post_rec_exec_stage_o = pipe_enter_window_step<
      _pre_rec_gate_, post_rec_exec, after_rec_condition, result_stage_o>;
  static constexpr u128 post_rec_judge =
      lb::effective_value(post_rec_exec_stage_o::flags::value);
  static constexpr u8 post_rec_gates = lb::gates_of(post_rec_judge);

  static constexpr bool _continue_ =
      _entered_ && lb::gates_have(post_rec_gates, lb::gate_continue);
// Death depends solely on whether the con cleared G_CONTINUE (the engine never decides for a node).
  static constexpr bool just_died = !_continue_;

  using con = post_rec_exec_stage_o;  // G_FINISH is kept; the next round rewrites it via the seed
  using rec = result_stage_o;
// Only the input-most node turns the generator into a new type, and that type must travel to the next round.
  using gen_out = generator_stage_o;

// ---- Emit / flush ----
  static constexpr bool emit_flag = con::flags::template test<pipe_emit_bit>;
  static constexpr bool flush_flag = con::flags::template test<pipe_flush_bit>;
// An uninitialized receiver has no valid produce and cannot emit.
  static constexpr bool has_produce = exp_utilities::has_type<rec>;
// Three conditions collapse into one bit expression: a normal round uses receive & emit, a dying round only honours flush.
  static constexpr bool emitted =
      has_produce && (((_receive_ && !_initialize_rec_) & emit_flag) | (just_died & flush_flag));
};

// The input end (IsSource) still runs its first step with an uninitialized generator (the con advances it);
// when a downstream node sees no upstream produce: a dead upstream kills this node, a live one idles.
template <class Con, class Rec, class Gen, bool HasValue, bool UpstreamDied,
          bool IsSource, class... Args>
struct pipe_node_step
    : pipe_node_step_impl<IsSource || exp_utilities::has_type<Gen>, Con, Rec,
                          Gen, HasValue, UpstreamDied, IsSource, Args...> {};

// One node's result inside the fold.
template <bool Alive, bool IsOutput, bool IsInputMost, std::size_t Index,
          u64 Mask, class GenOut, class InValue, bool HasValue,
          bool UpstreamDied, class Node, class... Args>
struct pipe_fold_node;

// A dead node is not called, stays verbatim and emits nothing.
template <bool IsOutput, bool IsInputMost, std::size_t Index, u64 Mask,
          class GenOut, class InValue, bool HasValue, bool UpstreamDied,
          class Node, class... Args>
struct pipe_fold_node<false, IsOutput, IsInputMost, Index, Mask, GenOut,
                      InValue, HasValue, UpstreamDied, Node, Args...> {
  using node = Node;
  using out = typename Node::rec;
  using gen_out = GenOut;
// A dead node's final produce: only nodes with FLUSH (wait_for_end and friends) hand their produce downstream
// on death; after that the downstream drains at its own pace - has_value stays true.
// An uninitialized rec has no produce.
  static constexpr bool flush_dead =
      Node::con::flags::template test<pipe_flush_bit>;
  static constexpr bool has_value = exp_utilities::has_type<out> && flush_dead;
  static constexpr bool just_died = false;
  static constexpr u64 mask = Mask;
};

// A live node: take one step.
template <bool IsOutput, bool IsInputMost, std::size_t Index, u64 Mask,
          class GenOut, class InValue, bool HasValue, bool UpstreamDied,
          class Node, class... Args>
struct pipe_fold_node<true, IsOutput, IsInputMost, Index, Mask, GenOut,
                      InValue, HasValue, UpstreamDied, Node, Args...> {
  using step = pipe_node_step<typename Node::con, typename Node::rec, InValue,
                              HasValue, UpstreamDied, IsInputMost, Args...>;
  using node = pipe_node<typename step::con, typename step::rec>;
  using out = typename step::rec;
  using gen_out =
      std::conditional_t<IsInputMost, typename step::gen_out, GenOut>;
  static constexpr bool has_value = step::emitted;
  static constexpr bool just_died = step::just_died;
// just_died is 0/1 and shifting it by Index yields exactly the bit to clear: one AND-NOT finishes the reduction.
  static constexpr u64 mask = Mask & ~(u64{just_died} << Index);
};

// ============================================================================
// 4) One round's fold (input -> output, right fold)
// ============================================================================
template <class Nodes, std::size_t Index, bool IsOutput, u64 Mask,
          class InValue, bool InHasValue, bool InDied, class... Args>
struct pipe_fold;

template <std::size_t Index, bool IsOutput, u64 Mask, class InValue,
          bool InHasValue, bool InDied, class... Args>
struct pipe_fold<pipe_nodes<>, Index, IsOutput, Mask, InValue, InHasValue,
                 InDied, Args...> {
  using nodes = pipe_nodes<>;
  using out = InValue;
  using prev = InValue;
  using gen_out = InValue;
// HasValue of the input-most node comes from the physical fact passed to the fold (true on the main line:
// node 0 advances the generator; on a branch it is "the source frame produce is not end_of_list").
  static constexpr bool has_value = InHasValue;
  static constexpr bool just_died = false;
  static constexpr u64 mask = Mask;
  static constexpr bool middle_died = false;
  static constexpr bool output_died = false;
};

template <class Head, class... Rest, std::size_t Index, bool IsOutput, u64 Mask,
          class InValue, bool InHasValue, bool InDied, class... Args>
struct pipe_fold<pipe_nodes<Head, Rest...>, Index, IsOutput, Mask, InValue,
                 InHasValue, InDied, Args...> {
// Handle the node closer to the input first (the list tail), then hand the result to head.
  using inner = pipe_fold<pipe_nodes<Rest...>, Index - 1, false, Mask, InValue,
                          InHasValue, InDied, Args...>;

  static constexpr bool alive = (Mask >> Index) & 1;
  static constexpr bool is_input_most = (sizeof...(Rest) == 0);
// Upstream died: a middle node reads its upstream liveness bit (cleared in the post-round bitmap = died now
// or earlier, and G_FINISH is visible in the same round); the input-most node has no bitmap upstream and uses
// the InDied passed to the fold (false on the main line - node 0 advances the generator itself and window (2)
// checks eol; the branch fold passes "the main source node died" because both consume the same element).
// The bitmap bit is cleared by the upstream node's own just_died; the engine only translates "upstream died"
// into a G_FINISH event and never decides death for downstream.
  static constexpr bool upstream_dead =
      is_input_most ? InDied : ((inner::mask >> (Index - 1)) & 1) == 0;

  using node_result = pipe_fold_node<
      alive, IsOutput, is_input_most, Index, inner::mask, typename inner::gen_out,
      typename inner::out, inner::has_value, upstream_dead, Head, Args...>;

  using nodes = typename prepend_node<typename node_result::node,
                                      typename inner::nodes>::type;
  using out = typename node_result::out;
  using prev = std::conditional_t<IsOutput, typename inner::out,
                                  typename inner::prev>;
  using gen_out = typename node_result::gen_out;

  static constexpr bool has_value = node_result::has_value;
  static constexpr bool just_died = node_result::just_died;
  static constexpr u64 mask = node_result::mask;
  static constexpr bool middle_died =
      inner::middle_died || (node_result::just_died && !IsOutput);
  static constexpr bool output_died =
      inner::output_died || (node_result::just_died && IsOutput);
};

// ============================================================================
// 5) One round: node fold + branch + main update
// ============================================================================
// A branch round: take the element the main line consumed this round (folded::gen_out) and run the whole fold.
// With no fork (Branch = void) nothing happens. SourceDied = the main source node (bitmap bit 0) is dead: the
// branch consumes the same source element, so source exhaustion is its "upstream finished".
template <bool Forked, class Branch, class InValue, u64 Mask, bool SourceDied,
          class... Args>
struct pipe_branch_fold_result {
  using nodes = void;
  using out = void;
  static constexpr u64 mask = 0;
  static constexpr bool died = false;
};
template <class Branch, class InValue, u64 Mask, bool SourceDied, class... Args>
struct pipe_branch_fold_result<true, Branch, InValue, Mask, SourceDied,
                               Args...> {
// The branch consumes the same source element frame; receiving does not assume an input (G_CALL_RECEIVER gates
// the invocation and meta_object_invoke unwraps the argument); InHasValue no longer takes part in the decision.
  using folded = pipe_fold<Branch, Branch::size - 1, true, Mask, InValue, true,
                           SourceDied, Args...>;
  using nodes = typename folded::nodes;
  using out = typename folded::out;
  static constexpr u64 mask = folded::mask;
  static constexpr bool died = folded::output_died;
};

template <class Main, class Nodes, class Gen, class Branch = void,
          class... Args>
struct pipe_round {
  static constexpr bool forked = !std::is_void_v<Branch>;
  static constexpr std::size_t main_nodes = Nodes::size;
  static constexpr std::size_t branch_nodes = branch_size_of<Branch>::value;
  static constexpr u64 mask_raw = Main::flags::value.high;

// The liveness bitmap has two segments: main bits 0..M-1 and branch bits M..M+B-1 (compile-time offset).
  static constexpr u64 mask_main = mask_raw & low_mask(main_nodes);
  static constexpr u64 mask_branch =
      forked ? (mask_shr(mask_raw, main_nodes) & low_mask(branch_nodes))
             : u64{0};
  static constexpr std::size_t index =
      Main::flags::value.low & meta_states::position_mask;

  using folded = pipe_fold<Nodes, Nodes::size - 1, true, mask_main, Gen, true,
                           false, Args...>;
  using branch_fold =
      pipe_branch_fold_result<forked, Branch, typename folded::gen_out,
                              mask_branch, (folded::mask & 1) == 0, Args...>;

  using nodes = typename folded::nodes;
  using branch = typename branch_fold::nodes;
  using out = std::conditional_t<forked,
                                 pipe_fork<typename folded::out,
                                           typename branch_fold::out>,
                                 typename folded::out>;
  using prev = typename folded::prev;
  using gen_out = typename folded::gen_out;

  static constexpr u64 mask_after =
      folded::mask | mask_shl(branch_fold::mask, main_nodes);

// Writes the new liveness bitmap into main's high 64 and records one step of position.
// position is a low 16-bit counter: it wraps under the mask after +1 and every other bit stays.
  static constexpr u64 low_before = Main::flags::value.low;
  static constexpr u64 low_counted =
      (low_before & ~meta_states::position_mask) |
      ((low_before + 1) & meta_states::position_mask);
  using flags_counted = flags_operator<u128{low_counted, folded::mask}>;
  using main_staged = typename Main::template meta_set<flags_counted>;
// main's algorithm receives the whole node object, consistent with the MSO observer protocol.
// main only reads the main-line bitmap; branch liveness never affects continue.
  using main_out_raw = meta_invoke<main_staged, typename folded::out>;
// position and the liveness bitmap belong to the engine and cannot be overwritten by main's Fn.
  static constexpr u128 main_owned_mask =
      u128{meta_states::position_mask} | (~u128{0} << pipe_mask_shift);
  using main_out_restored = typename main_out_raw::template meta_set<
      restore_owned_flags<main_owned_mask, flags_counted,
                          typename main_out_raw::flags>>;
  using main_out = typename main_out_restored::template meta_set<
      flags_operator<u128{main_out_restored::flags::value.low, mask_after}>>;

  static constexpr bool has_value = folded::has_value;
  static constexpr bool middle_died = folded::middle_died;
  static constexpr bool output_died = folded::output_died;
// Callback gating: only the output node con's G_CONTINUE / G_NOT_SKIP matter (as with the looper observe),
// independent of whether a value was emitted this round. A node that dies always clears G_CONTINUE
// (just_died = !_continue_), so a dead node stops calling back automatically.
  static constexpr bool observe_ = lb::gates_have(
      lb::gates_of(output_con<typename folded::nodes>::type::flags::value),
      lb::gate_not_skip | lb::gate_continue);
// Stop decision: an all-zero bitmap means no node is alive, so running on is pure spinning - the pipe's
// physical floor, not a decision about any node (a node's fate belongs to its con). Above that only main's
// G_CONTINUE matters (the default main clears it when mask == 0; a custom main may stop earlier).
  static constexpr bool continue_ =
      (mask_after != 0) &&
      main_out::flags::template test<meta_states::G_CONTINUE>;
};

// ============================================================================
// 6) Final and per-round views
// ============================================================================
template <class To, class From, class States, std::size_t Index,
          class Driver = void, std::size_t BranchNodes = 0>
struct pipe_view {
  using to = To;
  using from = From;
  using states = States;
  using driver = Driver;
  static constexpr std::size_t branch_nodes = BranchNodes;
  using to_t = get_type<to>;
  using from_t = get_type<from>;
  using cache = meta_produce_t<from>;

// Main and branch produce through the object protocol; without a fork the void branch product is never instantiated.
  using parts = fork_parts<to>;
  static constexpr bool forked = parts::forked;
  using main_t = meta_produce_t<typename parts::main_t>;
  using branch_t = select_if_t<forked, meta_quote::unary<meta_produce_t>,
                               always_fn<void>, typename parts::branch_t>;

  consteval std::size_t index() const { return Index; }
  consteval std::size_t left() const { return exp_size<from_t>; }
  consteval auto value() const { return value_of<to_t>; }
  constexpr auto object() const { return to_t{}; }
// After fork: s.main() is the main product and s.branch() the branch product (same round, same index).
  constexpr auto main() const { return main_t{}; }
  constexpr auto branch() const requires forked { return branch_t{}; }
  consteval const std::type_info& target_type() const { return typeid(to_t); }
};

// ============================================================================
// 7) Engine: run pipe_round each round, then recurse
// ============================================================================
// ============================================================================
// 7) Chunked continuation: pipe state / chunk / driver
// ============================================================================
template <class Main, class Nodes, class Gen, class Branch = void>
struct pipe_state {};

// Each round folds about as deep as the node count; the budget adapts to that count so that
// "rounds per chunk * depth per round + chunks" stays under the template depth limit.
template <std::size_t NodeCount>
inline constexpr std::size_t pipe_chunk_budget =
    std::clamp<std::size_t>(256 / (NodeCount + 4), 1, 64);

template <class Round, class Driver = void>
using pipe_round_view = pipe_view<typename Round::out, typename Round::prev,
                                  typename Round::main_out::flags,
                                  Round::index, Driver, Round::branch_nodes>;

template <class State>
struct pipe_state_budget;
template <class Main, class Nodes, class Gen, class Branch>
struct pipe_state_budget<pipe_state<Main, Nodes, Gen, Branch>> {
  static constexpr std::size_t value = pipe_chunk_budget<Nodes::size>;
};

template <class Main, class Nodes, class Gen, class Branch = void>
using default_pipe_driver_policy = default_driver_policy<
    pipe_state_budget<pipe_state<Main, Nodes, Gen, Branch>>::value>;

// ============================================================================
// 7b) Flags-driven pipe chunk / driver
//
// Shares flags_driver_mso / driver_control with the looper;
// the pipe step adapter hands Round::observe_/continue_ to the driver policy.
// ============================================================================
template <class Round>
struct pipe_driver_step {
  using round = Round;
  static constexpr bool continue_ = Round::continue_;
  static constexpr bool observe_ = Round::observe_;
  static constexpr bool consume_ = Round::main_out::flags::template test<G_CONSUME>;
};

template <class Driver, class State>
struct pipe_flags_chunk;
template <class Driver, class State>
struct pipe_flags_driver;

template <class Driver, class Main, class Nodes, class Gen, class Branch>
struct pipe_flags_chunk<Driver, pipe_state<Main, Nodes, Gen, Branch>> {
  using round = pipe_round<Main, Nodes, Gen, Branch>;
  using step = pipe_driver_step<round>;
  using round_driver = meta_invoke<Driver,
      driver_control::input<step, driver_control::window_round>>;
// The callback sees the driver state as it was before this round, matching the looper's meta_pack_stream.
  using view = pipe_round_view<round, Driver>;

  static constexpr u8 phase =
      round_driver::flags::template get<u8, driver_control::phase_shift>;

  using step_next_state = pipe_state<typename round::main_out,
                                     typename round::nodes,
                                     typename round::gen_out,
                                     typename round::branch>;
  using step_next_driver = meta_invoke<round_driver,
      driver_control::input<step, driver_control::window_more>>;
  using rest = pipe_flags_chunk<round_driver, step_next_state>;

  static consteval bool has_more_fn() {
    if constexpr (phase == driver_control::phase_more) {
      return true;
    } else if constexpr (phase == driver_control::phase_run) {
      return rest::has_more;
    } else {
      return false;
    }
  }
  static constexpr bool has_more = has_more_fn();

  static consteval auto next_driver_fn() {
    if constexpr (phase == driver_control::phase_run) {
      return std::type_identity<typename rest::next_driver>{};
    } else {
      return std::type_identity<step_next_driver>{};
    }
  }
  using next_driver = typename decltype(next_driver_fn())::type;

  static consteval auto next_state_fn() {
    if constexpr (phase == driver_control::phase_run) {
      return std::type_identity<typename rest::next_state>{};
    } else {
      return std::type_identity<step_next_state>{};
    }
  }
  using next_state = typename decltype(next_state_fn())::type;

  static consteval auto feed_fn() {
    if constexpr (phase == driver_control::phase_done) {
      return std::type_identity<view>{};
    } else if constexpr (phase == driver_control::phase_more) {
      return std::type_identity<void>{};
    } else {
      return std::type_identity<typename rest::feed>{};
    }
  }
  using feed = typename decltype(feed_fn())::type;

  static constexpr bool observes = [] {
    if constexpr (phase == driver_control::phase_done ||
                  phase == driver_control::phase_more) {
      return step::observe_;
    } else {
      return any_observes<step::observe_, rest>::value;
    }
  }();

  template <std::size_t N>
  static consteval bool observes_within_fn() {
    if constexpr (phase != driver_control::phase_run) {
      return N > 0 && step::observe_;
    } else if constexpr (N == 0) {
      return false;
    } else {
      return any_within<step::observe_, N - step::consume_, rest>::value;
    }
  }

  static constexpr std::size_t consumed = [] {
    if constexpr (phase == driver_control::phase_run)
      return std::size_t{step::consume_} + rest::consumed;
    else return std::size_t{step::consume_};
  }();

  template <class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (phase != driver_control::phase_run) {
      if constexpr (step::observe_) {
        return std::invoke(f, view{}, std::forward<A>(a)...);
      } else {
        return;
      }
    } else {
      if constexpr (step::observe_) {
        using return_type =
            std::invoke_result_t<F&, view, A...>;
        if constexpr (is_for_each_decision_v<return_type>) {
          if constexpr (is_for_each_stop_v<return_type>) {
            return std::invoke(f, view{}, std::forward<A>(a)...);
          } else if constexpr (rest::observes) {
            (void)std::invoke(f, view{}, std::forward<A>(a)...);
            return rest::for_each(std::forward<F>(f),
                                  std::forward<A>(a)...);
          } else {
            return std::invoke(f, view{}, std::forward<A>(a)...);
          }
        } else if constexpr (rest::observes) {
          if constexpr (std::is_void_v<return_type>) {
            std::invoke(f, view{}, std::forward<A>(a)...);
          } else {
            (void)std::invoke(f, view{}, std::forward<A>(a)...);
          }
          return rest::for_each(std::forward<F>(f),
                                std::forward<A>(a)...);
        } else {
          return std::invoke(f, view{}, std::forward<A>(a)...);
        }
      } else {
        if constexpr (rest::observes) {
          return rest::for_each(std::forward<F>(f),
                                std::forward<A>(a)...);
        } else {
          return;
        }
      }
    }
  }

  static consteval auto forward_next_type() {
    if constexpr (phase == driver_control::phase_more)
      return std::type_identity<pipe_flags_driver<next_driver, next_state>>{};
    else return std::type_identity<rest>{};
  }
  using forward_next = typename decltype(forward_next_type())::type;
  static constexpr bool consume = round::main_out::flags::template test<G_CONSUME>;

  template<std::size_t N>
  static consteval bool forward_observes() {
    if constexpr (N == 0) return false;
    else if constexpr (observes_within_fn<N>()) return true;
    else if constexpr (has_more && N > consumed)
      return pipe_flags_driver<next_driver, next_state>
          ::template forward_observes<N - consumed>();
    else return false;
  }

  template<class F, class First, class... A>
  static constexpr decltype(auto) forward_tail(F&& f, First&& first, A&&... a) {
    if constexpr (consume)
      return forward_next::for_each_forward(std::forward<F>(f), std::forward<A>(a)...);
    else return forward_next::for_each_forward(
        std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
  }

  template<class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first, A&&... a) {
    constexpr bool future = [] {
      if constexpr (phase == driver_control::phase_done) return false;
      else if constexpr (step::observe_) {
        if constexpr (is_for_each_stop_v<std::invoke_result_t<F&, view, First>>) return false;
        else return forward_next::template forward_observes<sizeof...(A) + !consume>();
      } else return forward_next::template forward_observes<sizeof...(A) + !consume>();
    }();
    if constexpr (step::observe_) {
      using result = std::invoke_result_t<F&, view, First>;
      if constexpr (is_for_each_stop_v<result> || !future) {
        return std::invoke(f, view{}, std::forward<First>(first));
      } else {
        (void)std::invoke(f, view{}, std::forward<First>(first));
        return forward_tail(std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
      }
    } else if constexpr (future) {
      return forward_tail(std::forward<F>(f), std::forward<First>(first), std::forward<A>(a)...);
    }
  }
};

template <class Driver, class Main, class Nodes, class Gen, class Branch>
struct pipe_flags_driver<Driver, pipe_state<Main, Nodes, Gen, Branch>> {
  using chunk = pipe_flags_chunk<Driver, pipe_state<Main, Nodes, Gen, Branch>>;

  template<std::size_t N>
  static consteval bool forward_observes() {
    return chunk::template forward_observes<N>();
  }

  template <class Decision>
  using decide = meta_invoke<Driver,
      driver_control::input<typename chunk::step,
                            driver_control::window_decide, Decision>>;

  static consteval auto result_type() {
    if constexpr (chunk::has_more) {
      return std::type_identity<typename pipe_flags_driver<
          typename chunk::next_driver,
          typename chunk::next_state>::type>{};
    } else {
      return std::type_identity<typename chunk::feed>{};
    }
  }
  using type = typename decltype(result_type())::type;

  struct observes_t {
    static constexpr bool value = [] {
      if constexpr (!chunk::has_more) {
        return chunk::observes;
      } else {
        using next = pipe_flags_driver<typename chunk::next_driver,
                                       typename chunk::next_state>;
        return any_observes<chunk::observes, next>::value;
      }
    }();
  };
  static constexpr bool observes = observes_t::value;

  template <class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (chunk::has_more) {
      using chunk_return = decltype(chunk::for_each(
          std::forward<F>(f), std::forward<A>(a)...));
      if constexpr (is_for_each_stop_v<chunk_return>) {
        return chunk::for_each(std::forward<F>(f),
                               std::forward<A>(a)...);
      } else {
        using next = pipe_flags_driver<typename chunk::next_driver,
                                       typename chunk::next_state>;
        if constexpr (next::observes_t::value) {
          if constexpr (chunk::observes) {
            (void)chunk::for_each(std::forward<F>(f),
                                  std::forward<A>(a)...);
          }
          return next::for_each(std::forward<F>(f),
                                std::forward<A>(a)...);
        } else {
          if constexpr (chunk::observes) {
            return chunk::for_each(std::forward<F>(f),
                                   std::forward<A>(a)...);
          } else {
            return;
          }
        }
      }
    } else {
      if constexpr (chunk::observes) {
        return chunk::for_each(std::forward<F>(f),
                               std::forward<A>(a)...);
      } else {
        return;
      }
    }
  }

  template <class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   A&&... a) {
    return chunk::for_each_forward(std::forward<F>(f),
                                   std::forward<First>(first),
                                   std::forward<A>(a)...);
  }
};


// The initial main already clears G_CONTINUE: no round runs and the initial view is returned directly.
template <class Main, class Nodes, class Gen, class Branch = void>
using pipe_initial_view = pipe_view<
    std::conditional_t<std::is_void_v<Branch>,
                       typename output_node<Nodes>::rec,
                       pipe_fork<typename output_node<Nodes>::rec,
                                 typename branch_rec<Branch>::type>>,
    typename output_prev<Nodes, Gen>::type, typename Main::flags, 0>;

template <bool Start, class Main, class Nodes, class Gen, class DriverPolicy,
          class Branch>
struct pipe_runner_engine {
  using state = pipe_state<Main, Nodes, Gen, Branch>;
  static constexpr std::size_t budget = pipe_state_budget<state>::value;
  static consteval auto engine_type() {
    if constexpr (Start) {
      return std::type_identity<
          pipe_flags_driver<flags_driver_mso<budget, DriverPolicy>, state>>{};
    } else {
      return std::type_identity<pipe_initial_view<Main, Nodes, Gen, Branch>>{};
    }
  }
  using engine = typename decltype(engine_type())::type;
  static consteval auto result_type() {
    if constexpr (Start) {
      return std::type_identity<typename engine::type>{};
    } else {
      return std::type_identity<engine>{};
    }
  }
  using type = typename decltype(result_type())::type;

  template <class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (Start) {
      return engine::for_each(std::forward<F>(f), std::forward<A>(a)...);
    }
  }

  template <class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   A&&... a) {
    if constexpr (Start) {
      return engine::for_each_forward(std::forward<F>(f),
                                      std::forward<First>(first),
                                      std::forward<A>(a)...);
    }
  }
};

// ============================================================================
// 8) runner: set the initial liveness bitmap, expose ::type / for_each
// ============================================================================
template <class Main, class Nodes, class Gen, class DriverPolicy = void,
          class Branch = void>
struct meta_pipe_runner {
  static constexpr bool forked = !std::is_void_v<Branch>;
  static constexpr std::size_t main_nodes = Nodes::size;
  static constexpr std::size_t branch_nodes = branch_size_of<Branch>::value;
  static_assert(main_nodes >= 1, "meta_pipe: run<> needs at least one main-line node");
  static_assert(main_nodes + branch_nodes <= 64,
  "meta_pipe: the liveness bitmap has only 64 bits (main + branch)");

  using policy = std::conditional_t<
      std::is_void_v<DriverPolicy>,
      default_pipe_driver_policy<Main, Nodes, Gen, Branch>, DriverPolicy>;

  static constexpr u64 initial_mask =
      initial_alive_mask<main_nodes> |
      (forked ? mask_shl(initial_alive_mask<branch_nodes>, main_nodes) : u64{0});
  using main_init = typename Main::template meta_set<
      typename Main::flags::template set<u64, pipe_mask_shift, initial_mask>>;
// When the initial main has already closed the master gate, no round runs.
  static constexpr bool start =
      main_init::flags::template test<meta_states::G_CONTINUE>;
  using engine = pipe_runner_engine<start, main_init, Nodes, Gen, policy, Branch>;
  using type = typename engine::type;

  template <template <class> class... Ps, class F, class... Args>
  static constexpr decltype(auto) for_each(F&& f, Args&&... args) {
    return engine::for_each(
        [&f]<class Stage, class... A>(Stage, A&&... a) -> decltype(auto) {
          return std::invoke(f, meta_fold<Stage, Ps...>{},
                             std::forward<A>(a)...);
        },
        std::forward<Args>(args)...);
  }

  template <template <class> class... Ps, class F, class First, class... Args>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   Args&&... args) {
    return engine::for_each_forward(
        [&f]<class Stage, class V>(Stage, V&& v) -> decltype(auto) {
          return std::invoke(f, meta_fold<Stage, Ps...>{},
                             std::forward<V>(v));
        },
        std::forward<First>(first), std::forward<Args>(args)...);
  }
};

// Explicit default-con marker unwrapping: accepts con<C> and a bare con type.
template <class T>
struct unwrap_con {
  using type = T;
};
template <class C>
struct unwrap_con<meta_ios::con<C>> {
  using type = C;
};
template <class T>
struct unwrap_main {
  using type = T;
};
template <class M>
struct unwrap_main<meta_ios::main_con<M>> {
  using type = M;
};

// ============================================================================
// 9) builder: written as gen -> output; internally prepended to the output end (like ranges)
// ============================================================================
// Builds the node list for run: Rec == void means "append no node" (run<> right after fork).
template <class Nodes, class Rec, class Con>
struct builder_run_nodes {
  using type = Nodes;
};
template <class Nodes, class Rec, class Con>
  requires (!std::is_void_v<Rec>)
struct builder_run_nodes<Nodes, Rec, Con> {
  using con = std::conditional_t<
      std::is_void_v<Con>, default_node_con<Nodes::size == 0>,
      typename unwrap_con<Con>::type>;
  using type = typename prepend_node<pipe_node<con, Rec>, Nodes>::type;
};

// After fork, all_to attaches to the branch (the main line is already written).
template <bool Forked, class Nodes, class Branch, class Node>
struct builder_append {
  using nodes = typename prepend_node<Node, Nodes>::type;
  using branch = Branch;
};
template <class Nodes, class Branch, class Node>
struct builder_append<true, Nodes, Branch, Node> {
  using nodes = Nodes;
  using branch = typename prepend_node<Node, Branch>::type;
};

template <class Nodes, class Gen, class Branch = void>
struct meta_pipe_builder {
  static constexpr bool empty = (Nodes::size == 0);
  static constexpr bool forked = !std::is_void_v<Branch>;

  template <class Rec, class Con = default_node_con<Nodes::size == 0>>
  using all_to = meta_pipe_builder<
      typename builder_append<forked, Nodes, Branch,
                              pipe_node<typename unwrap_con<Con>::type,
                                        Rec>>::nodes,
      Gen,
      typename builder_append<forked, Nodes, Branch,
                              pipe_node<typename unwrap_con<Con>::type,
                                        Rec>>::branch>;

// Fork: attach a branch at the current write position, consuming the same source element as the main line.
// all_to written afterwards attaches to the branch; run<> merges both products into pipe_fork<main, branch>.
  template <class Rec, class Con = default_node_con<false>>
  using fork = meta_pipe_builder<
      Nodes, Gen,
      typename prepend_node<pipe_node<typename unwrap_con<Con>::type, Rec>,
                            pipe_nodes<>>::type>;

  template <class Rec = void, class Con = void, class Main = pipe_main_co,
            class DriverPolicy = void>
  using run = meta_pipe_runner<
      typename unwrap_main<Main>::type,
      typename builder_run_nodes<Nodes, Rec, Con>::type, Gen, DriverPolicy,
      Branch>;

  using gen = Gen;
};

}  // namespace meta_stream_details

// ============================================================================
// Public API.
// ============================================================================
namespace meta_ios {

using meta_stream_details::wait_for_end_con;
using meta_stream_details::wait_for_end_advance_con;
using meta_stream_details::node_con;
using meta_stream_details::pipe_stream_co;
using meta_stream_details::pipe_main_co;
using meta_stream_details::pipe_main_fn;
using meta_stream_details::pipe_view;

// A node con's user Fn may return skip_t / yield_t / stop_t.
using meta_states_control::skip_t;
using meta_states_control::yield_t;
using meta_states_control::stop_t;
using meta_states_control::skip;
using meta_states_control::yield;
using meta_states_control::stop;

// Common projections of pipe_view, for for_each<Ps...>(f).
namespace protocols {
template <class Stage>
using stream_to_t = typename Stage::to_t;
template <class Stage>
using stream_from_t = typename Stage::from_t;
template <class Stage>
using stream_cache_t = typename Stage::cache;
}  // namespace protocols

// ----------------------------------------------------------------------------
// meta_pipe<Gen>
//
// Gen is the furthest upstream: any meta object; meta_empty_o (the default) means no upstream.
// all_to is written gen -> output order and internally prepended (ranges style).
// ----------------------------------------------------------------------------
template <class Gen = meta_objects::meta_empty_o>
  requires meta_objects::meta_object_t<Gen>
struct meta_pipe {
  template <class Rec, class Con = meta_stream_details::default_node_con<true>>
  using all_to = meta_stream_details::meta_pipe_builder<
      meta_stream_details::pipe_nodes<
          meta_stream_details::pipe_node<
              typename meta_stream_details::unwrap_con<Con>::type, Rec>>,
      Gen>;

// fork: several main nodes plus several branch nodes consuming the same source element.
// all_to written after fork attaches to the branch.
  template <class Rec,
            class Con = meta_stream_details::default_node_con<false>>
  using fork = meta_stream_details::meta_pipe_builder<
      meta_stream_details::pipe_nodes<>, Gen,
      meta_stream_details::pipe_nodes<meta_stream_details::pipe_node<
          typename meta_stream_details::unwrap_con<Con>::type, Rec>>>;

  template <class Rec = void, class Con = void,
            class Main = meta_stream_details::pipe_main_co,
            class DriverPolicy = void>
  using run = meta_stream_details::meta_pipe_runner<
      typename meta_stream_details::unwrap_main<Main>::type,
      typename meta_stream_details::builder_run_nodes<
          meta_stream_details::pipe_nodes<>, Rec, Con>::type,
      Gen, DriverPolicy, void>;

  using gen = Gen;
};

}  // namespace meta_ios
