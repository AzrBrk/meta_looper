#pragma once

// ============================================================================
// meta_function_series.hpp: chains multiple function_binders into one series.
//
// It mirrors select_type's exp_function_series, but:
//
//   1) Each stage keeps its own inline type_safe_aligned_range argument stack
//      (see meta_function_binder.hpp); the whole series is just
//
//        std::byte[field_size<...>]        - aligned buffer of each stage binder
//        std::byte[field_size<...>]        - aligned buffer of each return value
//        series_index / argc_index         - two 16-bit cursors
//
//      Binder and return values both use the type_safe_aligned_range layout. The
//      binder offset array comes from meta_pipe (aligned_offset_stream), and
//      construction / destruction / addressing all use seek_to compile-time
//      offsets: no heterogeneous value container, linked list, shared_ptr or
//      per-node heap allocation.
//
//   2) Return values no longer live in a heterogeneous value container: every
//      stage result is laid out in one aligned buffer with the
//      type_safe_aligned_range layout (same field_size / aligned_offset_stream as
//      the argument stacks). Hence:
//
//        auto [m, w, ok] = series._return();     // structured bindings
//        series._return().get<1>()               // compile-time index, exact type T&
//        series._return()[1]                     // run-time index, checked proxy
//        series._return<1>()                     // the same, member-template form
//        series.final_return()                   // the last stage's return value
//
//      A void stage has no value; its slot stores void* (nullptr), matching the
//      placeholder of the old library's _return().
//
// 3) There are two execution paths:
//
//      _continue() / _bind_continue()   expanded at compile time, one stage after
//                                       another, direct calls (fast)
//      drive()                          driven by the underlying looper state
//                                       machine: one stage per round, chunked by
//                                       the driver, each round writes that stage's
//                                       result into the aligned buffer; a long
//                                       chain never deepens the template recursion
//      run_loop(n, fn)                  runs "reset + drive" n times, handing each
//                                       iteration's return range to fn
//
// Typical usage:
//
//   auto s = meta_bind::link_f(
//       [](int a, double b) { return b - a; },      // stage 0
//       [](double c) { return std::to_string(c); }, // stage 1
//       []() { std::cout << "end\n"; });            // stage 2
//
//   meta_bind::series_bind(s, 1, 1.33, 0.33);       // feed arguments in stage order
//   s._continue();                                  // run every stage to the end
//   auto [first, second, v] = s._return();          // void -> void*(nullptr)
//
// Or let each stage's result flow into the next one:
//
//   auto s = meta_bind::link_f([](int a) { return a * 2; },
//                              [](int v) { return v + 1; });
//   s.bind(21);          // feed only stage 0
//   s._bind_continue();  // stage 1 receives 42 automatically
//   s.final_return();    // 43
//
// Semantics:
//   - every stage owns its cursor and argument stack; series_bind's flat
//     allocation walks all stage slots in order and may resume at argc_at(n);
//   - _continue runs from the current series_index to the end, _stop_at(i) stops after stage i;
//   - _bind_continue / drive append the previous result to the next stage's
//     current binding cursor (skipped when the next stage takes no arguments or
//     the previous one returns void);
//   - the return buffer is default constructed with the series and written during
//     execution; copies and moves travel with the series, so a series stays an
//     ordinary value type;
//   - any out-of-range index, type mismatch or unbound slot throws meta_bind::exception.
// ============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "meta_function_binder.hpp"
#include "meta_states.hpp"

// Stage objects live in one contiguous byte buffer: by default they are read back
// with a direct reinterpret_cast at a compile-time offset (placement new already
// constructed them and the type is known). Define META_BIND_STRICT_LAUNDER=1 to
// use std::launder inside seek_to::get instead (the strict object-model path,
// about 3x slower on GCC 16).
#ifndef META_BIND_STRICT_LAUNDER
#  define META_BIND_STRICT_LAUNDER 0
#endif

namespace meta_bind {

namespace detail {

// void has no value: the slot stores void* (nullptr), as the old library's _return() did.
template <class R>
struct return_storage {
  using type = R;
};
template <>
struct return_storage<void> {
  using type = void*;
};

template <class Binder>
using return_slot_of = typename return_storage<typename Binder::return_type>::type;

// Used by series_cat: call the callback only on the last round (OBS opens on the
// last round and stays closed before it), and drop CONT after N rounds.
template <std::size_t N>
struct last_round_condition {
  template <meta_core64::u8 Window, meta_core64::u64 L, meta_core64::u64 U,
            class Obj>
  static constexpr meta_core64::u64 value = [] {
    meta_core64::u64 w = meta_stream64::ops::yield(U);
    if constexpr (Window == meta_states::pre_rec_exec) {
      const meta_core64::u32 pos = meta_core64::lw::pos_v(L);
      // Observe the Nth round (pos == N-1); stop once pos reaches N.
      w = meta_stream64::ops::with_obs(w, pos + 1 == N);
      if (pos >= N) w = meta_stream64::ops::stop(w);
    }
    return w;
  }();
};

}  // namespace detail

// ============================================================================
// Run-time and compile-time sub-chain views.
// ============================================================================
// series[I] returns a run-time sub-chain view: the start stage is a run-time
// value, _continue / _bind_continue run from I to the end, and each stage is
// located through a static constexpr thunk table plus stage_offsets, so the
// first I stages need no run-time guards.
template <class Series>
class runtime_sub_series {
 public:
  runtime_sub_series(Series& owner, std::size_t start) noexcept
      : owner_(owner), start_(start), index_(start) {}

  std::size_t size() const noexcept { return owner_.stage_count - start_; }
  std::size_t series_index() const noexcept { return index_; }

  void _continue() {
    while (index_ < owner_.stage_count) owner_.execute_index(index_++);
    owner_.set_series_index(index_);
  }

  void _bind_continue() {
    while (index_ < owner_.stage_count) owner_.advance_index(index_++);
    owner_.set_series_index(index_);
  }

  void execute() {
    owner_.execute_index(index_);
    ++index_;
    owner_.set_series_index(index_);
  }

  void _stop_at(std::size_t end) {
    while (index_ <= end && index_ < owner_.stage_count) execute();
  }

  template <class... Args>
  void bind(Args&&... args) {
    owner_.bind_index(index_, std::forward<Args>(args)...);
  }

  auto _return() noexcept { return owner_._return(); }
  auto _return() const noexcept { return owner_._return(); }
  auto returns() noexcept { return owner_.returns(); }
  auto final_return() noexcept { return owner_.final_return(); }

  std::byte* stage_address(std::size_t index) { return owner_.stage_address(index); }

// Absolute stage number, as in series[I] (not an offset relative to start_).
  runtime_sub_series operator[](std::size_t absolute) const {
    if (absolute > owner_.stage_count) throw exception{error::index_exceed};
    return runtime_sub_series(owner_, absolute);
  }

 private:
  Series& owner_;
  std::size_t start_;
  std::size_t index_;
};

// series.at<I>() returns a compile-time sub-chain view: the stage is a template
// argument and expands into direct calls.
template <class Series, std::size_t Start>
class static_sub_series {
 public:
  explicit static_sub_series(Series& owner) noexcept : owner_(owner) {}

  static constexpr std::size_t size() noexcept {
    return Series::stage_count - Start;
  }
  static constexpr std::size_t series_index() noexcept { return Start; }

  void _continue() { owner_.template continue_from_static<Start>(); }
  void _bind_continue() { owner_.template bind_continue_from_static<Start>(); }

 private:
  Series& owner_;
};

template <class... Binders>
class function_series {
  static_assert(sizeof...(Binders) > 0,
                "function_series requires at least one binder");

 public:
  static constexpr std::size_t stage_count = sizeof...(Binders);
  static constexpr std::size_t return_count = stage_count;
  static constexpr std::size_t total_args = (Binders::arg_count + ... + 0);
  using binders_list = exp_list<Binders...>;

 private:
// Stage binders also sit in one contiguous buffer with the aligned_range layout:
// the offset array comes from meta_pipe (aligned_offset_stream), and construction,
// access and destruction all use seek_to compile-time offsets, never a run-time
// iterator.
  using stages_list = exp_list<Binders...>;
  using stages_layout = detail::aligned_slot_layout<0, stages_list>;

// Standard looper pipeline: index_sequence_istream<stage_count> produces exactly
// N rounds, meta_jostream collects the indices by type, and stream_states_co<>
// detects the end; no custom stop condition and no repeat-plus-manual-G_CONTINUE.
  using series_generator =
      meta_iostream::index_sequence_istream<stage_count>;
  using series_receiver = meta_iostream::meta_jostream<exp_list<>>;
  using series_condition = meta_ios::stream_states_co<>;
  using series_loop =
      meta_states::run<series_condition, series_receiver, series_generator>;

    using stages_loop = meta_states::run<
      meta_ios::node_con<meta_stream64::unchanged, true>,
      meta_iostream::meta_aligned_iterator<>,
      meta_iostream::meta_istream<stages_list>>;

  template <std::size_t I>
  using stage_type = exp_select<I, stages_list>;
  template <std::size_t I>
  using stage_cursor = typename stages_layout::template cursor<I>;

 public:
// Entry I is the absolute offset of binder I from the start of the stages buffer.
// Same thing as type_safe_aligned_range::aligned_layout::offsets; only the input
// here is a type list of binders fed through meta_pipe.
  static constexpr auto stage_offsets =
      to_meta_array_t<type_safe_aligned::aligned_offset_stream<
          0, Binders...>>::array();
  static constexpr std::size_t stage_bytes = stages_layout::bytes;
  static_assert(stage_offsets.size() == sizeof...(Binders));

 private:
// Return slots use the same aligned_range layout as the argument stacks (Alignment = 0).
  using returns_list =
      exp_list<detail::return_slot_of<Binders>...>;
  using returns_layout = detail::aligned_slot_layout<0, returns_list>;
  using returns_range = typename returns_layout::range_type;

  template <std::size_t I>
  using return_slot = typename returns_layout::template slot<I>;
  template <std::size_t I>
  using return_cursor = typename returns_layout::template cursor<I>;

// Return-value offset table: the same meta_pipe layout.
  static constexpr auto return_offsets =
      to_meta_array_t<type_safe_aligned::aligned_offset_stream<
          0, detail::return_slot_of<Binders>...>>::array();

  static_assert(
      ((std::is_void_v<typename Binders::return_type> ||
        std::is_default_constructible_v<typename Binders::return_type>)&&...),
      "function_series: every non-void return type must be default "
      "constructible (reference returns are not supported)");

  static_assert(
      ((std::is_void_v<typename Binders::return_type> ||
        std::is_constructible_v<detail::return_slot_of<Binders>,
                                detail::return_slot_of<Binders>&&>)&&...),
      "function_series: return values must be move- or copy-constructible");

  static constexpr bool stages_copyable =
      (std::is_copy_constructible_v<Binders> && ...);
  static constexpr bool stages_movable =
      (std::is_move_constructible_v<Binders> && ...);
  static constexpr bool returns_copyable =
      (std::is_copy_constructible_v<detail::return_slot_of<Binders>> && ...);
  static constexpr bool returns_movable =
      (std::is_move_constructible_v<detail::return_slot_of<Binders>> && ...);

// Stage I's argument count and the offset of its first slot among all stage slots.
  template <std::size_t I>
  static constexpr std::size_t stage_arity() {
    return stage_type<I>::arg_count;
  }

  template <std::size_t I, std::size_t J = 0, std::size_t Acc = 0>
  static constexpr std::size_t stage_start_impl() {
    if constexpr (J == I) {
      return Acc;
    } else {
      return stage_start_impl<I, J + 1, Acc + stage_arity<J>()>();
    }
  }

  template <std::size_t I>
  static constexpr std::size_t stage_start() {
    return stage_start_impl<I>();
  }

  template <class Arg>
  static constexpr bool any_stage_accepts() {
    return (Binders::template accepts_all<Arg>() || ...);
  }

 public:
// ---- Construction / destruction ----
  explicit function_series(Binders... binders) {
    construct_stages(std::move(binders)...);
    try {
      construct_returns(std::make_index_sequence<stage_count>{});
    } catch (...) {
      destroy_stages();
      throw;
    }
  }

// Concatenate any number of series: move every binder into this object's
// contiguous buffer.
  template <class... Series>
  static function_series from_series(Series&&... series) {
    static_assert(sizeof...(Series) > 0,
                  "function_series::from_series: at least one series");
    static_assert((std::remove_cvref_t<Series>::stage_count + ... + 0) ==
                      stage_count,
                  "function_series::from_series: stage count mismatch");
    return function_series(multi_cat_tag{}, std::forward<Series>(series)...);
  }

 private:
  struct multi_cat_tag {};

  template <class... Series>
  explicit function_series(multi_cat_tag, Series&&... series) {
    try {
      construct_series_pack<0>(std::forward<Series>(series)...);
    } catch (...) {
      destroy_stages();
      throw;
    }
    try {
      construct_returns(std::make_index_sequence<stage_count>{});
    } catch (...) {
      destroy_stages();
      throw;
    }
  }

  template <std::size_t TargetBase, class Series, class... Rest>
  void construct_series_pack(Series&& series, Rest&&... rest) {
    append_series_stages<TargetBase>(
        std::forward<Series>(series),
        std::make_index_sequence<
            std::remove_cvref_t<Series>::stage_count>{});
    if constexpr (sizeof...(Rest) > 0) {
      construct_series_pack<
          TargetBase + std::remove_cvref_t<Series>::stage_count>(
          std::forward<Rest>(rest)...);
    }
  }

  template <std::size_t TargetBase, class Series, std::size_t... I>
  void append_series_stages(Series&& series, std::index_sequence<I...>) {
    (construct_stage_at<TargetBase + I>(
         std::move(std::forward<Series>(series).template stage<I>())),
     ...);
  }

  template <std::size_t I, class Binder>
  void construct_stage_at(Binder&& binder) {
    ::new (static_cast<void*>(stage_address<I>()))
        stage_type<I>(std::forward<Binder>(binder));
    ++stages_constructed_;
  }

 public:

  function_series(function_series const& other)
    requires(stages_copyable && returns_copyable)
      : series_index_(other.series_index_), argc_index_(other.argc_index_) {
    copy_stages(other);
    try {
      copy_returns(other, std::make_index_sequence<stage_count>{});
    } catch (...) {
      destroy_stages();
      throw;
    }
  }

  function_series(function_series&& other)
    requires(stages_movable && returns_movable)
      : series_index_(other.series_index_), argc_index_(other.argc_index_) {
    move_stages(other);
    try {
      move_returns(other, std::make_index_sequence<stage_count>{});
    } catch (...) {
      destroy_stages();
      throw;
    }
  }

  function_series& operator=(function_series const& other)
    requires(stages_copyable && returns_copyable) {
    if (this == &other) return *this;
    destroy_returns();
    destroy_stages();
    copy_stages(other);
    try {
      copy_returns(other, std::make_index_sequence<stage_count>{});
    } catch (...) {
      destroy_stages();
      throw;
    }
    series_index_ = other.series_index_;
    argc_index_ = other.argc_index_;
    return *this;
  }

  function_series& operator=(function_series&& other)
    requires(stages_movable && returns_movable) {
    if (this == &other) return *this;
    destroy_returns();
    destroy_stages();
    move_stages(other);
    try {
      move_returns(other, std::make_index_sequence<stage_count>{});
    } catch (...) {
      destroy_stages();
      throw;
    }
    series_index_ = other.series_index_;
    argc_index_ = other.argc_index_;
    return *this;
  }

  ~function_series() {
    destroy_returns();
    destroy_stages();
  }

// ---- Access ----
  template <std::size_t I>
  auto& stage() noexcept {
    static_assert(I < stage_count, "function_series::stage: index out of range");
    return stage_at<I>();
  }

  template <std::size_t I>
  auto const& stage() const noexcept {
    static_assert(I < stage_count, "function_series::stage: index out of range");
    return stage_at<I>();
  }

// Run-time index: address of binder index (base plus the meta_pipe offset table).
  std::byte* stage_address(std::size_t index) noexcept {
    if (index >= stage_count) throw exception{error::index_exceed};
    return stages_storage_.data() + stage_offsets[index];
  }

  static constexpr std::size_t size() noexcept { return stage_count; }
  template <class... Args>
    requires(sizeof...(Args) == total_args)
  void initialize(Args&&... args) {
    initialize_empty(std::make_index_sequence<stage_count>{},
                     std::forward<Args>(args)...);
  }

 private:
  template <std::size_t Index, class Arg, std::size_t... Stage>
  void initialize_flat_slot(Arg&& arg, std::index_sequence<Stage...>) {
    constexpr auto owner = [] {
      constexpr std::array starts{stage_start<Stage>()...};
      constexpr std::array counts{stage_type<Stage>::arg_count...};
      for (std::size_t stage = 0; stage < stage_count; ++stage)
        if (Index >= starts[stage] && Index < starts[stage] + counts[stage])
          return stage;
      return stage_count;
    }();
    stage_at<owner>().template emplace_empty_slot<Index - stage_start<owner>()>(
        std::forward<Arg>(arg));
  }

  template <std::size_t... Index, class... Args>
  void initialize_flat_slots(std::index_sequence<Index...>, Args&&... args) {
    (initialize_flat_slot<Index>(std::forward<Args>(args),
                                std::make_index_sequence<stage_count>{}), ...);
  }

  template <std::size_t... Stage, class... Args>
  void initialize_empty(std::index_sequence<Stage...>, Args&&... args) {
    if (((stage_at<Stage>().bound_count() != 0) || ...))
      throw exception{error::argument_overflow};
    try {
      initialize_flat_slots(std::make_index_sequence<total_args>{},
                            std::forward<Args>(args)...);
      argc_index_ = total_args;
    } catch (...) {
      (stage_at<stage_count - 1 - Stage>().clear(), ...);
      argc_index_ = 0;
      throw;
    }
  }

 public:
  std::size_t series_index() const noexcept { return series_index_; }
  std::size_t argc_index() const noexcept { return argc_index_; }

// Return view: type_safe_aligned_range with both compile-time and run-time indices.
  returns_range _return() noexcept {
    return returns_range{returns_storage_.data()};
  }

// The range is a view (constness does not propagate to elements, as with
// std::span), so a const series still yields the same writable view.
  returns_range _return() const noexcept {
    return returns_range{const_cast<std::byte*>(returns_storage_.data())};
  }

  returns_range returns() noexcept { return _return(); }
  returns_range returns() const noexcept { return _return(); }

// Compile-time index: reference to the exact return type, with no checks.
  template <std::size_t I>
  auto& _return() noexcept {
    static_assert(I < stage_count, "function_series::_return<I>: out of range");
    return return_at<I>();
  }

  template <std::size_t I>
  auto const& _return() const noexcept {
    static_assert(I < stage_count, "function_series::_return<I>: out of range");
    return return_at<I>();
  }

  decltype(auto) final_return() noexcept {
    return return_at<stage_count - 1>();
  }

  decltype(auto) final_return() const noexcept {
    return return_at<stage_count - 1>();
  }

// series[I] returns a run-time sub-chain view; I may be a run-time value.
//   series[I]._continue() / _bind_continue() run from I to the end;
//   series[I].bind(...) / execute() act on the view's current stage;
//   I == stage_count is the end view (empty body); I > stage_count throws.
// To move a cursor only (old-library semantics) use select(I), which returns
// function_series&.
  runtime_sub_series<function_series> operator[](std::size_t index) {
    if (index > stage_count) throw exception{error::index_exceed};
    return runtime_sub_series<function_series>(*this, index);
  }

  function_series& select(std::size_t index) {
    if (index > stage_count) throw exception{error::index_exceed};
    series_index_ = static_cast<std::uint16_t>(index);
    return *this;
  }

  function_series& argc_at(std::size_t index) {
    if (index > total_args) throw exception{error::argument_overflow};
    argc_index_ = static_cast<std::uint16_t>(index);
    return *this;
  }

// ---- Binding to the current stage ----
  template <class... Args>
  void bind(Args&&... args) {
    if constexpr (sizeof...(Args) > 0) {
      static_assert((any_stage_accepts<Args>() && ...),
                    "function_series::bind: no stage can accept these arguments");
    }
    if (series_index_ >= stage_count) throw exception{error::index_exceed};
    bind_stage<0>(series_index_, std::forward<Args>(args)...);
  }

// ---- Flat binding: all stage slots as one argument stream in stage order ----
  template <class... Args>
  std::size_t bind_flat(Args&&... args) {
    if constexpr (sizeof...(Args) > 0) {
      static_assert((any_stage_accepts<Args>() && ...),
                    "function_series::bind_flat: no stage can accept these "
                    "arguments");
    }
    std::size_t position = argc_index_;
    (bind_flat_one(position, std::forward<Args>(args)), ...);
    argc_index_ = static_cast<std::uint16_t>(position);
    return position;
  }

 private:
// For the run-time cursor: one static thunk per index (I is baked into the body
// at compile time); the table lives in .rdata and takes no object storage.
// Addresses still come from stage_offsets.
  using stage_fn = void (*)(function_series&);

  template <std::size_t I>
  static void execute_thunk(function_series& self) {
    self.template execute_stage<I>();
  }

  template <std::size_t... I>
  static constexpr auto make_execute_table(std::index_sequence<I...>) {
    return std::array<stage_fn, sizeof...(I)>{&execute_thunk<I>...};
  }

  static constexpr auto execute_table =
      make_execute_table(std::make_index_sequence<stage_count>{});

  template <std::size_t I>
  static void advance_thunk(function_series& self) {
    self.template advance_stage<I>();
  }

  template <std::size_t... I>
  static constexpr auto make_advance_table(std::index_sequence<I...>) {
    return std::array<stage_fn, sizeof...(I)>{&advance_thunk<I>...};
  }

  static constexpr auto advance_table =
      make_advance_table(std::make_index_sequence<stage_count>{});

  template <std::size_t Previous>
  static void bind_last_thunk(function_series& self) {
    self.template feed_return_into<Previous + 1>();
  }

  template <std::size_t... P>
  static constexpr auto make_bind_last_table(std::index_sequence<P...>) {
    return std::array<stage_fn, sizeof...(P)>{&bind_last_thunk<P>...};
  }

// Indexed by the previous stage; the table is empty and never indexed when
// stage_count == 1.
  static constexpr auto bind_last_table =
      make_bind_last_table(std::make_index_sequence<stage_count - 1>{});

 public:
// ---- Execution ----
// Run-time cursor: static constexpr thunk table plus a run-time index, O(1), no
// compare chain.
  void execute() {
    if (series_index_ >= stage_count) throw exception{error::index_exceed};
    execute_table[series_index_](*this);
    ++series_index_;
  }

  void _continue() { continue_stage<0>(); }

  void _stop_at(std::size_t index) {
    while (series_index_ <= index) {
      if (series_index_ >= stage_count) break;
      execute();
    }
  }

  void _bind_continue() { bind_continue_stage<0>(); }

// ---- Single-stage entries used by sub-chain views and the run-time cursor ----
// execute_index / advance_index run only stage index and do not depend on
// series_index_, because the view keeps its own start stage and may begin at
// any run-time index.
  void execute_index(std::size_t index) {
    if (index >= stage_count) throw exception{error::index_exceed};
    execute_table[index](*this);
  }

  void advance_index(std::size_t index) {
    if (index >= stage_count) throw exception{error::index_exceed};
    advance_table[index](*this);
  }

  template <class... Args>
  void bind_index(std::size_t index, Args&&... args) {
    if (index >= stage_count) throw exception{error::index_exceed};
    bind_stage<0>(index, std::forward<Args>(args)...);
    series_index_ = static_cast<std::uint16_t>(index);
  }

  void set_series_index(std::size_t index) noexcept {
    series_index_ = static_cast<std::uint16_t>(index);
  }

// series.at<I>(): a compile-time sub-chain view that expands into direct calls.
  template <std::size_t Start>
  static_sub_series<function_series, Start> at() noexcept {
    static_assert(Start <= stage_count,
                  "function_series::at<Start>: out of range");
    return static_sub_series<function_series, Start>{*this};
  }

  template <std::size_t Start>
  void continue_from_static() {
    if constexpr (Start < stage_count) {
      execute_stage<Start>();
      series_index_ = static_cast<std::uint16_t>(Start + 1);
      continue_from_static<Start + 1>();
    }
  }

  template <std::size_t Start>
  void bind_continue_from_static() {
    if constexpr (Start < stage_count) {
      feed_return_into<Start>();
      execute_stage<Start>();
      series_index_ = static_cast<std::uint16_t>(Start + 1);
      bind_continue_from_static<Start + 1>();
    }
  }

// Runs stage by stage through the underlying looper state machine: one stage per
// round, the driver advancing and chunking; each round's callback runs that
// stage (the number is constexpr, so there is no run-time dispatch).
  void drive() {
    if (series_index_ >= stage_count) return;
    series_loop::for_each([this](auto stage) {
      constexpr std::size_t index = stage.index();
      if (index >= series_index_) advance_stage<index>();
    });
  }

// for_each: run the whole chain and call f(stage, args...) after each stage.
// Same semantics as the looper's for_each: f may return skip_t / yield_t /
// stop_t to steer later callbacks (stop_t prevents later stages from running);
// otherwise it returns the last observed value. This only runs the stages; it
// does not feed results forward.
  // _bind_continue / drive。
  template <class F, class... Args>
  decltype(auto) for_each(F&& f, Args&&... args) {
    series_index_ = 0;
    return series_loop::for_each(
        [this, &f, &args...](auto stage) -> decltype(auto) {
          constexpr std::size_t index = stage.index();
          execute_stage<index>();
          series_index_ = static_cast<std::uint16_t>(index + 1);
          return std::invoke(f, stage, std::forward<Args>(args)...);
        });
  }

// for_each_forward: every stage receives one extra argument (as in the looper's
// for_each_forward) and supports skip_t / yield_t / stop_t as well.
  template <class F, class First, class... Rest>
  decltype(auto) for_each_forward(F&& f, First&& first, Rest&&... rest) {
    series_index_ = 0;
    return series_loop::for_each_forward(
        [this, &f](auto stage, auto&& arg) -> decltype(auto) {
          constexpr std::size_t index = stage.index();
          execute_stage<index>();
          series_index_ = static_cast<std::uint16_t>(index + 1);
          return std::invoke(f, stage, std::forward<decltype(arg)>(arg));
        },
        std::forward<First>(first), std::forward<Rest>(rest)...);
  }

// Runs count times: each iteration does reset + drive and hands the return range
// to the callback; that range points into the series return buffer and is
// overwritten by the next iteration.
  template <class Fn>
  void run_loop(std::size_t count, Fn&& on_returns) {
    for (std::size_t i = 0; i < count; ++i) {
      reset();
      drive();
      on_returns(_return());
    }
  }

// Binds the previous stage's result into the current stage (old-library entry).
  void bind_last_return() {
    if constexpr (stage_count == 1) {
      throw exception{error::index_exceed};
    } else {
      if (series_index_ == 0 || series_index_ >= stage_count) {
        throw exception{error::index_exceed};
      }
      bind_last_table[series_index_ - 1](*this);
    }
  }

// Clears every stage binding cursor and the flat cursor; return values are kept.
  void clear_all_argc_idx() {
    stages_loop::for_each([this](auto stage) {
      using cursor = typename decltype(stage)::to_t;
#if META_BIND_STRICT_LAUNDER
      cursor{}.get(stages_storage_.data()).reset_argc_stack();
#else
      using binder = typename cursor::type;
      reinterpret_cast<binder*>(cursor{}.address_of(stages_storage_.data()))
          ->reset_argc_stack();
#endif
    });
    argc_index_ = 0;
  }

// Restart at stage 0: the series cursor, the flat cursor and every stage cursor
// go back to zero. Bound argument stacks stay (a new bind overwrites them), so
// rerunning the same arguments needs only reset() + _continue() (or drive()).
  void reset() {
    series_index_ = 0;
    clear_all_argc_idx();
  }

 private:
  template <std::size_t I>
  std::byte* return_address() noexcept {
    return return_cursor<I>{}.address_of(returns_storage_.data());
  }

  template <std::size_t I>
  auto& return_at() noexcept {
#if META_BIND_STRICT_LAUNDER
    return return_cursor<I>{}.get(returns_storage_.data());
#else
    return *reinterpret_cast<return_slot<I>*>(
        returns_storage_.data() + return_offsets[I]);
#endif
  }

  template <std::size_t I>
  auto const& return_at() const noexcept {
#if META_BIND_STRICT_LAUNDER
    return return_cursor<I>{}.get(returns_storage_.data());
#else
    return *reinterpret_cast<return_slot<I> const*>(
        returns_storage_.data() + return_offsets[I]);
#endif
  }

// ---- Stage objects (binders): contiguous buffer + meta_pipe offset table ----
  template <std::size_t I>
  std::byte* stage_address() noexcept {
    return stage_cursor<I>{}.address_of(stages_storage_.data());
  }

// Reference to a binder. The object was placement-new constructed, its type is
// known at compile time and its address is the constant stage_offsets[I]; by
// default this is a direct reinterpret_cast (GCC 16 treats std::launder inside
// seek_to::get as an optimization barrier and a 3-stage chain runs about 3x
// slower). For the strict P0137 object model define META_BIND_STRICT_LAUNDER=1.
  template <std::size_t I>
  auto& stage_at() noexcept {
#if META_BIND_STRICT_LAUNDER
    return stage_cursor<I>{}.get(stages_storage_.data());
#else
    return *reinterpret_cast<stage_type<I>*>(
        stages_storage_.data() + stage_offsets[I]);
#endif
  }

  template <std::size_t I>
  auto const& stage_at() const noexcept {
#if META_BIND_STRICT_LAUNDER
    return stage_cursor<I>{}.get(stages_storage_.data());
#else
    return *reinterpret_cast<stage_type<I> const*>(
        stages_storage_.data() + stage_offsets[I]);
#endif
  }

  template <std::size_t I, class Binder, class... Rest>
  void construct_stages_from(Binder&& binder, Rest&&... rest) {
    ::new (static_cast<void*>(stage_address<I>()))
        stage_type<I>(std::forward<Binder>(binder));
    ++stages_constructed_;
    if constexpr (sizeof...(Rest) > 0) {
      construct_stages_from<I + 1>(std::forward<Rest>(rest)...);
    }
  }

  void construct_stages(Binders&&... binders) {
    try {
      construct_stages_from<0>(std::move(binders)...);
    } catch (...) {
      destroy_stages();
      throw;
    }
  }

  template <std::size_t I>
  void copy_stages_from(function_series const& other) {
    ::new (static_cast<void*>(stage_address<I>()))
        stage_type<I>(other.template stage_at<I>());
    ++stages_constructed_;
    if constexpr (I + 1 < stage_count) copy_stages_from<I + 1>(other);
  }

  void copy_stages(function_series const& other) {
    try {
      copy_stages_from<0>(other);
    } catch (...) {
      destroy_stages();
      throw;
    }
  }

  template <std::size_t I>
  void move_stages_from(function_series& other) {
    ::new (static_cast<void*>(stage_address<I>()))
        stage_type<I>(std::move(other.template stage_at<I>()));
    ++stages_constructed_;
    if constexpr (I + 1 < stage_count) move_stages_from<I + 1>(other);
  }

  void move_stages(function_series& other) {
    try {
      move_stages_from<0>(other);
    } catch (...) {
      destroy_stages();
      throw;
    }
  }

  template <std::size_t I = 0>
  void destroy_stages_upto(std::size_t count) noexcept {
    if constexpr (I < stage_count) {
      if (I < count) stage_cursor<I>{}.destroy(stages_storage_.data());
      destroy_stages_upto<I + 1>(count);
    }
  }

  void destroy_stages() noexcept {
    if (stages_constructed_ == 0) return;
    destroy_stages_upto<0>(stages_constructed_);
    stages_constructed_ = 0;
  }

  template <std::size_t... I>
  void construct_returns(std::index_sequence<I...>) {
    std::size_t constructed = 0;
    try {
      ((::new (static_cast<void*>(return_address<I>())) return_slot<I>{},
        ++constructed),
       ...);
    } catch (...) {
      destroy_returns_upto(constructed);
      throw;
    }
    returns_ready_ = true;
  }

  template <std::size_t I = 0>
  void destroy_returns_upto(std::size_t count) noexcept {
    if constexpr (I < stage_count) {
      if (I < count) {
        return_cursor<I>{}.destroy(returns_storage_.data());
      }
      destroy_returns_upto<I + 1>(count);
    }
  }

  void destroy_returns() noexcept {
    if (!returns_ready_) return;
    destroy_returns_upto<0>(stage_count);
    returns_ready_ = false;
  }

  template <std::size_t... I>
  void copy_returns(function_series const& other, std::index_sequence<I...>) {
    std::size_t constructed = 0;
    try {
      ((::new (static_cast<void*>(return_address<I>()))
            return_slot<I>(other.template return_at<I>()),
        ++constructed),
       ...);
    } catch (...) {
      destroy_returns_upto(constructed);
      throw;
    }
    returns_ready_ = true;
  }

  template <std::size_t... I>
  void move_returns(function_series& other, std::index_sequence<I...>) {
    std::size_t constructed = 0;
    try {
      ((::new (static_cast<void*>(return_address<I>()))
            return_slot<I>(std::move(other.template return_at<I>())),
        ++constructed),
       ...);
    } catch (...) {
      destroy_returns_upto(constructed);
      throw;
    }
    returns_ready_ = true;
  }

  template <std::size_t I, class... Args>
  void bind_stage(std::size_t index, Args&&... args) {
    if (index == I) {
      if constexpr ((stage_type<I>::template accepts_all<Args>() && ...)) {
        stage_at<I>().bind_a_lot(std::forward<Args>(args)...);
      } else {
        throw exception{error::type_mismatch};
      }
      return;
    }
    if constexpr (I + 1 < stage_count) {
      bind_stage<I + 1>(index, std::forward<Args>(args)...);
    } else {
      throw exception{error::type_mismatch};
    }
  }

  template <class Arg>
  void bind_flat_one(std::size_t& position, Arg&& arg) {
    if (position >= total_args) throw exception{error::argument_overflow};
    bind_global<0, 0>(position, std::forward<Arg>(arg));
    ++position;
  }

// Compares slot Local of stage Stage with the current flat slot and binds on a hit.
  template <std::size_t Stage, std::size_t Local, class Arg>
  void bind_global(std::size_t position, Arg&& arg) {
    if (position == stage_start<Stage>() + Local) {
      if constexpr (stage_type<Stage>::template slot_accepts<Local, Arg>()) {
        stage_at<Stage>().template bind_at<Local>(std::forward<Arg>(arg));
      } else {
        throw exception{error::type_mismatch};
      }
      return;
    }
    if constexpr (Local + 1 < stage_arity<Stage>()) {
      bind_global<Stage, Local + 1>(position, std::forward<Arg>(arg));
    } else if constexpr (Stage + 1 < stage_count) {
      bind_global<Stage + 1, 0>(position, std::forward<Arg>(arg));
    } else {
      throw exception{error::index_exceed};
    }
  }

  template <std::size_t I>
  void execute_stage() {
    using return_type = typename stage_type<I>::return_type;
    if constexpr (std::is_void_v<return_type>) {
      stage_at<I>().apply_func();
      return_at<I>() = nullptr;
    } else {
      auto value = stage_at<I>().apply_func();
      using slot = return_slot<I>;
      if constexpr (std::is_assignable_v<slot&, return_type&&>) {
        return_at<I>() = std::move(value);
      } else {
        return_cursor<I>{}.destroy(returns_storage_.data());
        ::new (static_cast<void*>(return_address<I>()))
            slot(std::move(value));
      }
    }
  }

  template <std::size_t I>
  void continue_stage() {
    if constexpr (I < stage_count) {
      if (series_index_ <= I) {
        execute_stage<I>();
        series_index_ = static_cast<std::uint16_t>(I + 1);
      }
      continue_stage<I + 1>();
    }
  }

// Appends the previous stage's result at stage I's current binding cursor.
  template <std::size_t I>
  void feed_return_into() {
    if constexpr (I > 0) {
      using previous_return = typename stage_type<I - 1>::return_type;
      if constexpr (!std::is_void_v<previous_return> && stage_arity<I>() > 0) {
// Copyable results are fed by value (the stage keeps its own result for later
// reads); move-only ones (unique_ptr, ...) are moved across. A front_feed
// component overwrites slot 0 with the previous result (the HMODULE of
// register -> create, the HWND of show -> loop).
        if constexpr (std::is_copy_constructible_v<previous_return>) {
          if (stage_at<I>().front_feed()) {
            stage_at<I>().bind_front(return_at<I - 1>());
          } else {
            stage_at<I>().bind(return_at<I - 1>());
          }
        } else {
          if (stage_at<I>().front_feed()) {
            stage_at<I>().bind_front(std::move(return_at<I - 1>()));
          } else {
            stage_at<I>().bind(std::move(return_at<I - 1>()));
          }
        }
      }
    }
  }

// Runs stage I (absorbing the previous result first) and advances the series
// cursor to I+1.
  template <std::size_t I>
  void advance_stage() {
    static_assert(I < stage_count, "function_series::advance_stage: out of range");
    feed_return_into<I>();
    execute_stage<I>();
    series_index_ = static_cast<std::uint16_t>(I + 1);
  }

  template <std::size_t I>
  void bind_continue_stage() {
    if constexpr (I < stage_count) {
      if (series_index_ <= I) advance_stage<I>();
      bind_continue_stage<I + 1>();
    }
  }

  alignas(stages_layout::alignment)
      std::array<std::byte, stages_layout::bytes> stages_storage_{};
  std::uint16_t stages_constructed_{0};
  alignas(returns_layout::alignment)
      std::array<std::byte, returns_layout::bytes> returns_storage_{};
  bool returns_ready_{false};
  std::uint16_t series_index_{0};
  std::uint16_t argc_index_{0};
};

// ============================================================================
// Factories and free functions.
// ============================================================================

// series_cat(a, b, ...): concatenates series in order and returns one new series.
// Argument stacks extend in stage order and so does the return buffer; results
// crossing a series boundary are fed to the next series' first stage by
// _bind_continue / drive.
//
// Type-list concatenation is delegated to the framework:
//   generator = meta_istream<exp_list<Series::binders_list...>>
//   meta_jostream flattens each exp_list<Binders...>
//   the last round's pack::to_t is exp_list<AllBinders...>
//   exp_list::to<function_series> yields the merged series type
//   no hand-written concat_lists / series_from_list any more

// A single series: returned unchanged.
template <class Series>
auto series_cat(Series series) {
  return series;
}

// Any number: all series pass as for_each extra arguments at once; the condition
// opens G_NOT_SKIP only on the last round, where the callback moves each
// series' stages into the merged buffer.
template <class First, class Second, class... Rest>
auto series_cat(First first, Second second, Rest... rest) {
  constexpr std::size_t count = 2 + sizeof...(Rest);

  using condition =
      meta_ios::stream_states_co<detail::last_round_condition<count>>;
  using generator = meta_iostream::meta_istream<
      exp_list<typename std::remove_cvref_t<First>::binders_list,
               typename std::remove_cvref_t<Second>::binders_list,
               typename std::remove_cvref_t<Rest>::binders_list...>>;
  using receiver = meta_iostream::meta_jostream<exp_list<>>;
  using loop = meta_states::run<condition, receiver, generator>;

  return loop::for_each(
      [](auto stage, auto&... series) {
// The last round's pack::to_t is exp_list<AllBinders...>.
        using combined =
            typename decltype(stage)::to_t::template to<function_series>;
        return combined::from_series(std::move(series)...);
      },
      first, second, rest...);
}

// link_f(f0, f1, ...): deduces each stage's signature with meta_bind::bind, then
// packs them into a series.
template <std::size_t Alignment = 0, class... F>
auto link_f(F&&... functions) {
  using series_type =
      function_series<detail::binder_for_t<Alignment, F>...>;
  return series_type{meta_bind::bind<Alignment>(std::forward<F>(functions))...};
}

// link_efb(b0, b1, ...): links ready-made binders directly.
template <class... Binders>
auto link_efb(Binders&&... binders) {
  return function_series<std::decay_t<Binders>...>{
      std::forward<Binders>(binders)...};
}

// series_bind(series, args...): feeds flat arguments into the stage stacks in
// stage order.
template <class Series, class... Args>
std::size_t series_bind(Series& series, Args&&... args) {
  return series.bind_flat(std::forward<Args>(args)...);
}

template <class Series, class... Args>
void series_initialize(Series& series, Args&&... args) {
  series.initialize(std::forward<Args>(args)...);
}

}  // namespace meta_bind

