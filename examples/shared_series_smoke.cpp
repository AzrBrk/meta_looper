#include "meta_shared_series.hpp"

#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <stdexcept>

struct tracked {
  static inline int alive{};
  explicit tracked(int value) {
    if (value < 0) throw std::runtime_error("construction");
    ++alive;
  }
  tracked(tracked const&) { ++alive; }
  tracked(tracked&&) noexcept { ++alive; }
  ~tracked() { --alive; }
};

struct alignas(64) aligned_value { int value; };

struct immovable {
  int value;
  explicit immovable(int input) : value(input) {}
  immovable(immovable const&) = delete;
  immovable(immovable&&) = delete;
};

int answer() noexcept { return 42; }

int main() {
  bool ok = true;
  int target = 3;
  auto series = meta_bind::link_shared(
      [](int first, double second) { return first + second; },
      [](std::string const& text, int& value) { value += int(text.size()); },
      [] { return 42; },
      [](std::unique_ptr<int> value) { return *value; },
      [](aligned_value value) { return value.value; });
  using chain = decltype(series);
  static_assert(chain::total_args == 6);
  static_assert(chain::binder_type<1>::argument_begin == 2);
  static_assert(chain::binder_type<2>::argument_begin == 4);
  static_assert(chain::binder_type<2>::argument_end == 4);
  try { series.call<0>(); ok = false; }
  catch (meta_bind::exception const& error) {
    ok = ok && error.code() == meta_bind::error::not_fully_bound;
  }
  meta_bind::series_initialize(series, 21, 0.5, "abcd", target,
                               std::make_unique<int>(9), aligned_value{17});
  ok = ok && series.call<0>() == 21.5;
  series.call<1>();
  ok = ok && target == 7 && series.call<2>() == 42;
  ok = ok && series.call<3>() == 9 && series.call<4>() == 17;
  auto range = series.arguments();
  ok = ok && reinterpret_cast<std::uintptr_t>(
      std::addressof(range.get<5>())) % alignof(aligned_value) == 0;
  range.get<0>() = 7;
  ok = ok && series.call<0>() == 7.5;
  ok = ok && range.get<4>() == nullptr;
  try {
    meta_bind::series_initialize(series, 1, 2.0, "bad", target,
                                 std::make_unique<int>(1), aligned_value{1});
    ok = false;
  } catch (meta_bind::exception const&) {}
  {
    auto rollback = meta_bind::link_shared([](tracked const&, tracked const&) {});
    try { meta_bind::series_initialize(rollback, 1, -1); ok = false; }
    catch (std::runtime_error const&) {}
    ok = ok && tracked::alive == 0;
    meta_bind::series_initialize(rollback, 1, 2);
    ok = ok && tracked::alive == 2;
    rollback.call<0>();
  }
  ok = ok && tracked::alive == 0;
  auto empty = meta_bind::link_shared([] { return 8; });
  meta_bind::series_initialize(empty);
  ok = ok && empty.call<0>() == 8;
  auto duplicate = meta_bind::link_shared(answer, answer);
  meta_bind::series_initialize(duplicate);
  ok = ok && duplicate.call<0>() == 42 && duplicate.call<1>() == 42;
  auto original = meta_bind::link_f([](int first, double second) {
    return first + second;
  }, answer, [](std::string const& text) { return text.size(); });
  meta_bind::series_initialize(original, 21, 0.5, "abcd");
  original._continue();
  ok = ok && original._return<0>() == 21.5 && original.final_return() == 4;
  try {
    meta_bind::series_initialize(original, 1, 0.0, "bad");
    ok = false;
  } catch (meta_bind::exception const&) {}
  {
    auto rollback = meta_bind::link_f([](tracked const&) {},
                                      [](tracked const&) {});
    try { meta_bind::series_initialize(rollback, 1, -1); ok = false; }
    catch (std::runtime_error const&) {}
    ok = ok && tracked::alive == 0;
    meta_bind::series_initialize(rollback, 1, 2);
    rollback._continue();
  }
  ok = ok && tracked::alive == 0;
  auto partial = meta_bind::link_f([](int first, int second) {
    return first + second;
  });
  partial.stage<0>().bind_at<0>(9);
  try {
    meta_bind::series_initialize(partial, 1, 2);
    ok = false;
  } catch (meta_bind::exception const& error) {
    ok = ok && error.code() == meta_bind::error::argument_overflow;
  }
  ok = ok && partial.stage<0>().bound_count() == 1;
  partial.stage<0>().bind_at<1>(3);
  partial._continue();
  ok = ok && partial.final_return() == 12;
  auto reference = meta_bind::link_f([](int& value) { ++value; });
  meta_bind::series_initialize(reference, std::ref(target));
  reference._continue();
  ok = ok && target == 8;
  auto fixed = meta_bind::link_shared([](immovable const& value) {
    return value.value;
  });
  meta_bind::series_initialize(fixed, 19);
  ok = ok && fixed.call<0>() == 19;
  std::printf("shared_series_smoke: %s bytes=%zu\n", ok ? "PASS" : "FAIL",
              chain::stack_bytes);
  return ok ? 0 : 1;
}