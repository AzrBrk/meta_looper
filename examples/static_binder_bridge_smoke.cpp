#include "meta_static_function_series.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>

struct copy_only {
  int value = 5;
  copy_only() = default;
  copy_only(copy_only const&) = default;
  copy_only(copy_only&&) = delete;
};

struct import_probe {
  static inline int live = 0;
  static inline int copy_budget = -1;
  std::string text;
  explicit import_probe(std::string value) : text(std::move(value)) { ++live; }
  import_probe(import_probe const& other) : text(other.text) {
    if (copy_budget == 0) throw std::runtime_error("import copy");
    if (copy_budget > 0) --copy_budget;
    ++live;
  }
  import_probe(import_probe&& other) noexcept : text(std::move(other.text)) { ++live; }
  ~import_probe() { --live; }
};

int main() {
  auto first = meta_bind::bind_as<std::string(std::string const&)>(
      [](auto const& text) { return text + "!"; });
  first.bind(std::string("default"));
  auto second = meta_bind::bind_as<std::size_t(std::string, int)>(
      [](auto text, int suffix) { return text.size() + suffix; });
  second.bind_a_lot(std::string("unused"), 4);
  second.as_front_feed();
  auto series = meta_bind::link_static_binders(std::as_const(first), second);
  static_assert(decltype(series)::total_args == 3);
  assert(series.argument<0>() == "default");
  series.execute_feed();
  assert(series.returns().get<1>() == 12);
  assert(first.live_arguments().get<0>() == "default");
  assert(second.live_arguments().get<0>() == "unused");
  series.write<0>(std::string("new"));
  series.execute_feed();
  assert(series.returns().get<1>() == 8);

  int output = 7;
  auto reference = meta_bind::bind_as<int(int&, std::string&&)>(
      [](int& value, std::string&& text) { value += text.size(); return value; });
  reference.bind_a_lot(output, std::string("abc"));
  auto refs = meta_bind::link_static_binders(reference);
  assert(refs.argument<0>() == &output);
  assert(refs.call<0>() == 10 && output == 10);
  assert(reference.live_arguments().get<1>() == "abc");
    static_assert(std::is_same_v<decltype(std::as_const(reference).
      live_arguments().get<0>()), int* const&>);

    copy_only original;
    auto copy_parameter = meta_bind::bind_as<int(copy_only)>(
      [](copy_only value) { return value.value; });
    copy_parameter.bind(original);
    auto copies = meta_bind::link_static_binders(copy_parameter);
    assert(copies.call<0>() == 5);

    auto producer = meta_bind::bind_as<int()>([] { return 12; });
    auto ref_consumer = meta_bind::bind_as<int(int&)>([](int& value) { return ++value; });
    ref_consumer.bind(output);
    ref_consumer.as_front_feed();
    auto ref_feed = meta_bind::link_static_binders(producer, std::as_const(ref_consumer));
    ref_feed.execute_feed();
    assert(ref_feed.argument<0>() == &ref_feed.returns().get<0>());
    assert(ref_feed.returns().get<0>() == 13 && output == 10);

  auto owned = meta_bind::bind_as<int(std::unique_ptr<int>)>(
      [state = std::make_unique<int>(2)](std::unique_ptr<int> input) {
        return *state + *input;
      });
  owned.bind(std::make_unique<int>(9));
  auto moved = meta_bind::link_static_binders(std::move(owned));
  assert(!owned.live_arguments().get<0>());
  assert(moved.call<0>() == 11);

  auto consumer = second;
  consumer.as_front_feed(false);
  bool rejected = false;
  try { auto invalid = meta_bind::link_static_binders(first, consumer); }
  catch (std::invalid_argument const&) { rejected = true; }
  assert(rejected);
  auto incomplete = meta_bind::bind_as<void(int)>([](int) {});
  rejected = false;
  auto intact = meta_bind::bind_as<int(std::unique_ptr<int>)>(
      [](std::unique_ptr<int> value) { return *value; });
  intact.bind(std::make_unique<int>(23));
  try {
    auto invalid = meta_bind::link_static_binders(std::move(intact), incomplete);
  } catch (meta_bind::exception const& failure) {
    rejected = failure.code() == meta_bind::error::not_fully_bound;
  }
  assert(rejected && *intact.live_arguments().get<0>() == 23);
  auto append = meta_bind::bind_as<int(int)>([](int value) { return value; });
  append.bind(0);
  rejected = false;
  try { auto invalid = meta_bind::link_static_binders(std::move(intact), append); }
  catch (std::invalid_argument const&) { rejected = true; }
  assert(rejected && *intact.live_arguments().get<0>() == 23);

  {
    auto throwing = meta_bind::bind_as<void(import_probe const&, import_probe const&)>(
        [](import_probe const&, import_probe const&) {});
    throwing.bind_a_lot(import_probe("first"), import_probe("second"));
    assert(import_probe::live == 2);
    import_probe::copy_budget = 1;
    rejected = false;
    try { auto invalid = meta_bind::link_static_binders(throwing); }
    catch (std::runtime_error const&) { rejected = true; }
    assert(rejected && import_probe::live == 2);
    assert(throwing.live_arguments().get<0>().text == "first");
    assert(throwing.live_arguments().get<1>().text == "second");
    import_probe::copy_budget = -1;
    {
      auto retried = meta_bind::link_static_binders(throwing);
      assert(import_probe::live == 4);
      retried.execute_feed();
    }
    assert(import_probe::live == 2);
  }
  assert(import_probe::live == 0);

  int effects = 0;
  auto effect = meta_bind::bind_as<void()>([&] { ++effects; });
  auto independent = meta_bind::bind_as<int(int)>([](int value) { return value; });
  independent.bind(8);
  auto zero = meta_bind::bind_as<std::string()>([] { return std::string("zero"); });
  auto tail = second;
  auto skipped = meta_bind::link_static_binders(effect, independent, zero, tail);
  skipped.execute_feed();
  assert(effects == 1 && skipped.returns().get<1>() == 8);
  assert(skipped.returns().get<3>() == 8);
  auto empty = meta_bind::link_static_binders(std::as_const(effect), producer);
  empty.execute_feed();
  assert(effects == 2 && empty.returns().get<1>() == 12);
  std::cout << "static_binder_bridge_smoke: PASS\n";
}