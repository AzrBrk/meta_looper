#pragma once

// ============================================================================
// meta_looper64: the real loop on top of meta_loop64::step.
//
// It runs until the loop ends (CONT drops), which is data-driven: there is no
// fixed round budget and no "chunk every N rounds". A payload may still be
// chunked at a driver boundary (meta_chunk64), but that boundary is defined by
// the payload, not by a round counter.
//
// Value plane (`for_each`) matches the old states_looper shape:
//   - the callback runs once per observed round, with an empty stage view;
//   - the walk returns the value of the LAST observed callback;
//   - a callback returning stop_t ends the walk (and is returned);
//     skip_t / yield_t just keep walking.
// ============================================================================

#include "meta_chunk64.hpp"
#include "meta_loop64.hpp"

#include <type_traits>

namespace meta_looper64 {

using meta_core64::u32;
using meta_core64::u64;
using meta_core64::control::skip_t;
using meta_core64::control::yield_t;
using meta_core64::control::stop_t;
namespace lw = meta_core64::lw;

// Empty stage view: types only, no data member.
template <class Rec, class Gen, u32 Pos>
struct stage {
  using to_t = Rec;
  using from_t = Gen;
  static constexpr u32 index_v = Pos;
  consteval u32 index() const noexcept { return Pos; }
};

// "Does any later round observe?" - the look-ahead the value plane needs to
// decide whether this round's value is the last one.
template <class S, bool Cont>
struct observes_any;

template <class S>
struct observes_any<S, false> {
  static constexpr bool value = false;
};

template <class S>
struct observes_any<S, true> {
  using st = meta_loop64::step<S>;
  static constexpr bool value =
      st::observed || observes_any<typename st::next, st::cont>::value;
};

// Data-driven driver: run until CONT drops.
template <class S, bool Cont>
struct drive;

template <class S>
struct drive<S, false> {
  using state = S;
  static constexpr int calls = 0;
};

template <class S>
struct drive<S, true> {
  using st = meta_loop64::step<S>;
  using tail = drive<typename st::next, st::cont>;
  using state = typename tail::state;
  static constexpr int calls = static_cast<int>(st::observed) + tail::calls;
};

// Value plane: walk observed rounds; return the last observed value.
template <class S, bool Cont>
struct walker;

template <class S>
struct walker<S, false> {
  template <class F>
  static constexpr void run(F&&) noexcept {}
};

template <class S>
struct walker<S, true> {
  template <class F>
  static constexpr decltype(auto) run(F&& f) {
    using st = meta_loop64::step<S>;
    using next = typename st::next;
    if constexpr (st::observed) {
      using view = stage<typename st::next::rec, typename st::next::gen,
                         lw::pos_v(st::next::L) - 1>;
      using R = std::invoke_result_t<F&, view>;
      using D = std::remove_cvref_t<R>;
      if constexpr (std::is_same_v<D, stop_t>) {
        return std::invoke(f, view{});
      } else if constexpr (std::is_same_v<D, skip_t> ||
                           std::is_same_v<D, yield_t>) {
        (void)std::invoke(f, view{});
        return walker<next, st::cont>::run(f);
      } else {
        if constexpr (observes_any<next, st::cont>::value) {
          if constexpr (std::is_void_v<R>) {
            std::invoke(f, view{});
          } else {
            (void)std::invoke(f, view{});
          }
          return walker<next, st::cont>::run(f);
        } else {
          return std::invoke(f, view{});
        }
      }
    } else {
      return walker<next, st::cont>::run(f);
    }
  }
};

// Value plane, argument-consuming variant: same decision/return logic as
// for_each, but each consuming round eats one argument; the walk stops when the
// arguments run out.
template <class S, bool Cont>
struct forward_walker;

template <class S>
struct forward_walker<S, false> {
  template <class F, class... A>
  static constexpr void run(F&&, A&&...) noexcept {}
};

template <class S>
struct forward_walker<S, true> {
  // Consume drops `first`, otherwise it is re-passed. Only instantiated when
  // there is at least one more round to run.
  template <class FF, class FirstF, class... RestF>
  static constexpr decltype(auto) next_run(FF&& ff, FirstF&& firstf,
                                           RestF&&... restf) {
    using st = meta_loop64::step<S>;
    using next = typename st::next;
    if constexpr (st::consume) {
      return forward_walker<next, st::cont>::run(
          std::forward<FF>(ff), std::forward<RestF>(restf)...);
    } else {
      return forward_walker<next, st::cont>::run(
          std::forward<FF>(ff), std::forward<FirstF>(firstf),
          std::forward<RestF>(restf)...);
    }
  }

  template <class F, class First, class... Rest>
  static constexpr decltype(auto) run(F&& f, First&& first, Rest&&... rest) {
    using st = meta_loop64::step<S>;
    using next = typename st::next;
    using view = stage<typename st::next::rec, typename st::next::gen,
                       lw::pos_v(st::next::L) - 1>;
    constexpr bool no_more = st::consume && sizeof...(Rest) == 0;

    if constexpr (no_more) {
      // Last consuming round: no recursion, so this round's value is final.
      if constexpr (st::observed) {
        using R = std::invoke_result_t<F&, view, First>;
        using D = std::remove_cvref_t<R>;
        if constexpr (std::is_same_v<D, stop_t> ||
                      std::is_same_v<D, skip_t> ||
                      std::is_same_v<D, yield_t>) {
          (void)std::invoke(f, view{}, std::forward<First>(first));
          return;
        } else {
          return std::invoke(f, view{}, std::forward<First>(first));
        }
      } else {
        return;
      }
    } else {
      if constexpr (st::observed) {
        using R = std::invoke_result_t<F&, view, First>;
        using D = std::remove_cvref_t<R>;
        if constexpr (std::is_same_v<D, stop_t>) {
          return std::invoke(f, view{}, std::forward<First>(first));
        } else if constexpr (std::is_same_v<D, skip_t> ||
                             std::is_same_v<D, yield_t>) {
          (void)std::invoke(f, view{}, std::forward<First>(first));
          return next_run(std::forward<F>(f), std::forward<First>(first),
                          std::forward<Rest>(rest)...);
        } else {
          if constexpr (!observes_any<next, st::cont>::value) {
            return std::invoke(f, view{}, std::forward<First>(first));
          } else {
            (void)std::invoke(f, view{}, std::forward<First>(first));
            return next_run(std::forward<F>(f), std::forward<First>(first),
                            std::forward<Rest>(rest)...);
          }
        }
      } else {
        return next_run(std::forward<F>(f), std::forward<First>(first),
                        std::forward<Rest>(rest)...);
      }
    }
  }
};

template <class Cond, class Rec, class Gen>
struct looper {
  using init = meta_loop64::state<
      lw::make(meta_core64::gates::defaults, 0, 0, 0, 0), 0, Rec, Gen, Cond>;
  using result = drive<init, true>;

  // Product: the final receiver object (same shape as the old looper's ::type).
  using type = typename result::state::rec;
  static constexpr int calls = result::calls;
  static constexpr u32 pos = lw::pos_v(result::state::L);

  template <class F>
  static constexpr decltype(auto) for_each(F&& f) {
    return walker<init, true>::run(f);
  }

  template <class F, class First, class... Rest>
  static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                   Rest&&... rest) {
    return forward_walker<init, true>::run(
        std::forward<F>(f), std::forward<First>(first),
        std::forward<Rest>(rest)...);
  }
};

}  // namespace meta_looper64
