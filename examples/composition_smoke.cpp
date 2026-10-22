#include "meta_ios.hpp"

#include <array>
#include <tuple>
#include <type_traits>

namespace composition_tests {
using namespace meta_ios;
using namespace meta_iostream;
using namespace meta_objects;
using exp_utilities::exp_list;

struct twice {
  template<class T>
  using apply = std::integral_constant<std::size_t, T::value * 2>;
};

template<std::size_t Stride>
struct divisible {
  template<class T>
  using apply = std::bool_constant<(T::value % Stride == 0)>;
};

struct sum {
  template<class State, class Input>
  using apply = std::integral_constant<std::size_t, State::value + Input::value>;
};

template<std::size_t Rounds, std::size_t Stride, std::size_t Limit>
constexpr auto reference() {
  struct result {
    std::array<std::size_t, Rounds> values{};
    std::size_t size = 0;
    std::size_t total = 0;
  } out;
  for (std::size_t i = 0; i < Rounds && out.size < Limit; ++i) {
    if (i % Stride == 0) {
      out.values[out.size++] = i * 2;
      out.total += i * 2;
    }
  }
  return out;
}

template<class List, auto Expected>
struct matches;

template<class... Values, auto Expected>
struct matches<exp_list<Values...>, Expected> {
  static constexpr bool value = [] {
    constexpr std::array<std::size_t, sizeof...(Values)> actual{Values::value...};
    if (actual.size() != Expected.size) return false;
    for (std::size_t i = 0; i < actual.size(); ++i) {
      if (actual[i] != Expected.values[i]) return false;
    }
    return true;
  }();
};

template<class Source, std::size_t Rounds, std::size_t Stride, std::size_t Limit>
consteval bool check() {
  using pipeline = typename meta_pipe<Source>
      ::template all_to<meta_iterator, con<filter_con<divisible<Stride>, true>>>
      ::template all_to<transform<twice>>
      ::template all_to<take<Limit>, con<take_con<>>>
      ::template run<collect<>>;
  constexpr auto expected = reference<Rounds, Stride, Limit>();
  static_assert(matches<typename pipeline::type::to_t, expected>::value);
  std::size_t calls = 0;
  bool valid = true;
  pipeline::for_each([&]<class Stage>(Stage) {
    constexpr auto actual = exp_utilities::exp_size<typename Stage::to_t>;
    ++calls;
    valid = valid && actual == calls;
  });
  return valid && calls == expected.size;
}

template<std::size_t Rounds>
consteval bool matrix() {
  using source = index_sequence_istream<Rounds>;
  return check<source, Rounds, 1, 100>() &&
         check<source, Rounds, 2, 3>() &&
         check<source, Rounds, 3, 1>() &&
         check<source, Rounds, 100, 0>();
}

static_assert(matrix<0>());
static_assert(matrix<1>());
static_assert(matrix<7>());
static_assert(matrix<65>());
static_assert(check<meta_istream_list<
    std::integral_constant<int, 0>, std::integral_constant<long, 1>,
    std::integral_constant<short, 2>, std::integral_constant<unsigned, 3>>,
    4, 2, 2>());

using folded = meta_pipe<index_sequence_istream<7>>
    ::all_to<meta_iterator, con<filter_con<divisible<2>, true>>>
    ::all_to<transform<twice>>
    ::run<fold<std::integral_constant<std::size_t, 0>, sum>>;
static_assert(folded::type::to_t::value == reference<7, 2, 7>().total);

using flushed = meta_pipe<index_sequence_istream<5>>
    ::all_to<fold<std::integral_constant<std::size_t, 0>, sum>,
             con<wait_for_end_advance_con<>>>
    ::run<collect<>>;
static_assert(std::is_same_v<flushed::type::to_t,
                            exp_list<std::integral_constant<std::size_t, 10>>>);

struct add_pointer {
  template<class T>
  using apply = T*;
};
using heterogeneous = meta_pipe<meta_istream_list<int, const char, double>>
    ::all_to<transform<add_pointer>>
    ::all_to<take<2>, con<take_con<>>>
    ::run<collect<>>;
static_assert(std::is_same_v<heterogeneous::type::to_t,
                            exp_list<int*, const char*>>);

using source_take = meta_pipe<index_sequence_istream<10>>
    ::all_to<take<2>, con<take_con<true>>>
    ::run<collect<>>;
static_assert(std::is_same_v<source_take::type::to_t,
    exp_list<std::integral_constant<std::size_t, 0>,
             std::integral_constant<std::size_t, 1>>>);
static_assert(meta_produce_t<source_take::type::from>::value == 1);

using final_take = meta_pipe<index_sequence_istream<10>>
    ::run<take<2>, con<take_con<true>>>;
static_assert([] {
  std::size_t calls = 0;
  final_take::for_each([&](auto) { ++calls; });
  return calls == 2;
}());

using infinite_take = meta_pipe<meta_index_istream<0>>
    ::all_to<take<3, collect<std::tuple<>>>, con<take_con<true>>>
    ::run<meta_iterator>;
static_assert(std::tuple_size_v<infinite_take::type::to_t> == 3);

using no_match = meta_pipe<index_sequence_istream<7>>
    ::all_to<meta_iterator, con<filter_con<divisible<100>, true>>>
    ::all_to<take<3>, con<take_con<>>>
    ::run<collect<>>;
static_assert(exp_utilities::exp_size<no_match::type::to_t> == 1);

struct invalid_predicate {
  template<class T>
  using apply = typename T::invalid_inactive_branch;
};
using empty_filter = meta_pipe<index_sequence_istream<0>>
    ::run<meta_iterator, con<filter_con<invalid_predicate, true>>>;
static_assert(std::is_same_v<empty_filter::type::to_t,
                            exp_utilities::literal_types::no_exist_type>);
}  // namespace composition_tests

int main() {}
