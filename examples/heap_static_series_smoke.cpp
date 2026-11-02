#include "meta_static_function_series.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace allocation_probe {
inline std::size_t allocations{};
inline std::size_t releases{};
inline bool fail{};
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
  if (std::exchange(allocation_probe::fail, false)) throw std::bad_alloc{};
  auto boundary = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
  void* memory = _aligned_malloc(bytes, boundary);
#else
  void* memory = std::aligned_alloc(boundary,
      ((bytes + boundary - 1) / boundary) * boundary);
#endif
  if (!memory) throw std::bad_alloc{};
  ++allocation_probe::allocations;
  return memory;
}

void operator delete(void* memory, std::align_val_t) noexcept {
  if (!memory) return;
  ++allocation_probe::releases;
#if defined(_WIN32)
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}

void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept {
  ::operator delete(memory, alignment);
}

namespace {

template <class Op>
void rejects(Op&& operation) {
  bool rejected{};
  try { operation(); }
  catch (meta_bind::exception const& failure) {
    rejected = failure.code() == meta_bind::error::not_fully_bound;
  }
  assert(rejected);
}

template <class Op>
void throws_construction(Op&& operation) {
  bool rejected{};
  try { operation(); }
  catch (std::runtime_error const&) { rejected = true; }
  assert(rejected);
}

struct alignas(64) wide {
  int value{};
};

struct alignas(128) wider {
  int value{};
};

struct alignas(128) large_callable {
  std::array<std::byte, 8192> capture{};
  std::uintptr_t* address{};
  wider operator()(char first, wide second, wider third) {
    *address = reinterpret_cast<std::uintptr_t>(this);
    return wider{first + second.value + third.value};
  }
};

struct counted {
  static inline int live{};
  static inline int built{};
  static inline int destroyed{};
  static inline int remaining{-1};
  int value{};
  explicit counted(int input = 0) : value(input) {
    if (remaining == 0) throw std::runtime_error("slot construction");
    if (remaining > 0) --remaining;
    ++live;
    ++built;
  }
  counted(counted const&) = delete;
  counted(counted&&) = delete;
  ~counted() { --live; ++destroyed; }
};

struct cache {
  static inline int live{};
  static inline int remaining{-1};
  cache() {
    if (remaining == 0) throw std::runtime_error("cache construction");
    if (remaining > 0) --remaining;
    ++live;
  }
  cache(cache const&) { ++live; }
  cache& operator=(cache const&) = default;
  ~cache() { --live; }
};

template <class F>
struct alignas(128) observed_callable {
  using imported_signature = meta_bind::static_series_detail::callable_info<F>;
  F function;
  template <class... Args>
  decltype(auto) operator()(Args&&... args) {
    return std::invoke(function, std::forward<Args>(args)...);
  }
};

template <class F>
auto observed(F function) { return observed_callable<F>{std::move(function)}; }

struct alignas(128) owned_stage {
  static inline int live{};
  static inline int destroyed{};
  owned_stage() { ++live; }
  owned_stage(owned_stage const&) { ++live; }
  owned_stage(owned_stage&&) { ++live; }
  ~owned_stage() { --live; ++destroyed; }
  cache operator()(counted const&) { return cache{}; }
};

struct alignas(128) throwing_callable {
  static inline int live{};
  static inline int moves{};
  static inline int remaining{-1};
  throwing_callable() { ++live; }
  throwing_callable(throwing_callable const&) = delete;
  throwing_callable(throwing_callable&&) {
    if (remaining == 0) throw std::runtime_error("callable construction");
    if (remaining > 0) --remaining;
    ++live;
    ++moves;
  }
  ~throwing_callable() { --live; }
  int operator()() { return 7; }
};

void layout_and_owner_size() {
  std::uintptr_t callable_address{};
  auto before = allocation_probe::allocations;
  {
    auto series = meta_bind::link_static_heap(large_callable{{}, &callable_address});
    using owner = decltype(series);
    using small_owner = decltype(meta_bind::link_static_heap([] { return 1; }));
    static_assert(sizeof(owner) == sizeof(small_owner));
    static_assert(sizeof(owner) <= 5 * sizeof(void*));
    static_assert(std::is_nothrow_move_constructible_v<owner>);
    static_assert(std::is_nothrow_move_assignable_v<owner>);
    static_assert(!std::is_copy_constructible_v<owner>);
    static_assert(!std::is_copy_assignable_v<owner>);
    series.initialize('a', wide{2}, wider{3});
    auto args = series.arguments();
    auto result = series.returns();
    assert(series.call<0>().value == 'a' + 5);
    using namespace meta_bind::static_series_detail;
    using args_layout = storage_layout<exp_utilities::exp_list<char, wide, wider>>;
    using results_layout = storage_layout<exp_utilities::exp_list<wider>>;
    using functions_layout = storage_layout<exp_utilities::exp_list<large_callable>>;
    using storage = heap_storage<args_layout, results_layout, functions_layout>;
    using arena_layout = storage::layout;
    auto arg_address = reinterpret_cast<std::uintptr_t>(&args.get<0>());
    auto result_address = reinterpret_cast<std::uintptr_t>(&result.get<0>());
    assert(callable_address % 128 == 0);
    assert(reinterpret_cast<std::uintptr_t>(&args.get<1>()) % 64 == 0);
    assert(reinterpret_cast<std::uintptr_t>(&args.get<2>()) % 128 == 0);
    assert(result_address % 128 == 0);
    assert(arg_address == callable_address + arena_layout::cursor<1>::value);
    assert(result_address == callable_address + arena_layout::cursor<2>::value);
    assert(callable_address < arg_address && arg_address < result_address);
    assert(reinterpret_cast<std::uintptr_t>(&args.get<1>()) ==
        arg_address + args_layout::cursor<1>::value);
    assert(reinterpret_cast<std::uintptr_t>(&args.get<2>()) ==
        arg_address + args_layout::cursor<2>::value);
    assert(allocation_probe::allocations == before + 1);
    std::cout << "heap owner=" << sizeof(owner)
              << " arena=" << sizeof(storage::arena)
              << " alignment=" << alignof(storage::arena)
              << " regions=" << arena_layout::cursor<0>::value << ','
              << arena_layout::cursor<1>::value << ','
              << arena_layout::cursor<2>::value << '\n';
  }
  assert(allocation_probe::allocations == allocation_probe::releases);
}

void references_and_moves() {
  auto producer = [] { return 8; };
  auto consumer = [](int& value) { return ++value; };
  auto source = meta_bind::link_static_heap(producer, consumer);
  int initial{};
  source.initialize(initial);
  source.execute_feed();
  auto args = source.arguments();
  auto results = source.returns();
  auto* cached = &results.get<0>();
  assert(args.get<0>() == cached && *cached == 9);
  auto destination = std::move(source);
  assert(&destination.returns().get<0>() == cached);
  assert(destination.argument<0>() == cached);
  assert(destination.call<1>() == 10 && *cached == 10);
  rejects([&] { source.initialize(initial); });
  rejects([&] { source.arguments(); });
  rejects([&] { source.argument<0>(); });
  rejects([&] { source.returns(); });
  rejects([&] { source.write(); });
  rejects([&] { source.call<0>(); });
  rejects([&] { source.execute<2>(); });
  rejects([&] { source.execute_feed<2>(); });
  source = std::move(destination);
  assert(source.argument<0>() == cached && source.call<1>() == 11);
  auto* same = &source;
  source = std::move(*same);
  assert(&source.returns().get<0>() == cached && source.call<1>() == 12);
  auto empty = meta_bind::link_static_heap(producer, consumer);
  auto uninitialized = std::move(empty);
  rejects([&] { empty.initialize(initial); });
  uninitialized.initialize(initial);
  uninitialized = std::move(source);
  assert(&uninitialized.returns().get<0>() == cached);
}

void assignment_cleanup() {
  owned_stage callable;
  {
    auto source = meta_bind::link_static_heap(callable);
    auto target = meta_bind::link_static_heap(callable);
    source.initialize(3);
    target.initialize(9);
    auto* address = &source.argument<0>();
    int old_destroyed = counted::destroyed;
    int old_callable_destroyed = owned_stage::destroyed;
    assert(cache::live == 2 && owned_stage::live == 3);
    target = std::move(source);
    assert(counted::destroyed == old_destroyed + 1 && counted::live == 1);
    assert(owned_stage::destroyed == old_callable_destroyed + 1);
    assert(cache::live == 1 && owned_stage::live == 2);
    assert(&target.argument<0>() == address && address->value == 3);
    auto empty = std::move(source);
    target = std::move(empty);
    assert(counted::live == 0);
    assert(cache::live == 0 && owned_stage::live == 1);
    rejects([&] { target.initialize(4); });
  }
  assert(counted::live == 0 && counted::built == counted::destroyed);
}

void rollback_and_retry() {
  {
    auto series = meta_bind::link_static_heap(
      observed([](counted const&, counted const&) { return 1; }));
    counted::remaining = 1;
    throws_construction([&] { series.initialize(2, 3); });
    assert(counted::live == 0);
    rejects([&] { series.execute(); });
    counted::remaining = -1;
    series.initialize(2, 3);
    assert(counted::live == 2 && series.call<0>() == 1);
  }
  assert(counted::live == 0 && counted::built == counted::destroyed);
  {
    auto series = meta_bind::link_static_heap(
      observed([](counted const&) { return cache{}; }), [] { return cache{}; });
    cache::remaining = 1;
    throws_construction([&] { series.initialize(5); });
    assert(cache::live == 0 && counted::live == 0);
    cache::remaining = -1;
    series.initialize(6);
    assert(cache::live == 2 && counted::live == 1);
    series.execute();
  }
  assert(cache::live == 0 && counted::live == 0);
  auto before = allocation_probe::allocations;
  {
    throwing_callable first;
    throwing_callable second;
    throwing_callable::remaining = 1;
    throws_construction([&] {
      auto series = meta_bind::link_static_heap(std::move(first), std::move(second));
      static_cast<void>(series);
    });
    assert(throwing_callable::live == 2);
    assert(allocation_probe::allocations == before + 1);
    assert(allocation_probe::allocations == allocation_probe::releases);
    throwing_callable::remaining = -1;
    auto series = meta_bind::link_static_heap(std::move(first), std::move(second));
    series.initialize();
    int moves = throwing_callable::moves;
    auto next = std::move(series);
    assert(throwing_callable::moves == moves && next.call<0>() == 7);
  }
  assert(throwing_callable::live == 0);
  allocation_probe::fail = true;
  bool allocation_failed{};
  try {
    auto series = meta_bind::link_static_heap(throwing_callable{});
    static_cast<void>(series);
  } catch (std::bad_alloc const&) { allocation_failed = true; }
  assert(allocation_failed && throwing_callable::live == 0);
  assert(allocation_probe::allocations == allocation_probe::releases);
}

void move_only_and_bridge() {
  auto source = meta_bind::link_static_heap(
      [owned = std::make_unique<int>(13)] { return std::make_unique<int>(*owned); },
      [](std::unique_ptr<int> input) { return *input; });
  source.initialize();
  auto next = std::move(source);
  next.execute_feed();
  assert(!next.returns().get<0>() && next.returns().get<1>() == 13);
  auto binder = meta_bind::bind_as<int(int)>([](auto value) { return value + 1; });
  binder.bind_a_lot(4);
  auto imported = meta_bind::link_static_heap_binders(binder);
  auto moved = std::move(imported);
  assert(moved.call<0>() == 5);
  rejects([&] { imported.execute(); });
  auto move_binder = meta_bind::bind_as<int(std::unique_ptr<int>)>(
      [offset = std::make_unique<int>(2)](auto value) { return *offset + *value; });
  move_binder.bind_a_lot(std::make_unique<int>(6));
  auto move_import = meta_bind::link_static_heap_binders(std::move(move_binder));
  assert(move_import.call<0>() == 8);
}

}

int main() {
  layout_and_owner_size();
  references_and_moves();
  assignment_cleanup();
  rollback_and_retry();
  move_only_and_bridge();
  assert(owned_stage::live == 0);
  assert(allocation_probe::allocations == allocation_probe::releases);
  std::cout << "heap_static_series: PASS (over-aligned arenas "
            << allocation_probe::allocations << '/' << allocation_probe::releases << ")\n";
}