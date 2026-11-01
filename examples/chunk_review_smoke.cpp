#define META_STATES_LOOPER_BUDGET 2

#include "meta_ios.hpp"

#include <array>
#include <cassert>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace meta_stream_details {
template <>
inline constexpr std::size_t pipe_chunk_budget<1> = 2;
}

namespace chunk_review_tests {
using namespace meta_states;
using namespace meta_ios;
using namespace meta_iostream;

template <unsigned Mask, meta_states::u8 WindowId>
struct select_rounds {
  template <class Window, class Flags, class Object>
  using apply = std::conditional_t<
      Window::value == WindowId,
      std::conditional_t<
          ((Mask >> user_fn::index_of<Flags>()) & 1) != 0,
          yield_t, skip_t>, Flags>;
};

template <unsigned Mask>
using loop = meta_states::run<
    stream_states_co<select_rounds<Mask, pre_rec_exec>>, meta_iterator,
    index_sequence_istream<7>>;

template <unsigned Mask>
using pipe = meta_pipe<index_sequence_istream<7>>::run<
    meta_iterator, con<node_con<select_rounds<Mask, post_gen_exec>, true>>>;

consteval int last_round(unsigned mask, unsigned count) {
  int last = -1;
  for (unsigned index = 0; index < count && index < 7; ++index) {
    if ((mask >> index) & 1) {
      last = static_cast<int>(index);
    }
  }
  return last;
}

template <class Runner, unsigned Mask, std::size_t... Index>
constexpr bool check_forward(std::index_sequence<Index...>) {
  constexpr int expected = last_round(Mask, sizeof...(Index));
  auto callback = [](auto stage, auto value) {
    static_assert(decltype(value)::value == stage.index());
    return decltype(value)::value;
  };
  if constexpr (expected < 0) {
    static_assert(std::is_void_v<decltype(Runner::for_each_forward(
        callback, std::integral_constant<int, Index>{}...))>);
    Runner::for_each_forward(callback, std::integral_constant<int, Index>{}...);
    return true;
  } else {
    return Runner::for_each_forward(callback, std::integral_constant<int, Index>{}...) ==
           expected;
  }
}

template <class Runner, unsigned Mask, std::size_t... Count>
constexpr bool check_lengths(std::index_sequence<Count...>) {
  return (check_forward<Runner, Mask>(std::make_index_sequence<Count + 1>{}) &&
          ...);
}

template <class Runner, unsigned Mask>
constexpr bool check_each() {
  std::array<int, 7> seen{};
  unsigned calls = 0;
  bool same_arguments = true;
  int shared = 42;
  const char marker = 'x';
  Runner::for_each([&](auto stage, auto&& value, auto&& tag) {
    static_assert(std::is_same_v<decltype(value), int&>);
    static_assert(std::is_same_v<decltype(tag), const char&>);
    same_arguments = same_arguments && &value == &shared && &tag == &marker;
    seen[calls++] = static_cast<int>(stage.index());
  }, shared, marker);
  unsigned expected_calls = 0;
  for (unsigned index = 0; index < 7; ++index) {
    if ((Mask >> index) & 1) {
      if (seen[expected_calls++] != static_cast<int>(index)) {
        return false;
      }
    }
  }
  auto typed_callback = [](auto stage) {
    return std::integral_constant<std::size_t, stage.index()>{};
  };
  constexpr int expected = last_round(Mask, 7);
  if constexpr (expected < 0) {
    static_assert(std::is_void_v<decltype(Runner::for_each(typed_callback))>);
  } else {
    using result = decltype(Runner::for_each(typed_callback));
    static_assert(std::is_same_v<
                  result, std::integral_constant<std::size_t, expected>>);
    int storage = 42;
    int& reference = Runner::for_each(
        [&](auto) -> int& { return storage; });
    if (&reference != &storage) {
      return false;
    }
  }
  return same_arguments && calls == expected_calls;
}

template <unsigned Mask>
constexpr bool check_pattern() {
  return check_each<loop<Mask>, Mask>() && check_each<pipe<Mask>, Mask>() &&
         check_lengths<loop<Mask>, Mask>(std::make_index_sequence<9>{}) &&
         check_lengths<pipe<Mask>, Mask>(std::make_index_sequence<9>{});
}

static_assert(check_pattern<0>());
static_assert(check_pattern<1>());
static_assert(check_pattern<0b0000011>());
static_assert(check_pattern<0b1000000>());
static_assert(check_pattern<0b1000011>());
static_assert(check_pattern<0b1010101>());
static_assert(check_pattern<0b1111111>());

struct lvalue_callback {
  template <class Stage, class... Args>
  constexpr int operator()(Stage, Args&&...) & { return 42; }
};

constexpr bool check_lvalue_callback() {
  lvalue_callback callback;
  return loop<0b1111110>::for_each(callback) == 42 &&
         loop<0b1111110>::for_each_forward(callback, 0, 1, 2, 3) == 42;
}
static_assert(check_lvalue_callback());

// 临时对象 + operator() &：返回类型必须按实际调用的左值类别推导。
static_assert(loop<0b1111110>::for_each(lvalue_callback{}) == 42);
static_assert(loop<0b1111110>::for_each_forward(
                  lvalue_callback{}, 0, 1, 2, 3) == 42);

template<class Runner>
void check_forward_references() {
  int first = 17;
  const double second = 2.5;
  char third = 'x';
  std::string fourth = "fourth";
  std::integral_constant<long, 4> fifth;
  bool sixth = true;
  auto last = std::make_unique<int>(99);
  using arguments = std::tuple<int&, const double&, char&&, std::string&&,
      std::integral_constant<long, 4>&&, bool&&, std::unique_ptr<int>&&>;
  unsigned calls = 0;
  auto callback = [&](auto stage, auto&& value) -> decltype(auto) {
    using expected = decltype(std::get<stage.index()>(
        std::declval<arguments&&>()));
    static_assert(std::is_same_v<decltype(value), expected>);
    assert(calls++ == stage.index());
    return std::forward<decltype(value)>(value);
  };
  static_assert(std::is_same_v<decltype(Runner::for_each_forward(
      callback, first, second, 'x', std::string("fourth"),
      std::integral_constant<long, 4>{}, true, std::move(last))),
      std::unique_ptr<int>&&>);
  auto&& result = Runner::for_each_forward(
      callback, first, second, std::move(third), std::move(fourth),
      std::move(fifth), std::move(sixth), std::move(last));
  assert(calls == 7);
  assert(&result == &last);
  auto moved = std::move(result);
  assert(!last);
  assert(*moved == 99);
}
}

int main() {
  chunk_review_tests::check_forward_references<
      chunk_review_tests::loop<0b1111111>>();
  chunk_review_tests::check_forward_references<
      chunk_review_tests::pipe<0b1111111>>();
  return chunk_review_tests::check_pattern<0b1000011>() ? 0 : 1;
}