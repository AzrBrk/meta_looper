#pragma once

#include "meta_function_series.hpp"

namespace meta_bind {

namespace shared_details {

template <class F>
using callable_info = typename decltype([] {
  if constexpr (std::is_pointer_v<F>)
    return std::type_identity<detail::function_pointer_traits<F>>{};
  else
    return std::type_identity<exp_function_info<F>>{};
}())::type;

template <class... F>
inline constexpr auto starts = [] {
  std::array<std::size_t, sizeof...(F) + 1> result{};
  std::array counts{std::size_t{exp_size<typename callable_info<F>::argument_types>}...};
  for (std::size_t index = 0; index < counts.size(); ++index)
    result[index + 1] = result[index] + counts[index];
  return result;
}();

template <class... F>
using parameter_list = typename meta_states::run<
    meta_ios::stream_states_co<>, meta_iostream::meta_jostream<exp_list<>>,
    meta_iostream::meta_istream_list<typename callable_info<F>::argument_types...>>
    ::type::to_t;

}

template <class Range, std::size_t Begin, class F, class Parameters,
          std::size_t StageId = 0>
class shared_function_binder;

template <class Range, std::size_t Begin, class F, class... Parameters,
          std::size_t StageId>
class shared_function_binder<Range, Begin, F, exp_list<Parameters...>, StageId> {
 public:
  using range_type = Range;
  static constexpr std::size_t argument_begin = Begin;
  static constexpr std::size_t argument_end = Begin + sizeof...(Parameters);

  explicit shared_function_binder(F callable) : callable_(std::move(callable)) {}

  decltype(auto) operator()(Range range) {
    return invoke(range, std::index_sequence_for<Parameters...>{});
  }

 private:
  template <std::size_t... Index>
  decltype(auto) invoke([[maybe_unused]] Range range, std::index_sequence<Index...>) {
    return std::invoke(callable_, detail::call_argument<Parameters>(
        range.template get<Begin + Index>())...);
  }

  [[no_unique_address]] F callable_;
};

namespace shared_details {

template <class Parameters, class Sequence, class... F>
class series;

template <class... Parameters, std::size_t... Stage, class... F>
class series<exp_list<Parameters...>, std::index_sequence<Stage...>, F...>
    : private shared_function_binder<
          typename detail::aligned_slot_layout<0, ::select_if_t<
              sizeof...(Parameters) == 0,
              ::always_fn<exp_list<detail::no_argument_slot>>,
              ::always_fn<exp_list<detail::slot_type_t<Parameters>...>>>>::range_type,
          starts<F...>[Stage], F, typename callable_info<F>::argument_types, Stage>... {
  using slots = ::select_if_t<sizeof...(Parameters) == 0,
      ::always_fn<exp_list<detail::no_argument_slot>>,
      ::always_fn<exp_list<detail::slot_type_t<Parameters>...>>>;
  using layout = detail::aligned_slot_layout<0, slots>;

 public:
  using range_type = typename layout::range_type;
  static constexpr std::size_t total_args = sizeof...(Parameters);
  static constexpr std::size_t stage_count = sizeof...(F);
  static constexpr std::size_t stack_bytes = total_args == 0 ? 0 : layout::bytes;

  template <std::size_t Index>
  using binder_type = shared_function_binder<range_type, starts<F...>[Index],
      exp_select<Index, exp_list<F...>>,
      typename callable_info<exp_select<Index, exp_list<F...>>>::argument_types, Index>;

  explicit series(F... callable)
      : shared_function_binder<range_type, starts<F...>[Stage], F,
        typename callable_info<F>::argument_types, Stage>(std::move(callable))... {}

  series(series const&) = delete;
  series(series&&) = delete;
  series& operator=(series const&) = delete;
  series& operator=(series&&) = delete;

  ~series() { destroy(std::make_index_sequence<total_args>{}); }

  template <class... Args>
    requires(sizeof...(Args) == total_args)
  void initialize(Args&&... args) {
    if (initialized_) throw exception{error::argument_overflow};
    try {
      initialize_slots(std::make_index_sequence<total_args>{},
                       std::forward<Args>(args)...);
      initialized_ = true;
    } catch (...) {
      destroy(std::make_index_sequence<total_args>{});
      throw;
    }
  }

  range_type arguments() & {
    if (!initialized_) throw exception{error::not_fully_bound};
    return range_type{storage_.data()};
  }

  template <std::size_t Index>
    requires(Index < stage_count)
  decltype(auto) call() {
    auto range = arguments();
    return static_cast<binder_type<Index>&>(*this)(range);
  }

 private:
  template <std::size_t Index, class Arg>
  void initialize_slot(Arg&& arg) {
    using parameter = exp_select<Index, exp_list<Parameters...>>;
    using cursor = typename layout::template cursor<Index>;
    using slot = typename cursor::type;
    static_assert(detail::slot_bindable_v<parameter, Arg&&>);
    if constexpr (detail::is_ref_slot_v<parameter>) {
      ::new (static_cast<void*>(cursor{}.address_of(storage_.data())))
          slot(detail::slot_address_from<parameter>(std::forward<Arg>(arg)));
    } else {
      ::new (static_cast<void*>(cursor{}.address_of(storage_.data())))
          slot(std::forward<Arg>(arg));
    }
    ++constructed_;
  }

  template <std::size_t... Index, class... Args>
  void initialize_slots(std::index_sequence<Index...>, Args&&... args) {
    (initialize_slot<Index>(std::forward<Args>(args)), ...);
  }

  template <std::size_t... Index>
  void destroy(std::index_sequence<Index...>) noexcept {
    ([&] {
      constexpr auto reverse = total_args - 1 - Index;
      if (reverse < constructed_) {
        using cursor = typename layout::template cursor<reverse>;
        cursor{}.destroy(storage_.data());
      }
    }(), ...);
    constructed_ = 0;
  }

  alignas(layout::alignment) std::array<std::byte, layout::bytes> storage_;
  std::size_t constructed_{};
  bool initialized_{};
};

}

template <class... F>
  requires(sizeof...(F) > 0)
auto link_shared(F&&... callable) {
  return shared_details::series<
      shared_details::parameter_list<std::decay_t<F>...>,
      std::index_sequence_for<F...>, std::decay_t<F>...>(
          std::forward<F>(callable)...);
}

}