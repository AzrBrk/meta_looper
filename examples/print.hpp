#pragma once

#include "meta_ios.hpp"

#include <bit>
#include <utility>

namespace print_detail {
using namespace meta_states;
using namespace meta_objects;
using namespace exp_utilities;
using namespace meta_core64;

// ============================================================================
// print: format a static string with '#' placeholders, one char at a time.
//
// The receiver is the plain meta_ostream<> (a parameter-initialized object):
// each receive appends one char; G_INITIALIZE (the init gate) force-invokes its
// initialize, resetting it to the "waiting for first input" proxy. The condition
// carries three bits of user-Fn state in USER (bits 40..42, OUTSIDE the stream's
// own 8-bit block at 32..39, which stream_states_co owns and the user Fn cannot
// write):
//   ready     (bit 40): the generator just produced '#' or '\0', the fragment
//             must be delivered.
//   delivered (bit 41): the fragment was delivered; the receiver must be reset.
//   hold      (bit 42): one-round latch — "the generator was NOT advanced this
//             round", written in pre_gen, read in pre_rec.
//
// Round flow for "hello #":
//   - chars accumulate into the receiver; the round that produces '#' still
//     receives it (fragment "hello #"), OBS stays off.
//   - the next round holds the generator (ready -> hold) and delivers: OBS |
//     CONSUME on, delivered set (ready kept), REC suppressed one round via
//     GATES_ARM. The callback pops the trailing '#' and prints "hello " + arg.
//   - the round after (delivered) resets the receiver with G_INITIALIZE.
// The trailing '\0' drives the final (empty) fragment, carried by the extra ""
// argument. Placeholder count must equal argument count.
//
// Idempotence (the two-word engine's communicate() calls the condition until it
// re-issues the same gates, Fuel=2): every window flips state only ONCE and the
// flipped state no longer re-enters a different branch of the SAME window:
//   - pre_gen  clears ready when delivered (deliver is over) — clearing is
//     idempotent and hold stays 1 because delivered is still set.
//   - pre_rec  delivers when ready&&hold (sets delivered, keeps ready, so the
//     next communicate() round still hits the deliver branch) and resets when
//     delivered&&!ready (keeps delivered, so it stays in the reset branch).
//   - post_rec clears delivered only when !ready&&delivered (reset is over),
//     so the deliver round keeps delivered for the reset round to see.
// ============================================================================

inline constexpr u64 ready_bit = u64{1} << 40;
inline constexpr u64 delivered_bit = u64{1} << 41;
inline constexpr u64 hold_bit = u64{1} << 42;

struct condition {
  template <u8 Window, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    const u64 user = uw::user_v(U);
    const bool ready = (user & ready_bit) != 0;
    const bool delivered = (user & delivered_bit) != 0;
    const bool hold = (user & hold_bit) != 0;

    if constexpr (Window == 0) {  // pre_gen
      // Hold the generator while a fragment is pending (ready || delivered);
      // latch "hold" so pre_rec can tell produced-this-round from held-this-
      // round. When delivered (reset round), also clear ready: deliver is over.
      const bool h = ready | delivered;
      u64 w = meta_stream64::ops::yield(U);
      w = meta_stream64::ops::with_gen(w, !h);
      u64 u = (user & ~hold_bit) |
              meta_core64::select64(h, hold_bit, u64{0});
      if (delivered) u = u & ~ready_bit;
      return uw::with_user(w, u);
    } else if constexpr (Window == 1) {  // post_gen
      // A boundary char produced: mark the fragment ready (unless we are in the
      // delivered->reset handshake). Setting ready is idempotent.
      constexpr bool boundary =
          std::is_same_v<meta_produce_t<Obj>, exp_char<'#'>> ||
          std::is_same_v<meta_produce_t<Obj>, exp_char<'\0'>>;
      u64 w = meta_stream64::ops::yield(U);
      u64 u = user;
      if constexpr (boundary) {
        if (!delivered) u = u | ready_bit;
      }
      return uw::with_user(w, u);
    } else if constexpr (Window == 2) {  // pre_rec
      u64 w = meta_stream64::ops::yield(U);
      if (ready && hold) {
        // Deliver: OBS | CONSUME on, delivered set (ready kept), REC suppressed
        // one round via GATES_ARM (sets the gates AND arms the timer). Keeping
        // ready makes this branch idempotent under communicate().
        w = uw::with_user(w, user | delivered_bit);
        return meta_stream64::ops::gates_arm(
            w, gates::defaults, u8{1},
            static_cast<u8>(std::countr_zero(gates::rec)));
      } else if (delivered && !ready) {
        // Reset: G_INITIALIZE force-invokes the receiver's initialize. Not
        // observed, not consuming. delivered stays set (post_rec clears it), so
        // this branch is idempotent under communicate().
        return meta_stream64::ops::set_gates(w, gates::init,
                                             gates::obs | gates::consume);
      }
      // Accumulating round: receive, do not observe, do not consume.
      return meta_stream64::ops::set_gates(w, u8{0},
                                           gates::obs | gates::consume);
    }
    // Window 3 (post_rec): clear delivered only once the reset round is over
    // (!ready && delivered); a deliver round keeps it for the reset round.
    u64 w = meta_stream64::ops::yield(U);
    u64 u = user;
    if (!ready && delivered) u = u & ~delivered_bit;
    return uw::with_user(w, u);
  }();
};
}  // namespace print_detail

template<exp_utilities::static_str s>
void print(auto& os, auto&&... args) {
    using namespace exp_utilities;
    constexpr std::size_t placeholders = [] {
        std::size_t count = 0;
        for (char c : s.str) count += c == '#';
        return count;
    }();
    static_assert(placeholders == sizeof...(args), "print: placeholder/argument count mismatch");

    meta_ios::stream_run<print_detail::condition,
        meta_iostream::meta_ostream<>, meta_iostream::meta_istream<str_to_list<s>>>
        ::for_each_forward([&](auto str_stream, auto&& arg) {
            using fragment = typename decltype(str_stream.object())::pop_back;
            using terminated = typename fragment::template push_back<exp_char<'\0'>>;
            os << to_exp_char_t<terminated>{} << std::forward<decltype(arg)>(arg);
        }, std::forward<decltype(args)>(args)..., "");
}
