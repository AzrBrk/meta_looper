#pragma once

// ============================================================================
// meta_stream: a compile-time pipeline (meta_pipe) on the two-word model.
//
// Each stage is a node (con, rec): the con is a condition Fn
//   template <u8 Window, u64 L, u64 U, class Obj> -> u64
// (the user Fn wrapped in meta_stream64::stream_cond64), the rec is a meta
// object receiver. One round folds from the generator toward the output: each
// node runs meta_loop64::step over its own (L, U) state, and the upstream
// produce becomes the next node's generator.
//
// Pipe-reserved USER bits (in the user word):
//   USER[46] = PIPE_EMIT   whether a value received this round is passed downstream
//   USER[47] = PIPE_FLUSH  flush the final product on the round the node dies
//
// Liveness: the pipe state carries a THIRD u64 (the liveness bitmap), bit k = node
// k alive, bit 0 = input-most node. A node's death clears its bit; the main
// condition stops the pipe once the bitmap is empty. 64 nodes stay supported
// because the bitmap never travels through the 48-bit USER field.
// ============================================================================

#include <functional>
#include <typeinfo>

#include "meta_stream64.hpp"
#include "meta_loop64.hpp"
#include "meta_iostream.hpp"

namespace meta_ios {
template <class C>
struct con {};
template <class M>
struct main_con {};
}  // namespace meta_ios

namespace meta_stream_details {
using namespace exp_utilities;
using namespace literal_types;
using namespace meta_objects;
using namespace meta_invoke_protocols;
using namespace meta_core64;

// ============================================================================
// 1) Pipe USER bits + seed words
// ============================================================================
inline constexpr u64 pipe_emit_bit  = u64{1} << 46;
inline constexpr u64 pipe_flush_bit = u64{1} << 47;

// A freshly built node starts at POS 0 with a NOP mail; the gates are rebuilt by
// the con every window, so the seed gates byte is irrelevant.
inline constexpr u64 node_seed_L = lw::make(0, 0, 0, 0, 0);

// The seed USER bits: EMIT for a normal node, FLUSH for wait-for-end. The con's
// `yield` keeps USER untouched, so these survive the whole round and the engine
// reads them from the final user word.
constexpr u64 node_seed_U(bool flush) noexcept {
  const u64 bits = select64(flush, pipe_flush_bit, pipe_emit_bit);
  return uw::make(isa::encode(isa::f_nop, 0), bits);
}

// ============================================================================
// 2) Node condition
// ============================================================================
// node_con_t is the raw window Fn; the public aliases wrap it in a meta states
// object whose `flags` is the seed (zero here: the stream rules latch their own
// bits) and whose `function` is this Fn. This keeps `meta_states_looper`'s entry
// taking a meta_states_object_t, the same shape as every other condition.
template <class Fn, bool Advance, bool Flush>
struct node_con_t {
  static constexpr bool flush = Flush;
  template <u8 Window, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    u64 w = meta_stream64::stream_cond64<Fn>::template value<Window, L, U, Obj>;
    if constexpr (!Advance) {
      w = meta_stream64::ops::with_gen(w, false);
    }
    // stream_cond64's window-2 init-failure rule is already gated on
    // `no_generator`: with a live upstream produce a receive-to-init receiver
    // (meta_aligned_iterator / meta_wire_iterator) initializes by receiving, so
    // no undo is needed here anymore.
    return w;
  }();
};

template <class Con>
using node_con_mso = meta_objects::meta_states_object<u64{0}, Con>;

template <bool Advance>
using default_node_con =
    node_con_mso<node_con_t<meta_stream64::unchanged, Advance, false>>;
template <class Fn = meta_stream64::unchanged, bool Advance = false>
using node_con = node_con_mso<node_con_t<Fn, Advance, false>>;
template <class Fn = meta_stream64::unchanged, bool Advance = false>
using wait_for_end_con = node_con_mso<node_con_t<Fn, Advance, true>>;
// Node 0: wait-for-end while also advancing the source generator.
template <class Fn = meta_stream64::unchanged>
using wait_for_end_advance_con = node_con_mso<node_con_t<Fn, true, true>>;

// ============================================================================
// 3) Nodes and node lists (output -> input; head is the output node)
// ============================================================================
template <class Con, u64 L, u64 U, class Rec>
struct pipe_node {
  using con = Con;
  static constexpr u64 looper = L;
  static constexpr u64 user = U;
  using rec = Rec;
};

template <class... Nodes>
struct pipe_nodes {
  static constexpr std::size_t size = sizeof...(Nodes);
};

template <class Node, class List>
struct prepend_impl;
template <class Node, class... Nodes>
struct prepend_impl<Node, pipe_nodes<Nodes...>> {
  using type = pipe_nodes<Node, Nodes...>;
};
template <class Node, class List>
using prepend = typename prepend_impl<Node, List>::type;

template <class Nodes>
struct output_rec;
template <class Head, class... Rest>
struct output_rec<pipe_nodes<Head, Rest...>> {
  using type = typename Head::rec;
};

// The produce the output node would consume before any round runs: the second
// node's receiver (the node feeding the output), or the source for a single
// node. Carried in pipe_state::prev so the final view exposes it.
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

// ============================================================================
// 3b) Fork: one branch and the main line share the same source element frame.
// ============================================================================
template <std::size_t N>
inline constexpr u64 low_mask = N >= 64 ? ~u64{0} : ((u64{1} << N) - 1);

// The two end receivers merged into one product; pipe_view exposes .main() /
// .branch() through this.
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

template <class T>
struct branch_size_of { static constexpr std::size_t value = 0; };
template <class... Nodes>
struct branch_size_of<pipe_nodes<Nodes...>> {
  static constexpr std::size_t value = sizeof...(Nodes);
};

// Branch output receiver, or void when there is no branch.
template <class T>
struct branch_rec { using type = void; };
template <class Head, class... Rest>
struct branch_rec<pipe_nodes<Head, Rest...>> {
  using type = typename output_rec<pipe_nodes<Head, Rest...>>::type;
};

// ============================================================================
// 4) One node step (meta_loop64::step over the node's own two words)
// ============================================================================

// The con actually run for one round: the node's con, plus the upstream signals.
//   UpstreamDied: upstream died this round -> the node stops (cont/rec/gen
//     cleared, OBS kept so a flush round is still visible to the callback).
//   !HasValue: upstream is alive but produced nothing this round (a filter
//     swallowed) -> skip receiving AND observing while staying alive, so the
//     swallowed value neither reaches the receiver nor fires a callback.
template <class Con, bool UpstreamDied, bool HasValue>
struct node_step_cond {
  template <u8 Window, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    u64 w = Con::function::template value<Window, L, U, Obj>;
    // A swallowed value (filter, or upstream died without a flush) never
    // reaches the receiver nor fires a callback.
    if constexpr (!HasValue && Window == 2) {
      w = meta_stream64::ops::with_rec(w, false);
      w = meta_stream64::ops::with_obs(w, false);
    }
    // Upstream died: die at the round tail, AFTER the window-2 receive, so a
    // flushed value is still received before this node stops.
    if constexpr (UpstreamDied && Window == 3) {
      w = meta_stream64::ops::stop(w);
    }
    return w;
  }();
};

template <class Con, u64 L, u64 U, class Rec, class Gen, bool UpstreamDied,
          bool HasValue>
struct pipe_node_step {
  using st = meta_loop64::step<
      meta_loop64::state<L, U, Rec, Gen, node_step_cond<Con, UpstreamDied,
                                                        HasValue>>>;

  // Death depends solely on the con clearing CONT.
  static constexpr bool just_died = !st::cont;

  // The emit/flush bits the con left in the final user word.
  static constexpr u64 out_U = st::next::U;
  static constexpr bool emit_flag  = (uw::user_v(out_U) & pipe_emit_bit) != 0;
  static constexpr bool flush_flag = (uw::user_v(out_U) & pipe_flush_bit) != 0;

  static constexpr bool has_produce = exp_utilities::has_type<typename st::next::rec>;

  // Emit downstream this round: a live round that received (and left EMIT armed),
  // or a dying round that left FLUSH armed (wait-for-end). `HasValue && cont` is
  // the "received" signal: for the standard cons REC is dropped only together
  // with CONT (stream stop_mask) or by the !HasValue branch above.
  static constexpr bool emitted =
      has_produce &&
      ((HasValue && st::cont && emit_flag) || (just_died && flush_flag));

  using node = pipe_node<Con, st::next::L, st::next::U, typename st::next::rec>;
  using rec_out = typename st::next::rec;
  using gen_out = typename st::next::gen;
  // The con's own OBS (pre_rec) AND its own CONT (post_rec, before the engine's
  // UpstreamDied -> stop). A dying flush round observes; a user stop does not.
  static constexpr bool observed = st::obs && (st::cont || UpstreamDied);
};

// ============================================================================
// 5) The fold: input-most -> output, threading the upstream produce.
// ============================================================================

// Only the input-most node owns the source generator; its advanced form becomes
// the pipe's generator for the next round. Middle nodes keep the threaded form.
template <bool IsInputMost, class StepGen, class ThreadedGen>
struct fold_gen_out {
  using type = ThreadedGen;
};
template <class StepGen, class ThreadedGen>
struct fold_gen_out<true, StepGen, ThreadedGen> {
  using type = StepGen;
};

// `prev` for the view: the produce the output node consumed this round. It is
// threaded from the input side and pinned at the output (IsOutput).
template <bool IsOutput, class InnerOut, class InnerPrev>
struct fold_prev {
  using type = InnerPrev;
};
template <class InnerOut, class InnerPrev>
struct fold_prev<true, InnerOut, InnerPrev> {
  using type = InnerOut;
};

template <bool Alive, std::size_t Index, u64 Mask, class GenOut, class InValue,
          bool HasValue, bool UpstreamDied, class Node, bool IsInputMost>
struct pipe_fold_node;

// A dead node is not called, stays verbatim and emits nothing.
template <std::size_t Index, u64 Mask, class GenOut, class InValue,
          bool HasValue, bool UpstreamDied, class Node, bool IsInputMost>
struct pipe_fold_node<false, Index, Mask, GenOut, InValue, HasValue,
                      UpstreamDied, Node, IsInputMost> {
  using node = Node;
  using out = typename Node::rec;
  using gen_out = GenOut;
  static constexpr bool has_value = false;
  static constexpr bool observed = false;
  static constexpr bool just_died = false;
  static constexpr u64 mask = Mask;
};

// A live node takes one step.
template <std::size_t Index, u64 Mask, class GenOut, class InValue,
          bool HasValue, bool UpstreamDied, class Node, bool IsInputMost>
struct pipe_fold_node<true, Index, Mask, GenOut, InValue, HasValue,
                      UpstreamDied, Node, IsInputMost> {
  using step = pipe_node_step<typename Node::con, Node::looper, Node::user,
                              typename Node::rec, InValue, UpstreamDied,
                              HasValue>;
  using node = typename step::node;
  using out = typename step::rec_out;
  using gen_out = typename fold_gen_out<IsInputMost, typename step::gen_out,
                                        GenOut>::type;
  static constexpr bool has_value = step::emitted;
  static constexpr bool observed = step::observed;
  static constexpr bool just_died = step::just_died;
  // just_died is 0/1; shifting by Index yields exactly the bit to clear.
  static constexpr u64 mask = Mask & ~(u64{just_died} << Index);
};

template <class Nodes, std::size_t Index, bool IsOutput, u64 Mask,
          class InValue, bool InHasValue, bool InDied>
struct pipe_fold;

template <std::size_t Index, bool IsOutput, u64 Mask, class InValue,
          bool InHasValue, bool InDied>
struct pipe_fold<pipe_nodes<>, Index, IsOutput, Mask, InValue, InHasValue,
                 InDied> {
  using nodes = pipe_nodes<>;
  using out = InValue;
  using prev = InValue;
  using gen_out = InValue;
  static constexpr bool has_value = InHasValue;
  static constexpr bool observed = false;
  static constexpr u64 mask = Mask;
};

template <class Head, class... Rest, std::size_t Index, bool IsOutput, u64 Mask,
          class InValue, bool InHasValue, bool InDied>
struct pipe_fold<pipe_nodes<Head, Rest...>, Index, IsOutput, Mask, InValue,
                 InHasValue, InDied> {
  // Fold the input side first, then this head (closer to the output).
  using inner = pipe_fold<pipe_nodes<Rest...>, Index - 1, false, Mask, InValue,
                          InHasValue, InDied>;

  static constexpr bool alive = (Mask >> Index) & 1;
  static constexpr bool is_input_most = (sizeof...(Rest) == 0);

  // Upstream died: the input-most node reads InDied (false on the main line; its
  // con detects source eol itself); a middle node reads its upstream bitmap bit.
  static constexpr bool upstream_dead =
      is_input_most ? InDied : ((inner::mask >> (Index - 1)) & 1) == 0;

  using node_result =
      pipe_fold_node<alive, Index, inner::mask, typename inner::gen_out,
                     typename inner::out, inner::has_value, upstream_dead, Head,
                     is_input_most>;

  using nodes = prepend<typename node_result::node, typename inner::nodes>;
  using out = typename node_result::out;
  using prev = typename fold_prev<IsOutput, typename inner::out,
                                  typename inner::prev>::type;
  using gen_out = typename node_result::gen_out;

  static constexpr bool has_value = node_result::has_value;
  static constexpr bool observed = node_result::observed;
  static constexpr u64 mask = node_result::mask;
};

// ============================================================================
// 6) Pipe state, one round, and the view
// ============================================================================
template <class Main, class Nodes, class Gen, class Branch, u64 Bitmap, u32 Index,
          class Prev>
struct pipe_state {
  using main = Main;
  using nodes = Nodes;
  using gen = Gen;
  using branch = Branch;
  // The produce the output node consumed last round (threaded from the input
  // side via fold_prev), not the source generator.
  using prev = Prev;
  static constexpr u64 bitmap = Bitmap;
  static constexpr u32 index = Index;
};

// The default main: stop once every node is dead (bitmap == 0).
struct pipe_main_fn {
  template <u64 Bitmap, class Out>
  static constexpr bool cont = (Bitmap != 0);
};

// Forked product: pipe_fork<Main, Branch>, otherwise the main product alone.
template <bool Forked, class MainOut, class BranchOut>
struct fork_out { using type = MainOut; };
template <class MainOut, class BranchOut>
struct fork_out<true, MainOut, BranchOut> {
  using type = pipe_fork<MainOut, BranchOut>;
};

// Branch produce: meta_produce_t when forked, void otherwise (never instantiate
// the void branch product).
template <bool Forked, class Branch>
struct branch_produce { using type = void; };
template <class Branch>
struct branch_produce<true, Branch> { using type = meta_produce_t<Branch>; };

// The per-round view a for_each callback receives.
template <class To, class From, u32 Index, std::size_t BranchNodes = 0>
struct pipe_view {
  using to = To;
  using from = From;
  using to_t = get_type<to>;
  using from_t = get_type<from>;
  using cache = meta_produce_t<from>;
  static constexpr u32 index_v = Index;
  static constexpr std::size_t branch_nodes = BranchNodes;

  using parts = fork_parts<to>;
  static constexpr bool forked = parts::forked;
  using main_t = meta_produce_t<typename parts::main_t>;
  using branch_t = typename branch_produce<forked, typename parts::branch_t>::type;

  consteval u32 index() const noexcept { return Index; }
  consteval auto value() const { return value_of<to_t>; }
  consteval std::size_t left() const { return exp_size<from_t>; }
  constexpr auto object() const { return to_t{}; }
  constexpr auto main() const { return main_t{}; }
  constexpr auto branch() const requires forked { return branch_t{}; }
  consteval const std::type_info& target_type() const { return typeid(to_t); }
};

// A branch round: run the whole fold over the branch, feeding it the element the
// main line consumed this round. SourceDied = the main source node died.
template <bool Forked, class Branch, class InValue, u64 Mask, bool SourceDied>
struct pipe_branch_fold {
  using nodes = void;
  using out = void;
  static constexpr u64 mask = 0;
};
template <class Branch, class InValue, u64 Mask, bool SourceDied>
struct pipe_branch_fold<true, Branch, InValue, Mask, SourceDied> {
  using folded = pipe_fold<Branch, Branch::size - 1, true, Mask, InValue, true,
                           SourceDied>;
  using nodes = typename folded::nodes;
  using out = typename folded::out;
  static constexpr u64 mask = folded::mask;
};

template <class State>
struct pipe_round {
  using main = typename State::main;
  using nodes = typename State::nodes;
  using gen = typename State::gen;
  using branch = typename State::branch;
  static constexpr u64 bitmap = State::bitmap;
  static constexpr u32 index = State::index;

  static constexpr bool forked = !std::is_void_v<branch>;
  static constexpr std::size_t main_nodes = nodes::size;
  static constexpr std::size_t branch_nodes = branch_size_of<branch>::value;

  // The liveness bitmap has two segments: main bits 0..M-1, branch M..M+B-1.
  static constexpr u64 mask_main = bitmap & low_mask<main_nodes>;
  static constexpr u64 mask_branch =
      forked ? ((bitmap >> main_nodes) & low_mask<branch_nodes>) : u64{0};

  using folded = pipe_fold<nodes, nodes::size - 1, true, mask_main, gen, true,
                           false>;
  using branch_fold =
      pipe_branch_fold<forked, branch, typename folded::gen_out, mask_branch,
                       (folded::mask & 1) == 0>;

  using new_nodes = typename folded::nodes;
  using new_branch = typename branch_fold::nodes;
  using out = typename fork_out<forked, typename folded::out,
                                typename branch_fold::out>::type;
  using prev = typename folded::prev;
  using gen_out = typename folded::gen_out;

  static constexpr u64 new_bitmap =
      folded::mask | (branch_fold::mask << main_nodes);

  // main reads the main-line bitmap only; branch liveness never keeps the pipe
  // alive by itself.
  static constexpr bool main_cont = main::template cont<folded::mask, out>;

  // Observe once per round, gated by the output node's own OBS/CONT (the con's
  // result, not the engine's UpstreamDied translation). main_cont is the loop
  // continuation, a separate decision, not a callback gate.
  static constexpr bool observe_ = folded::observed;

  using next =
      pipe_state<main, new_nodes, gen_out, new_branch, new_bitmap, index + 1,
                 prev>;
  using view = pipe_view<out, prev, index, branch_nodes>;
};

// ============================================================================
// 7) Chunked driver: bound the per-round template recursion.
// ============================================================================
// The pipe is data-driven (runs until the liveness bitmap is empty), so a deep
// pipe recurses one template level per round. A chunk budget bounds that: one
// chunk runs up to `pipe_budget` rounds and stops on a resumable state; the
// outer driver opens the next chunk. The per-round walk is lazy (consteval
// functions over the `rest` alias), so the template depth is the chunk count,
// not the round count.
#ifndef META_PIPE_BUDGET
#define META_PIPE_BUDGET 64
#endif
inline constexpr std::size_t pipe_budget = META_PIPE_BUDGET;

// Final view of a terminated pipe.
template <class State>
struct pipe_final_view {
  using main_out = typename output_rec<typename State::nodes>::type;
  using branch_out = typename branch_rec<typename State::branch>::type;
  using final_out =
      typename fork_out<!std::is_void_v<typename State::branch>, main_out,
                        branch_out>::type;
  using view =
      pipe_view<final_out, typename State::prev, State::index,
                branch_size_of<typename State::branch>::value>;
};

template <class State, bool Cont>
struct pipe_driver;

// One chunk: bounded per-round walk. `Remaining` counts down; the recursion is
// lazy (consteval functions over the `rest` alias), so the template depth stays
// bounded by the budget, not the round count.
template <std::size_t Remaining, class State>
struct pipe_chunk {
  using round = pipe_round<State>;
  using next_state = typename round::next;
  using rest = pipe_chunk<Remaining - 1, next_state>;

  static constexpr bool ended = !round::main_cont;   // loop ended this round
  static constexpr bool spent = (Remaining == 1);    // last round of this chunk

  // Does the loop continue past this chunk?
  static consteval bool has_more_fn() {
    if constexpr (spent) return round::main_cont;
    else if constexpr (ended) return false;
    else return rest::has_more_fn();
  }

  // The resumable state (the state after this chunk's rounds).
  static consteval auto next_state_fn() {
    if constexpr (spent || ended)
      return std::type_identity<next_state>{};
    else
      return rest::next_state_fn();
  }

  // The final view if the loop ends within this chunk.
  static consteval auto feed_fn() {
    if constexpr (spent || ended)
      return std::type_identity<typename pipe_final_view<next_state>::view>{};
    else
      return rest::feed_fn();
  }

  // "Does any round observe from here to the end of this chunk?" — precomputed
  // as a bool (lazily via rest::observes), so for_each reads it O(1) instead of
  // recursing per call. `if constexpr` keeps the search from instantiating the
  // recursive branch once an observed round is found.
  static constexpr bool observes = [] {
    if constexpr (round::observe_) return true;
    else if constexpr (ended || spent) return false;
    else return rest::observes;
  }();

  // Does any of the next N rounds observe? (N arguments remain to consume)
  template <std::size_t N, bool Across = false>
  static consteval bool observes_within_fn() {
    if constexpr (N == 0) return false;
    else if constexpr (round::observe_) return true;
    else if constexpr (ended) return false;
    else if constexpr (spent)
      return Across &&
             pipe_driver<next_state, round::main_cont>::
                 template observes_within_fn<N>();
    else if constexpr (N == 1) return false;
    else return rest::template observes_within_fn<N - 1, Across>();
  }

  // for_each (value plane).
  template <class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (spent || ended) {
      if constexpr (round::observe_)
        return std::invoke(f, typename round::view{}, std::forward<A>(a)...);
      else return;
    } else if constexpr (round::observe_) {
      using view = typename round::view;
      using R = std::invoke_result_t<F&, view, A...>;
      using D = std::remove_cvref_t<R>;
      if constexpr (std::is_same_v<D, meta_core64::control::stop_t>) {
        return std::invoke(f, view{}, std::forward<A>(a)...);
      } else if constexpr (std::is_same_v<D, meta_core64::control::skip_t> ||
                           std::is_same_v<D, meta_core64::control::yield_t>) {
        (void)std::invoke(f, view{}, std::forward<A>(a)...);
        return rest::for_each(std::forward<F>(f), std::forward<A>(a)...);
      } else {
        if constexpr (!rest::observes) {
          return std::invoke(f, view{}, std::forward<A>(a)...);
        } else {
          (void)std::invoke(f, view{}, std::forward<A>(a)...);
          return rest::for_each(std::forward<F>(f), std::forward<A>(a)...);
        }
      }
    } else {
      return rest::for_each(std::forward<F>(f), std::forward<A>(a)...);
    }
  }

  // forward_tail: consume `first`, recurse into the rest or the next chunk.
  template <class F, class First, class... Rest>
  static constexpr decltype(auto) forward_tail(F&& f, First&&, Rest&&... rest) {
    if constexpr (spent)
      return pipe_driver<next_state, round::main_cont>::for_each_forward(
          std::forward<F>(f), std::forward<Rest>(rest)...);
    else
      return rest::for_each_forward(std::forward<F>(f),
                                    std::forward<Rest>(rest)...);
  }

  // for_each_forward (consuming).
  template <class F, class First, class... Rest>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   Rest&&... rest) {
    constexpr bool no_more = sizeof...(Rest) == 0;
    if constexpr (no_more) {
      if constexpr (round::observe_) {
        using view = typename round::view;
        using R = std::invoke_result_t<F&, view, First>;
        using D = std::remove_cvref_t<R>;
        if constexpr (std::is_same_v<D, meta_core64::control::stop_t> ||
                      std::is_same_v<D, meta_core64::control::skip_t> ||
                      std::is_same_v<D, meta_core64::control::yield_t>) {
          (void)std::invoke(f, view{}, std::forward<First>(first));
          return;
        } else {
          return std::invoke(f, view{}, std::forward<First>(first));
        }
      } else {
        return;
      }
    } else if constexpr (round::observe_) {
      using view = typename round::view;
      using R = std::invoke_result_t<F&, view, First>;
      using D = std::remove_cvref_t<R>;
      if constexpr (std::is_same_v<D, meta_core64::control::stop_t>) {
        return std::invoke(f, view{}, std::forward<First>(first));
      } else if constexpr (std::is_same_v<D, meta_core64::control::skip_t> ||
                           std::is_same_v<D, meta_core64::control::yield_t>) {
        (void)std::invoke(f, view{}, std::forward<First>(first));
        return forward_tail(std::forward<F>(f), std::forward<First>(first),
                            std::forward<Rest>(rest)...);
      } else {
        constexpr bool future = [] {
          if constexpr (spent)
            return pipe_driver<next_state, round::main_cont>::
                template observes_within_fn<sizeof...(Rest)>();
          else
            return rest::template observes_within_fn<sizeof...(Rest)>();
        }();
        if constexpr (!future) {
          return std::invoke(f, view{}, std::forward<First>(first));
        } else {
          (void)std::invoke(f, view{}, std::forward<First>(first));
          return forward_tail(std::forward<F>(f), std::forward<First>(first),
                              std::forward<Rest>(rest)...);
        }
      }
    } else {
      return forward_tail(std::forward<F>(f), std::forward<First>(first),
                          std::forward<Rest>(rest)...);
    }
  }
};

// The outer driver: opens chunks until the loop ends.
template <class State>
struct pipe_driver<State, false> {
  using type = typename pipe_final_view<State>::view;
  template <class F, class... A>
  static constexpr void for_each(F&&, A&&...) noexcept {}
  template <class F, class First, class... A>
  static constexpr void for_each_forward(F&&, First&&, A&&...) noexcept {}
  static constexpr bool observes = false;
  template <std::size_t N>
  static consteval bool observes_within_fn() { return false; }
};

template <class State>
struct pipe_driver<State, true> {
  using chunk = pipe_chunk<pipe_budget, State>;

  static consteval bool has_more_fn() { return chunk::has_more_fn(); }
  static consteval auto resume_state_fn() { return chunk::next_state_fn(); }
  using resume_state = typename decltype(resume_state_fn())::type;
  using next = pipe_driver<resume_state, has_more_fn()>;

  using type = typename decltype([] {
    if constexpr (has_more_fn())
      return std::type_identity<typename next::type>{};
    else
      return chunk::feed_fn();
  }())::type;

  // Cross-chunk: does any round observe from here to the end?
  static constexpr bool observes = chunk::observes || next::observes;
  template <std::size_t N>
  static consteval bool observes_within_fn() {
    return chunk::template observes_within_fn<N, true>();
  }

  template <class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    if constexpr (has_more_fn()) {
      using r = decltype(chunk::for_each(std::forward<F>(f), std::forward<A>(a)...));
      if constexpr (std::is_same_v<std::remove_cvref_t<r>,
                                   meta_core64::control::stop_t>) {
        return chunk::for_each(std::forward<F>(f), std::forward<A>(a)...);
      } else if constexpr (next::observes) {
        (void)chunk::for_each(std::forward<F>(f), std::forward<A>(a)...);
        return next::for_each(std::forward<F>(f), std::forward<A>(a)...);
      } else {
        return chunk::for_each(std::forward<F>(f), std::forward<A>(a)...);
      }
    } else {
      return chunk::for_each(std::forward<F>(f), std::forward<A>(a)...);
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

// ============================================================================
// 8) Public runner: initial bitmap + for_each / type
// ============================================================================
template <std::size_t Count>
inline constexpr u64 initial_alive_mask =
    Count >= 64 ? ~u64{0} : ((u64{1} << Count) - 1);

template <class Main, class Nodes, class Gen, class Branch = void>
struct meta_pipe_runner {
  static_assert(Nodes::size >= 1, "meta_pipe: run<> needs at least one node");
  static constexpr std::size_t main_nodes = Nodes::size;
  static constexpr std::size_t branch_nodes = branch_size_of<Branch>::value;
  static_assert(main_nodes + branch_nodes <= 64,
                "meta_pipe: the liveness bitmap has only 64 bits "
                "(main + branch)");

  static constexpr u64 initial_mask =
      initial_alive_mask<main_nodes> |
      (branch_size_of<Branch>::value
           ? (initial_alive_mask<branch_nodes> << main_nodes)
           : u64{0});
  using state = pipe_state<Main, Nodes, Gen, Branch, initial_mask, 0,
                           typename output_prev<Nodes, Gen>::type>;
  // When the main closes the master gate up front, no round runs at all (the
  // receivers stay uninitialized), matching the old pipe's `start` flag.
  static constexpr bool start = Main::template cont<initial_mask, void>;
  using engine = pipe_driver<state, start>;
  using type = typename engine::type;

  template <class F, class... A>
  static constexpr decltype(auto) for_each(F&& f, A&&... a) {
    return engine::for_each(std::forward<F>(f), std::forward<A>(a)...);
  }

  template <class F, class First, class... A>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   A&&... a) {
    return engine::for_each_forward(std::forward<F>(f),
                                    std::forward<First>(first),
                                    std::forward<A>(a)...);
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

// Build a node from a con descriptor + receiver, seeding the words.
template <class Con, class Rec>
struct make_pipe_node {
  using node = pipe_node<Con, node_seed_L, node_seed_U(Con::function::flush), Rec>;
};

// run<Rec, Con>: append the final node (Rec == void appends nothing).
struct unwrap_con_fn {
  template <class T>
  using apply = typename unwrap_con<T>::type;
};

template <class Nodes, class Rec, class Con>
struct builder_run_nodes {
  using type = Nodes;
};
template <class Nodes, class Rec, class Con>
  requires(!std::is_void_v<Rec>)
struct builder_run_nodes<Nodes, Rec, Con> {
  using con = select_if_t<
      std::is_void_v<Con>,
      always_fn<default_node_con<Nodes::size == 0>>,
      unwrap_con_fn,
      Con>;
  using type = prepend<typename make_pipe_node<con, Rec>::node, Nodes>;
};

// After fork, all_to attaches to the branch (the main line is already written).
template <bool Forked, class Nodes, class Branch, class Node>
struct builder_append {
  using nodes = prepend<Node, Nodes>;
  using branch = Branch;
};
template <class Nodes, class Branch, class Node>
struct builder_append<true, Nodes, Branch, Node> {
  using nodes = Nodes;
  using branch = prepend<Node, Branch>;
};

template <class Nodes, class Gen, class Branch = void>
struct meta_pipe_builder {
  static constexpr bool forked = !std::is_void_v<Branch>;

  // all_to<Rec, Con>: prepend a node (ranges style: gen -> output order).
  // After fork it attaches to the branch; the con's `Advance` keeps using the
  // main-line size (branch nodes never advance the source).
  template <class Rec, class Con = default_node_con<Nodes::size == 0>>
  using all_to = meta_pipe_builder<
      typename builder_append<
          forked, Nodes, Branch,
          typename make_pipe_node<typename unwrap_con<Con>::type, Rec>::node>::nodes,
      Gen,
      typename builder_append<
          forked, Nodes, Branch,
          typename make_pipe_node<typename unwrap_con<Con>::type, Rec>::node>::branch>;

  // fork<Rec, Con>: attach a branch, sharing the source element with the main line.
  template <class Rec, class Con = default_node_con<false>>
  using fork = meta_pipe_builder<
      Nodes, Gen,
      pipe_nodes<typename make_pipe_node<typename unwrap_con<Con>::type,
                                         Rec>::node>>;

  template <class Rec = void, class Con = void, class Main = pipe_main_fn>
  using run = meta_pipe_runner<
      typename unwrap_main<Main>::type,
      typename builder_run_nodes<Nodes, Rec, Con>::type, Gen, Branch>;

  using gen = Gen;
};

}  // namespace meta_stream_details

namespace meta_ios {
using meta_stream_details::pipe_node;
using meta_stream_details::pipe_nodes;
using meta_stream_details::node_con;
using meta_stream_details::wait_for_end_con;
using meta_stream_details::wait_for_end_advance_con;
using meta_stream_details::default_node_con;
using meta_stream_details::pipe_main_fn;
using meta_stream_details::pipe_view;
using meta_stream_details::pipe_emit_bit;
using meta_stream_details::pipe_flush_bit;

// A node con's user Fn may return skip_t / yield_t / stop_t.
using meta_core64::control::skip_t;
using meta_core64::control::yield_t;
using meta_core64::control::stop_t;
using meta_core64::control::skip;
using meta_core64::control::yield;
using meta_core64::control::stop;

template <class Gen = meta_objects::meta_empty_o>
  requires meta_objects::meta_object_t<Gen>
struct meta_pipe {
  template <class Rec, class Con = meta_stream_details::default_node_con<true>>
  using all_to = meta_stream_details::meta_pipe_builder<
      meta_stream_details::pipe_nodes<
          typename meta_stream_details::make_pipe_node<
              typename meta_stream_details::unwrap_con<Con>::type, Rec>::node>,
      Gen>;

  // fork<Rec, Con>: several main nodes plus several branch nodes sharing the
  // same source element; all_to written after fork attaches to the branch.
  template <class Rec, class Con = meta_stream_details::default_node_con<false>>
  using fork = meta_stream_details::meta_pipe_builder<
      meta_stream_details::pipe_nodes<>, Gen,
      meta_stream_details::pipe_nodes<
          typename meta_stream_details::make_pipe_node<
              typename meta_stream_details::unwrap_con<Con>::type, Rec>::node>>;

  template <class Rec = void, class Con = void,
            class Main = meta_stream_details::pipe_main_fn>
  using run = meta_stream_details::meta_pipe_runner<
      typename meta_stream_details::unwrap_main<Main>::type,
      typename meta_stream_details::builder_run_nodes<
          meta_stream_details::pipe_nodes<>, Rec, Con>::type,
      Gen>;

  using gen = Gen;
};

}  // namespace meta_ios
