#include "meta_stream.hpp"

#include <cstdio>
#include <type_traits>

// Minimal end-to-end for the re-done pipe (meta_stream.hpp, 6.1):
// builder, node step, fold, liveness bitmap, emit/flush semantics.

using namespace meta_ios;
using namespace meta_iostream;
using namespace meta_objects;

using ic0 = std::integral_constant<std::size_t, 0>;
using ic1 = std::integral_constant<std::size_t, 1>;
using ic2 = std::integral_constant<std::size_t, 2>;
using idx3 = index_sequence_istream<3>;
using expect = exp_utilities::exp_list<ic0, ic1, ic2>;

// --- 1) one node: source -> ostream, collect the 3 indices -------------------
using pipe3 = meta_pipe<idx3>::all_to<meta_ostream<>>::run<void>;
static_assert(std::is_same_v<pipe3::type::to_t, expect>,
              "pipe3 should collect 0,1,2");

// --- 2) two nodes: iterator then rolling ostream (reversed) -------------------
using chain = meta_pipe<idx3>::all_to<meta_iterator>::all_to<meta_rostream<>>::run<void>;
using chain_expect = exp_utilities::exp_list<ic2, ic1, ic0>;
static_assert(std::is_same_v<chain::type::to_t, chain_expect>,
              "chained pipe should collect reversed types");

// --- 3) wait_for_end: accumulate everything, flush once on death --------------
using acc = meta_pipe<idx3>::all_to<meta_ostream<>, con<wait_for_end_advance_con<>>>::run<void>;
static_assert(std::is_same_v<acc::type::to_t, expect>,
              "wait_for_end should flush the whole list");

// --- 4) value plane: for_each returns the last observed value ----------------
constexpr int last_idx = pipe3::for_each([](auto s) {
  return static_cast<int>(decltype(s)::index_v);
});
static_assert(last_idx == 2, "last observed index should be 2");

// --- 5) run<Rec> builds a single node; run<Rec> after all_to appends ----------
using run1 = meta_pipe<idx3>::run<meta_ostream<>>;
static_assert(std::is_same_v<run1::type::to_t, expect>, "run<Rec> single node");
using run2 = meta_pipe<idx3>::all_to<meta_iterator>::run<meta_ostream<>>;
static_assert(std::is_same_v<run2::type::to_t, expect>, "all_to + run appends node");

// --- 6) three nodes: iterator -> iterator -> ostream, death propagates --------
using three = meta_pipe<idx3>::all_to<meta_iterator>::all_to<meta_iterator>
                  ::run<meta_ostream<>>;
static_assert(std::is_same_v<three::type::to_t, expect>,
              "three-node chain should collect 0,1,2");

// --- 7) stop_t ends the walk early and is returned ---------------------------
using stop_ret = decltype(pipe3::for_each([](auto s) -> decltype(auto) {
  if constexpr (decltype(s)::index_v == 1) return meta_ios::stop();
  else return static_cast<int>(decltype(s)::index_v);
}));
static_assert(std::is_same_v<stop_ret, meta_core64::control::stop_t>,
              "stop_t should be returned verbatim");

// --- 8) a node that clears EMIT swallows its produce; downstream stays empty --
struct swallow_fn {
  template <meta_core64::u8 Window, meta_core64::u64 L, meta_core64::u64 U, class Obj>
  static constexpr meta_core64::u64 value = [] {
    meta_core64::u64 w = meta_stream64::ops::yield(U);
    return meta_core64::uw::with_user(
        w, meta_core64::uw::user_v(w) & ~meta_stream_details::pipe_emit_bit);
  }();
};
using swallow_pipe = meta_pipe<idx3>::all_to<meta_iterator, con<node_con<swallow_fn, true>>>
                         ::all_to<meta_ostream<>::meta_set<exp_utilities::exp_list<>>>::run<void>;
static_assert(std::is_same_v<swallow_pipe::type::to_t, exp_utilities::exp_list<>>,
              "swallowed values must not reach downstream");

int main() {
  int calls = 0;
  pipe3::for_each([&](auto) { ++calls; });
  int stop_calls = 0;
  pipe3::for_each([&](auto s) {
    if constexpr (decltype(s)::index_v == 1) return meta_ios::stop();
    else { ++stop_calls; return; }
  });
  const bool ok = calls == 3 && stop_calls == 1;
  std::printf("pipe64_test: %s (calls=%d stop_calls=%d)\n",
              ok ? "PASS" : "FAIL", calls, stop_calls);
  return ok ? 0 : 1;
}
