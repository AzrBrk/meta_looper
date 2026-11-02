#include "meta_static_function_series.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

template <class Op>
bool rejects_uninitialized(Op&& operation) {
  try {
    operation();
  } catch (meta_bind::exception const& failure) {
    return failure.code() == meta_bind::error::not_fully_bound;
  }
  return false;
}

struct alignas(64) wide {
  int value{};
};

struct tracked {
  static inline int live{};
  static inline int built{};
  static inline int destroyed{};
  static inline int fail_at{-1};
  int value{};

  explicit tracked(int input = 0) : value(input) {
    if (built == fail_at) throw std::runtime_error("construction");
    ++built;
    ++live;
  }
  tracked(tracked const&) = delete;
  tracked(tracked&&) = delete;
  tracked& operator=(tracked const&) = delete;
  ~tracked() { --live; ++destroyed; }
};

struct cached {
  static inline int until_failure{-1};
  static inline int live{};
  int value{};
  cached() {
    if (until_failure == 0) throw std::runtime_error("cache construction");
    if (until_failure > 0) --until_failure;
    ++live;
  }
  cached(cached const& other) : value(other.value) { ++live; }
  cached& operator=(cached const&) = default;
  ~cached() { --live; }
};

struct non_default {
  explicit non_default(int input) : value(input) {}
  int value;
};

void assignment_and_initialization() {
  auto series = meta_bind::link_static(
      [](int first, std::string second, double third, int fourth) {
        return first + static_cast<int>(second.size()) + static_cast<int>(third) + fourth;
      });
  assert(rejects_uninitialized([&] { series.call<0>(); }));
  assert(rejects_uninitialized([&] { series.arguments(); }));
  assert(rejects_uninitialized([&] { series.argument<0>(); }));
  assert(rejects_uninitialized([&] { series.returns(); }));
  assert(rejects_uninitialized([&] { series.write(); }));
  assert(rejects_uninitialized([&] { series.execute(); }));
  assert(rejects_uninitialized([&] { series.execute_feed(); }));
  series.initialize(1, std::string("hello"), 2.0, 3);
  assert(series.call<0>() == 11);
  series.write<1>(std::string("x"), 4.0, 5);
  assert(series.call<0>() == 11);
  series.write(2, std::string("ab"), 3.0, 4);
  assert(series.call<0>() == 11);
  series.write<4>();
  auto& stable_string = series.argument<1>();
  series.write<1>(std::string("assigned"));
  assert(std::addressof(stable_string) == std::addressof(series.argument<1>()));
  auto [first, second, third, fourth] = series.arguments();
  second = "binding";
  assert(series.argument<1>() == "binding");
  assert(first == 2 && third == 3.0 && fourth == 4);
  bool duplicate_rejected{};
  try { series.initialize(); }
  catch (meta_bind::exception const& failure) {
    duplicate_rejected = failure.code() == meta_bind::error::argument_overflow;
  }
  assert(duplicate_rejected);

  auto partial = meta_bind::link_static(
      [](int, std::string text, int tail) { return text.size() + tail; });
  partial.initialize(7);
  assert(partial.argument<1>().empty() && partial.argument<2>() == 0);
  partial.write(4, std::string("odd"), 8);
  assert(partial.call<0>() == 11);

  auto full = meta_bind::link_static([](non_default value) { return value.value; });
  full.initialize(42);
  assert(full.call<0>() == 42);
}

void zero_and_one() {
  int calls{};
  auto zero = meta_bind::link_static([&] { ++calls; return 7; });
  zero.initialize();
  zero.write();
  zero.execute();
  zero.execute<1>();
  zero.execute_feed<1>();
  assert(calls == 1 && zero.call<0>() == 7);
  static_assert(decltype(zero)::total_args == 0);
  static_assert(!std::is_copy_constructible_v<decltype(zero)>);
  static_assert(!std::is_move_constructible_v<decltype(zero)>);
  static_assert(!std::is_copy_assignable_v<decltype(zero)>);
  static_assert(!std::is_move_assignable_v<decltype(zero)>);

  auto one = meta_bind::link_static([](std::string value) { return value; });
  one.initialize();
  assert(one.call<0>().empty());
  one.write(std::string("one"));
  assert(one.call<0>() == "one");
}

void padding_and_absolute_offsets() {
  using layout = meta_bind::static_series_detail::storage_layout<
      exp_utilities::exp_list<char, wide, char, double, std::string>>;
  using frame = meta_bind::static_series_detail::pair_frame<layout, 1,
      std::integral_constant<std::size_t, 1>>;
  static_assert(frame::main::value == layout::cursor<3>::value);
  static_assert(frame::branch::value == layout::cursor<4>::value);
  static_assert(layout::cursor<1>::value == 64);
  static_assert(layout::cursor<2>::value == 128);
  std::size_t visited{};
  meta_bind::static_series_detail::dispatch<layout, 1, 4>([&](auto cursor) {
    assert(decltype(cursor)::index == visited + 1);
    ++visited;
  });
  assert(visited == 4);

  auto series = meta_bind::link_static(
      [](char first, wide second, char third, double fourth, std::string fifth) {
        return first + second.value + third + static_cast<int>(fourth) +
               static_cast<int>(fifth.size());
      });
  series.initialize('a', wide{1}, 'b', 2.0, std::string("pad"));
  assert(reinterpret_cast<std::uintptr_t>(std::addressof(series.argument<1>())) % 64 == 0);
  series.write<1>(wide{8}, 'c', 4.0, std::string("tail"));
  assert(series.argument<0>() == 'a' && series.argument<1>().value == 8);
  assert(series.argument<2>() == 'c' && series.argument<3>() == 4.0);
  assert(series.argument<4>() == "tail");
  series.write<2>('d', 9.0, std::string("odd"));
  assert(series.argument<2>() == 'd' && series.argument<4>() == "odd");
}

void references_and_move_only() {
  int source = 3;
  std::string text = "owned";
  auto series = meta_bind::link_static(
      [](int& output, std::string const& copy, std::unique_ptr<int> input,
         std::string&& moved) {
        output += *input;
        std::string consumed = std::move(moved);
        return copy + consumed;
      });
  series.initialize(source, text, std::make_unique<int>(4), std::string("!"));
  text = "changed";
  assert(series.call<0>() == "owned!" && source == 7);
  assert(!series.argument<2>());
  int replacement = 10;
  series.write(std::ref(replacement), std::string("new"), std::make_unique<int>(2),
               std::string("?"));
  assert(series.call<0>() == "new?" && replacement == 12 && source == 7);
  assert(series.argument<0>() == std::addressof(replacement));

  auto move_callable = meta_bind::link_static(
      [owned = std::make_unique<int>(13)] { return *owned; });
  move_callable.initialize();
  assert(move_callable.call<0>() == 13);
}

void caches_and_execution() {
  int effects{};
  auto series = meta_bind::link_static(
      [](int value) { return value + 1; },
      [&](std::string const& value) { effects += static_cast<int>(value.size()); },
      [](double value) { return value * 2; });
  series.initialize(2, std::string("abc"), 1.5);
  auto [initial, placeholder, final] = series.returns();
  static_cast<void>(placeholder);
  assert(initial == 0 && final == 0.0);
  static_assert(std::is_same_v<decltype(series.call<0>()), int&>);
  static_assert(std::is_void_v<decltype(series.call<1>())>);
  int& cached_result = series.call<0>();
  series.write<0>(8);
  series.execute<1>();
  assert(cached_result == 3 && effects == 3 && final == 3.0);
  series.execute();
  assert(cached_result == 9 && effects == 6);
  assert(std::addressof(cached_result) == std::addressof(series.returns().get<0>()));
}

void feeding() {
  auto series = meta_bind::link_static(
      [](int value) { return value + 1; },
      [](int first, int second) { return first * second; },
      [](int value) { return std::to_string(value); });
  series.initialize(2, 0, 4, 0);
  series.execute_feed();
  auto [first, second, third] = series.returns();
  assert(first == 3 && second == 12 && third == "12");
  series.write<1>(5);
  series.execute_feed<1>();
  assert(first == 3 && second == 20 && third == "20");

  auto moved = meta_bind::link_static(
      [] { return std::make_unique<int>(17); },
      [](std::unique_ptr<int> value) { return *value; });
  moved.initialize();
  moved.execute_feed();
  assert(!moved.returns().get<0>() && moved.returns().get<1>() == 17);

  auto copied = meta_bind::link_static(
      [] { return std::string("preserved"); },
      [](std::string value) { return value.size(); });
  copied.initialize();
  copied.execute_feed();
  assert(copied.returns().get<0>() == "preserved");
  assert(copied.returns().get<1>() == 9);

  auto reference_feed = meta_bind::link_static(
      [] { return 5; }, [](int& value) { return ++value; });
  int unused{};
  reference_feed.initialize(unused);
  reference_feed.execute_feed();
  assert(reference_feed.returns().get<0>() == 6);
  assert(reference_feed.returns().get<1>() == 6 && unused == 0);

  int effects{};
  auto skipped = meta_bind::link_static(
      [&] { ++effects; }, [](int value) { return value; },
      [] { return std::string("zero"); }, [](std::string value) { return value; });
  skipped.initialize(8, std::string("original"));
  skipped.execute_feed();
  assert(effects == 1 && skipped.returns().get<1>() == 8);
  assert(skipped.returns().get<3>() == "zero");
}

void rollback() {
  tracked::live = tracked::built = tracked::destroyed = 0;
  tracked::fail_at = 2;
  {
    auto series = meta_bind::link_static(
        [](tracked const&, tracked const&, tracked const&, tracked const&) {});
    bool failed{};
    try { series.initialize(1, 2, 3, 4); }
    catch (std::runtime_error const&) { failed = true; }
    assert(failed && tracked::live == 0 && tracked::destroyed == 2);
    assert(rejects_uninitialized([&] { series.arguments(); }));
    tracked::fail_at = -1;
    series.initialize(1, 2, 3, 4);
    assert(tracked::live == 4 && tracked::built == 6);
    series.call<0>();
  }
  assert(tracked::live == 0 && tracked::destroyed == 6);

  {
    auto series = meta_bind::link_static([](tracked const&) { return cached{}; },
                                         [] { return cached{}; });
    cached::until_failure = 1;
    bool failed{};
    try { series.initialize(7); }
    catch (std::runtime_error const&) { failed = true; }
    assert(failed && tracked::live == 0 && cached::live == 0);
    cached::until_failure = -1;
    series.initialize(8);
    assert(tracked::live == 1 && cached::live == 2);
    series.execute();
  }
  assert(tracked::live == 0 && cached::live == 0);
}

}

int main() {
  assignment_and_initialization();
  zero_and_one();
  padding_and_absolute_offsets();
  references_and_move_only();
  caches_and_execution();
  feeding();
  rollback();
  std::cout << "static_series: PASS\n";
}