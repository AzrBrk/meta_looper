// function_binder / function_series 基准：参数栈用 type_safe_aligned_range 之后，
// 绑定 + 调用的开销与对象大小，对比 select_type 那套“std::function + tuple +
// 每槽一个堆节点”的存储形状。
//
// 三种被测量的实现：
//
//   new     meta_bind::function_binder：参数直接排在 binder 内联的对齐缓冲里，
//           无堆分配；bind 是槽位号分派，调用用编译期下标展开。
//   erased  常见的“std::function + std::tuple 参数栈”写法：没有节点链，
//           但每次调用都要经过 std::function 的类型擦除间接层。
//   legacy  复刻旧库的存储形状：std::function + tuple + 每个槽位一个
//           make_shared 出来的节点（运行期下标 -> shared_ptr -> 虚函数写入）。
//           绑定/调用走指针链，构造时 N 次堆分配。
//
// 测量项：
//   S   sizeof（字节）——对象本身的形状
//   B   复用 binder：bind 3 个参数 + 调用，ns/次
//   C   每次重建 binder（构造 + bind + 调用），ns/次
//   T   编译期槽位 bind_at（new 独有，跳过槽位号分派）
//   F   series：3 段链式调用（新实现）对比手写直调
//
// 编译：
//   g++ -std=c++23 -O2 -falign-functions=64 -falign-loops=32 -I. \
//       -o bench_binder bench/function_binder_bench.cpp
//
// 对齐开关与 hetero_access_bench 相同：不固定函数/循环对齐时，同一份机器码
// 换个地址就能差 20%，表格不可读。

#include "meta_function_series.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace bench {

using clock_type = std::chrono::steady_clock;

template <class T>
inline void keep(T const& value) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "m"(value) : "memory");
#else
  (void)value;
#endif
}

// 交错测量：每轮把所有方案各跑一次，取每个方案的最好值。
// 不交错时，“先测的方案”会被更低的初始 CPU 频率/冷缓存拖慢；方案之间的
// 顺序差能达到 10% 以上，比这里想测的抽象开销还大。
struct measurement {
  char const* name;
  double (*run)(std::size_t);
  std::size_t bytes;
  double best_ns{1e300};
};

double measure_interleaved(measurement* rows, std::size_t count,
                           std::size_t iterations, int reps) {
  double sink = 0;
  for (int r = 0; r < reps; ++r) {
    for (int i = 0; i < static_cast<int>(count); ++i) {
      auto const t0 = clock_type::now();
      double const result = rows[i].run(iterations);
      auto const t1 = clock_type::now();
      sink += result;
      keep(result);
      double const ns =
          std::chrono::duration<double, std::nano>(t1 - t0).count() /
          static_cast<double>(iterations);
      if (ns < rows[i].best_ns) rows[i].best_ns = ns;
    }
  }
  return sink;
}

// ============================================================================
// 被绑定的函数（3 个参数，值语义，不产生堆分配）
// ============================================================================
double mix(int a, double b, int c) {
  return static_cast<double>(a) * 0.5 + b + static_cast<double>(c & 7);
}

using fn_type = double(int, double, int);
using std_function = std::function<fn_type>;

// ============================================================================
// erased：std::function + tuple 参数栈（运行期下标分派）
// ============================================================================
struct erased_mix {
  std_function fn;
  std::tuple<int, double, int> args;
  std::size_t cursor{0};

  explicit erased_mix(std_function f) : fn(std::move(f)) {}

  template <class T>
  void bind(T&& value) {
    assign<0>(cursor, std::forward<T>(value));
    ++cursor;
  }

  double operator()() {
    cursor = 0;
    return std::apply(
        [this](auto&... values) { return fn(values...); }, args);
  }

 private:
  template <std::size_t I, class T>
  void assign(std::size_t index, T&& value) {
    if (index == I) {
      std::get<I>(args) = std::forward<T>(value);
      return;
    }
    if constexpr (I + 1 < 3) {
      assign<I + 1>(index, std::forward<T>(value));
    }
  }
};

// ============================================================================
// legacy：std::function + tuple + 每槽一个 shared_ptr 节点
// （复刻 select_type 的 exp_node 存储形状：构造时 N 次 make_shared，
//   绑定沿节点链走，节点里存的是 tuple 槽位的地址）
// ============================================================================
struct legacy_node_base {
  virtual ~legacy_node_base() = default;
  virtual void assign(const void* source) = 0;
};

template <class T>
struct legacy_node final : legacy_node_base {
  T* slot;
  explicit legacy_node(T* target) : slot(target) {}
  void assign(const void* source) override {
    *slot = *static_cast<T const*>(source);
  }
};

struct legacy_mix {
  std_function fn;
  std::tuple<int, double, int> args;
  std::array<std::shared_ptr<legacy_node_base>, 3> nodes;
  std::size_t cursor{0};

  explicit legacy_mix(std_function f)
      : fn(std::move(f)),
        nodes{std::make_shared<legacy_node<int>>(&std::get<0>(args)),
              std::make_shared<legacy_node<double>>(&std::get<1>(args)),
              std::make_shared<legacy_node<int>>(&std::get<2>(args))} {}

  template <class T>
  void bind(T&& value) {
    nodes[cursor]->assign(static_cast<void const*>(std::addressof(value)));
    ++cursor;
  }

  double operator()() {
    cursor = 0;
    return std::apply(
        [this](auto&... values) { return fn(values...); }, args);
  }
};

// ============================================================================
// 各方案的执行函数
// ============================================================================
constexpr std::size_t iters = 1u << 20;

__attribute__((noinline)) double run_direct(std::size_t count) {
  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    acc += mix(static_cast<int>(i), static_cast<double>(i) * 0.25,
               static_cast<int>(i * 3));
  }
  return acc;
}

__attribute__((noinline)) double run_new(std::size_t count) {
  auto binder = meta_bind::bind(mix);
  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    binder.bind(static_cast<int>(i));
    binder.bind(static_cast<double>(i) * 0.25);
    binder.bind(static_cast<int>(i * 3));
    acc += binder();
  }
  return acc;
}

__attribute__((noinline)) double run_new_at(std::size_t count) {
  auto binder = meta_bind::bind(mix);
  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    binder.bind_at<0>(static_cast<int>(i));
    binder.bind_at<1>(static_cast<double>(i) * 0.25);
    binder.bind_at<2>(static_cast<int>(i * 3));
    acc += binder();
  }
  return acc;
}

__attribute__((noinline)) double run_erased(std::size_t count) {
  erased_mix binder{std_function{mix}};
  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    binder.bind(static_cast<int>(i));
    binder.bind(static_cast<double>(i) * 0.25);
    binder.bind(static_cast<int>(i * 3));
    acc += binder();
  }
  return acc;
}

__attribute__((noinline)) double run_legacy(std::size_t count) {
  legacy_mix binder{std_function{mix}};
  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    binder.bind(static_cast<int>(i));
    binder.bind(static_cast<double>(i) * 0.25);
    binder.bind(static_cast<int>(i * 3));
    acc += binder();
  }
  return acc;
}

// 每次重建：把构造开销也算进去。
__attribute__((noinline)) double run_new_per_call(std::size_t count) {
  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    auto binder = meta_bind::bind(mix);
    binder.bind(static_cast<int>(i));
    binder.bind(static_cast<double>(i) * 0.25);
    binder.bind(static_cast<int>(i * 3));
    acc += binder();
  }
  return acc;
}

__attribute__((noinline)) double run_erased_per_call(std::size_t count) {
  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    erased_mix binder{std_function{mix}};
    binder.bind(static_cast<int>(i));
    binder.bind(static_cast<double>(i) * 0.25);
    binder.bind(static_cast<int>(i * 3));
    acc += binder();
  }
  return acc;
}

__attribute__((noinline)) double run_legacy_per_call(std::size_t count) {
  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    legacy_mix binder{std_function{mix}};
    binder.bind(static_cast<int>(i));
    binder.bind(static_cast<double>(i) * 0.25);
    binder.bind(static_cast<int>(i * 3));
    acc += binder();
  }
  return acc;
}

// ============================================================================
// series：3 段链
// ============================================================================
__attribute__((noinline)) double run_series_direct(std::size_t count) {
  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    int const first = static_cast<int>(i) + 1;
    double const second = static_cast<double>(first) * 0.5;
    acc += second - 1.0;
  }
  return acc;
}

__attribute__((noinline)) double run_series(std::size_t count) {
  auto series = meta_bind::link_f(
      [](int value) { return value + 1; },
      [](int value) { return static_cast<double>(value) * 0.5; },
      [](double value) { return value - 1.0; });

  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    series.reset();
    series.bind(static_cast<int>(i));
    series._bind_continue();
    acc += series.final_return();
  }
  return acc;
}

// 同一套链，但让 looper 状态机一段一轮地驱动。
__attribute__((noinline)) double run_series_drive(std::size_t count) {
  auto series = meta_bind::link_f(
      [](int value) { return value + 1; },
      [](int value) { return static_cast<double>(value) * 0.5; },
      [](double value) { return value - 1.0; });

  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    series.reset();
    series.bind(static_cast<int>(i));
    series.drive();
    acc += series.final_return();
  }
  return acc;
}

// 只测 execute() 的 thunk 表：3 段参数预先绑好，循环 reset + 3 次 execute。
__attribute__((noinline)) double run_series_execute(std::size_t count) {
  auto series = meta_bind::link_f([](int value) { return value + 1; },
                                  [](int value) { return value * 2; },
                                  [](int value) { return value - 3; });
  meta_bind::series_bind(series, 1, 2, 3);

  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    series.reset();
    series.execute();
    series.execute();
    series.execute();
    acc += series.final_return();
  }
  return acc;
}

// 只测 _continue() 的编译期展开（参数都已绑好）。
__attribute__((noinline)) double run_series_continue(std::size_t count) {
  auto series = meta_bind::link_f([](int value) { return value + 1; },
                                  [](int value) { return value * 2; },
                                  [](int value) { return value - 3; });
  meta_bind::series_bind(series, 1, 2, 3);

  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    series.reset();
    series._continue();
    acc += series.final_return();
  }
  return acc;
}

// 运行期子链视图：从运行期下标 1 开始，thunk 表走到结尾。
__attribute__((noinline)) double run_series_view(std::size_t count) {
  auto series = meta_bind::link_f([](int value) { return value + 1; },
                                  [](int value) { return value * 2; },
                                  [](int value) { return value - 3; });
  meta_bind::series_bind(series, 1, 2, 3);

  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    series.reset();
    series[1]._continue();
    acc += series.final_return();
  }
  return acc;
}

// 编译期起点视图：series.at<1>()，展开成直接调用。
__attribute__((noinline)) double run_series_static_view(std::size_t count) {
  auto series = meta_bind::link_f([](int value) { return value + 1; },
                                  [](int value) { return value * 2; },
                                  [](int value) { return value - 3; });
  meta_bind::series_bind(series, 1, 2, 3);

  double acc = 0;
  for (int i = 0; i < static_cast<int>(count); ++i) {
    series.reset();
    series.template at<1>()._continue();
    acc += series.final_return();
  }
  return acc;
}

// ============================================================================
// 输出
// ============================================================================
void print_rows(measurement const* rows, std::size_t count, double floor) {
  std::printf("%-30s %10s %8s %9s\n", "scheme", "ns/op", "vs floor", "sizeof");
  for (int i = 0; i < static_cast<int>(count); ++i) {
    std::printf("%-30s %10.3f %8.2fx %9zu\n", rows[i].name, rows[i].best_ns,
                rows[i].best_ns / floor, rows[i].bytes);
  }
}

void size_table() {
  using new_t = decltype(meta_bind::bind(mix));
  using new_one = decltype(meta_bind::bind([](int) { return 1; }));
  using new_zero = decltype(meta_bind::bind([] { return 1; }));
  using new_string =
      decltype(meta_bind::bind([](int, double, std::string) { return 1; }));
  using series_t = decltype(meta_bind::link_f(
      [](int) { return 1; }, [](int) { return 2.0; }, [](double) { return 3; }));

  std::printf("\nsizeof (bytes)\n");
  std::printf("  new binder<int>                  %zu\n", sizeof(new_one));
  std::printf("  new binder<int,double,int>       %zu\n", sizeof(new_t));
  std::printf("  new binder<3 args, packed>       %zu\n",
              sizeof(decltype(meta_bind::bind<1>(mix))));
  std::printf("  new binder<int,double,string>    %zu\n", sizeof(new_string));
  std::printf("  new binder<0 args>               %zu\n", sizeof(new_zero));
  std::printf("  new series<3 stages>             %zu\n", sizeof(series_t));
  std::printf("  erased std::function+tuple       %zu\n", sizeof(erased_mix));
  std::printf("  legacy + 3 shared nodes          %zu\n", sizeof(legacy_mix));
  std::printf("  bare std::function               %zu\n",
              sizeof(std_function));
}

}  // namespace bench

int main() {
  using namespace bench;

  size_table();

  int const reps = 25;
  double sink = 0;

  measurement reuse[] = {
      {"direct call", run_direct, sizeof(std_function)},
      {"new binder (bind)", run_new,
       sizeof(decltype(meta_bind::bind(mix)))},
      {"new binder (bind_at<I>)", run_new_at,
       sizeof(decltype(meta_bind::bind(mix)))},
      {"erased std::function+tuple", run_erased, sizeof(erased_mix)},
      {"legacy shared_ptr nodes", run_legacy, sizeof(legacy_mix)},
  };
  sink += measure_interleaved(reuse, std::size(reuse), iters, reps);
  std::printf("\nreuse binder: bind(3 args) + call\n");
  print_rows(reuse, std::size(reuse), reuse[0].best_ns);

  measurement ctor[] = {
      {"new binder", run_new_per_call,
       sizeof(decltype(meta_bind::bind(mix)))},
      {"erased std::function+tuple", run_erased_per_call, sizeof(erased_mix)},
      {"legacy shared_ptr nodes", run_legacy_per_call, sizeof(legacy_mix)},
  };
  sink += measure_interleaved(ctor, std::size(ctor), iters, reps);
  std::printf("\nconstruct + bind(3 args) + call\n");
  print_rows(ctor, std::size(ctor), reuse[0].best_ns);

  using series_size = decltype(meta_bind::link_f(
      [](int) { return 1; }, [](int) { return 2.0; },
      [](double) { return 3; }));
  measurement series[] = {
      {"hand-written chain", run_series_direct, 0},
      {"function_series _continue", run_series_continue, sizeof(series_size)},
      {"function_series _bind_continue", run_series, sizeof(series_size)},
      {"function_series execute table", run_series_execute,
       sizeof(series_size)},
      {"series[1] view _continue", run_series_view, sizeof(series_size)},
      {"series.at<1>() _continue", run_series_static_view, sizeof(series_size)},
      {"function_series drive (looper)", run_series_drive,
       sizeof(series_size)},
  };
  sink += measure_interleaved(series, std::size(series), iters, reps);
  std::printf("\nseries: 3 stages (int -> double -> double)\n");
  print_rows(series, std::size(series), series[0].best_ns);

  std::printf("\nsink = %g\n", sink);
  return 0;
}

