#include "meta_function_series.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

using namespace meta_bind;

namespace series_tests {

template <class F>
bool throws_error(F&& call, error expected) {
  try {
    std::forward<F>(call)();
  } catch (exception const& e) {
    return e.code() == expected;
  } catch (...) {
    return false;
  }
  return false;
}

// 具名函数对象：拷贝/移动测试需要“同一个 series 类型”。
struct plus_one {
  int operator()(int value) const { return value + 1; }
};

struct stringify {
  std::string operator()(int value) const { return std::to_string(value); }
};

bool run() {
  bool ok = true;

  // ---- README 的三段式：series_bind + _continue + 结构化绑定 ----
  {
    auto series = meta_bind::link_f(
        [](int a, double b) { return b - a; },
        [](double c) { return std::to_string(c); },
        []() { return 123; });

    static_assert(decltype(series)::stage_count == 3);
    static_assert(decltype(series)::return_count == 3);
    static_assert(decltype(series)::total_args == 3);
    ok = ok && series.size() == 3 && series.series_index() == 0;

    meta_bind::series_bind(series, 1, 1.33, 0.33);
    ok = ok && series.argc_index() == 3;
    series._continue();
    ok = ok && series.series_index() == 3;

    // 返回值现在存在 aligned_range 里：结构化绑定直接可用。
    auto [first, second, third] = series._return();
    ok = ok && (first - 0.33 < 1e-9) && (0.33 - first < 1e-9);
    ok = ok && second == "0.330000";
    ok = ok && third == 123;

    // 编译期下标 / 运行期下标两种入口。
    ok = ok && series._return<2>() == 123;
    ok = ok && series._return().get<1>() == "0.330000";
    auto proxy = series._return()[2];
    ok = ok && static_cast<int&>(proxy) == 123;
    ok = ok && series.final_return() == 123;

    std::cout << "readme flow: ok\n";
  }

  // ---- _bind_continue：上一段返回值自动进入下一段 ----
  {
    auto series = meta_bind::link_f(
        [](int a) { return a * 2; },
        [](int value) { return std::to_string(value); },
        [](std::string const& text) { return text.size(); });

    series.bind(21);
    series._bind_continue();
    ok = ok && series.final_return() == 2;  // "42" 的长度
    ok = ok && series._return<0>() == 42;
    ok = ok && series._return<1>() == "42";

    std::cout << "bind_continue: ok\n";
  }

  // ---- 只能移动的返回值：unique_ptr 自动 move 到下一段 ----
  {
    auto series = meta_bind::link_f(
        []() { return std::make_unique<int>(41); },
        [](std::unique_ptr<int> value) { return *value + 1; });
    series.bind();
    series._bind_continue();
    ok = ok && series.final_return() == 42;
    // 上一段返回值已经被 move 进下一段，原槽位留空。
    ok = ok && series._return<0>() == nullptr;
    std::cout << "move-only return: ok\n";
  }

  // ---- 中间段返回 void 时跳过自动绑定 ----
  {
    int order[3]{};
    auto series = meta_bind::link_f(
        [&](int a) {
          order[0] = a;
          return a + 1;
        },
        [&](int value) { order[1] = value; },
        [&]() {
          order[2] = 1;
          return 99;
        });

    series.bind(1);
    series._bind_continue();
    ok = ok && order[0] == 1 && order[1] == 2 && order[2] == 1;
    ok = ok && series.final_return() == 99;
    // void 段在返回 range 里占位 nullptr
    ok = ok && series._return<1>() == nullptr;

    std::cout << "void middle stage: ok\n";
  }

  // ---- 只给第一段绑参数时，_bind_continue 会跳过第一段 ----
  {
    auto series = meta_bind::link_f(
        [](int a) { return a; }, [](int value) { return value + 1; });
    series.bind(10);
    series._bind_continue();
    ok = ok && series.final_return() == 11;
    std::cout << "first-stage-only bind: ok\n";
  }

  // ---- 段级 bind / execute / operator[] ----
  {
    auto series = meta_bind::link_f(
        [](int a) { return a; }, [](int a, int b) { return a + b; });

    series.bind(5);  // 当前段是 0
    series.execute();
    ok = ok && series.series_index() == 1;

    series.select(1);  // 旧式游标：只设置当前段
    series.bind(6);
    series.bind(7);
    series.execute();

    ok = ok && series._return<0>() == 5 && series._return<1>() == 13;
    std::cout << "stage-local bind/execute: ok\n";
  }

  // ---- _stop_at：只跑到指定段 ----
  {
    int calls[3]{};
    auto series = meta_bind::link_f(
        [&](int) { calls[0] = 1; }, [&](int) { calls[1] = 1; },
        [&](int) { calls[2] = 1; });
    meta_bind::series_bind(series, 1, 2, 3);
    series._stop_at(1);
    ok = ok && calls[0] == 1 && calls[1] == 1 && calls[2] == 0;
    ok = ok && series.series_index() == 2;
    series._continue();
    ok = ok && calls[2] == 1;
    std::cout << "stop_at: ok\n";
  }

  // ---- series[I] 是游标 setter：从指定下标开始 _continue ----
  {
    auto series = meta_bind::link_f([](int a) { return a + 1; },
                                    [](int b) { return b * 2; },
                                    [](int c) { return c * 3; });
    meta_bind::series_bind(series, 1, 2, 3);

    series[1]._continue();  // 只跑第 1、2 段
    ok = ok && series.series_index() == 3;
    ok = ok && series._return<0>() == 0;  // 第 0 段没跑，保持默认值
    ok = ok && series._return<1>() == 4 && series._return<2>() == 9;

    auto from_two = meta_bind::link_f([](int a) { return a + 1; },
                                      [](int b) { return b * 2; },
                                      [](int c) { return c * 3; });
    meta_bind::series_bind(from_two, 1, 2, 3);
    from_two[2]._continue();  // 只跑第 2 段
    ok = ok && from_two._return<0>() == 0 && from_two._return<1>() == 0 &&
         from_two._return<2>() == 9;

    // 运行期下标同样可以设游标
    std::size_t start = 1;
    from_two.reset();
    from_two[start]._continue();
    ok = ok && from_two._return<1>() == 4 && from_two._return<2>() == 9;

    // end 游标：_continue 是 no-op（与旧库的 series[size()] 行为一致）
    from_two.reset();
    from_two[from_two.size()]._continue();
    ok = ok && from_two.series_index() == 3;
    ok = ok && from_two._return<2>() == 9;

    std::cout << "continue from index: ok\n";
  }

  // ---- 子链视图：_bind_continue / at<I>（编译期起点）----
  {
    auto series = meta_bind::link_f([](int a) { return a + 1; },
                                    [](int b) { return b * 2; },
                                    [](int c) { return c - 3; });
    series.bind(5);
    series.execute();          // 段 0 → 6
    series[1]._bind_continue();  // 段 1 吃 6 → 12，段 2 吃 12 → 9
    ok = ok && series.final_return() == 9;

    auto static_view = meta_bind::link_f([](int a) { return a + 1; },
                                         [](int b) { return b * 2; },
                                         [](int c) { return c * 3; });
    meta_bind::series_bind(static_view, 1, 2, 3);
    static_view.template at<1>()._continue();  // 只跑段 1、2，编译期展开
    ok = ok && static_view._return<0>() == 0;
    ok = ok && static_view._return<1>() == 4 && static_view._return<2>() == 9;

    std::cout << "sub-series view: ok\n";
  }

  // ---- for_each：返回值 / 额外参数 / stop / skip / forward ----
  {
    auto series = meta_bind::link_f([](int a) { return a + 1; },
                                    [](int b) { return b * 2; },
                                    [](int c) { return c * 3; });
    meta_bind::series_bind(series, 1, 2, 3);

    int calls = 0;
    int seen[3]{};
    auto result = series.for_each(
        [&](auto stage, int* count) {
          constexpr std::size_t index = stage.index();
          seen[index] = static_cast<int>(index) + 1;
          ++*count;
          return static_cast<int>(index) + 100;
        },
        &calls);
    ok = ok && result == 102 && calls == 3;
    ok = ok && seen[0] == 1 && seen[1] == 2 && seen[2] == 3;

    auto stop_series = meta_bind::link_f([](int a) { return a + 1; },
                                         [](int b) { return b * 2; },
                                         [](int c) { return c * 3; });
    meta_bind::series_bind(stop_series, 1, 2, 3);
    auto stopped = stop_series.for_each([](auto stage) {
      if constexpr (stage.index() == 1) {
        return meta_states::stop_t{};
      } else {
        return 0;
      }
    });
    static_assert(std::is_same_v<decltype(stopped), meta_states::stop_t>);
    ok = ok && stop_series.series_index() == 2;  // 第 2 段没有执行
    ok = ok && stop_series._return<2>() == 0;

    auto skip_series = meta_bind::link_f([](int a) { return a + 1; },
                                         [](int b) { return b * 2; },
                                         [](int c) { return c * 3; });
    meta_bind::series_bind(skip_series, 1, 2, 3);
    auto skipped = skip_series.for_each([](auto stage) {
      if constexpr (stage.index() == 1) {
        return meta_states::skip_t{};
      } else {
        return 7;
      }
    });
    static_assert(std::is_same_v<decltype(skipped), int>);
    ok = ok && skipped == 7;  // skip 不终止，最后一个观察值是 int

    auto forward_series = meta_bind::link_f([](int a) { return a + 1; },
                                            [](int b) { return b * 2; },
                                            [](int c) { return c * 3; });
    meta_bind::series_bind(forward_series, 1, 2, 3);
    int args_seen[3]{};
    auto forwarded = forward_series.for_each_forward(
        [&](auto stage, int value) {
          constexpr std::size_t index = stage.index();
          args_seen[index] = value;
          return value + static_cast<int>(index);
        },
        10, 20, 30);
    ok = ok && forwarded == 32;
    ok = ok && args_seen[0] == 10 && args_seen[1] == 20 && args_seen[2] == 30;

    std::cout << "for_each values/args/decisions: ok\n";
  }

  // ---- 扁平绑定可以分段：从 argc_at 的槽位继续 ----
  {
    auto series = meta_bind::link_f(
        [](int a) { return a; }, [](int b) { return b * 10; });
    meta_bind::series_bind(series, 5);
    ok = ok && series.argc_index() == 1;
    meta_bind::series_bind(series, 6);
    series._continue();
    ok = ok && series._return<0>() == 5 && series._return<1>() == 60;

    // 也可以从一段的中间继续绑定
    auto split = meta_bind::link_f(
        [](int a, int b) { return a + b; }, [](int c) { return c; });
    meta_bind::series_bind(split, 1);
    meta_bind::series_bind(split, 2, 3);
    split._continue();
    ok = ok && split._return<0>() == 3 && split._return<1>() == 3;

    std::cout << "incremental flat bind: ok\n";
  }

  // ---- reset 之后可以重新绑定、重新执行 ----
  {
    auto series = meta_bind::link_f(
        [](int a) { return a; }, [](int b) { return b; });
    meta_bind::series_bind(series, 1, 2);
    series._continue();
    ok = ok && series._return<1>() == 2;

    series.reset();
    ok = ok && series.series_index() == 0 && series.argc_index() == 0;
    meta_bind::series_bind(series, 7, 8);
    series._continue();
    ok = ok && series._return<0>() == 7 && series._return<1>() == 8;

    std::cout << "rebind after reset: ok\n";
  }

  // ---- link_efb：链接已经绑定好的 binder ----
  {
    auto first = meta_bind::bind([](int v) { return v + 1; });
    auto second = meta_bind::bind([](int v) { return v * 2; });
    first.bind(1);
    second.bind(9);
    auto series = meta_bind::link_efb(std::move(first), std::move(second));
    series._continue();
    ok = ok && series._return<0>() == 2 && series._return<1>() == 18;
    std::cout << "link_efb: ok\n";
  }

  // ---- drive：同样的链，用 looper 状态机一段一轮地跑 ----
  {
    auto fast = meta_bind::link_f(
        [](int a) { return a + 1; }, [](int a) { return a * 3; },
        [](int a) { return a - 4; });
    fast.bind(5);
    fast._bind_continue();

    auto looped = meta_bind::link_f(
        [](int a) { return a + 1; }, [](int a) { return a * 3; },
        [](int a) { return a - 4; });
    looped.bind(5);
    looped.drive();

    ok = ok && looped._return<0>() == fast._return<0>();
    ok = ok && looped._return<1>() == fast._return<1>();
    ok = ok && looped._return<2>() == fast._return<2>();
    ok = ok && looped.series_index() == 3;

    // 已经执行过段 0 时，drive 只跑剩下的段。
    auto partial = meta_bind::link_f(
        [](int a) { return a + 1; }, [](int a) { return a * 3; },
        [](int a) { return a - 4; });
    partial.bind(5);
    partial.execute();
    partial.drive();
    ok = ok && partial._return<2>() == 14;  // ((5+1)*3)-4
    ok = ok && partial.series_index() == 3;

    std::cout << "drive (looper): ok\n";
  }

  // ---- run_loop：重复执行，每次拿到本次的返回 range ----
  {
    auto series = meta_bind::link_f(
        [](int a) { return a * 2; }, [](int a) { return a + 1; });
    series.bind(10);

    std::size_t iterations = 0;
    int seen_first = 0;
    int seen_second = 0;
    series.run_loop(4, [&](auto results) {
      ++iterations;
      seen_first = results.template get<0>();
      seen_second = results.template get<1>();
    });
    ok = ok && iterations == 4 && seen_first == 20 && seen_second == 21;
    ok = ok && series.series_index() == 2;

    std::cout << "run_loop: ok\n";
  }

  // ---- 拷贝 / 移动：返回缓冲随 series 一起搬 ----
  {
    auto series = meta_bind::link_f(plus_one{}, stringify{});
    series.bind(4);
    series._bind_continue();

    auto copied = series;
    ok = ok && copied._return<0>() == 5 && copied._return<1>() == "5";

    auto moved = std::move(copied);
    ok = ok && moved._return<0>() == 5 && moved._return<1>() == "5";

    auto assigned = meta_bind::link_f(plus_one{}, stringify{});
    assigned = moved;
    ok = ok && assigned._return<0>() == 5 && assigned._return<1>() == "5";

    auto move_assigned = meta_bind::link_f(plus_one{}, stringify{});
    move_assigned = std::move(assigned);
    ok = ok && move_assigned._return<0>() == 5 &&
         move_assigned._return<1>() == "5";

    std::cout << "copy/move series: ok\n";
  }

  // ---- series_cat 任意条数 ----
  {
    auto six = meta_bind::series_cat(
        meta_bind::link_f([](int v) { return v + 1; }),
        meta_bind::link_f([](int v) { return v * 2; }),
        meta_bind::link_f([](int v) { return v + 3; }),
        meta_bind::link_f([](int v) { return v * 4; }),
        meta_bind::link_f([](int v) { return v - 5; }),
        meta_bind::link_f([](int v) { return v + 6; }));
    static_assert(decltype(six)::stage_count == 6);
    six.bind(1);
    six._bind_continue();
    ok = ok && six.final_return() == 29;
    ok = ok && six._return<0>() == 2 && six._return<5>() == 29;
    std::cout << "series_cat 6 pieces: ok (sizeof=" << sizeof(six) << ")\n";
  }

  // ---- 错误路径 ----
  {
    auto series = meta_bind::link_f([](int a, int b) { return a + b; });
    series.bind(1);
    ok = ok && throws_error([&] { series._continue(); },
                            error::not_fully_bound);

    auto extra = meta_bind::link_f([](int a) { return a; }, []() {});
    ok = ok && throws_error([&] { meta_bind::series_bind(extra, 1, 2); },
                            error::argument_overflow);

    ok = ok && throws_error([&] { extra[9]; }, error::index_exceed);
    ok = ok && throws_error([&] { extra.argc_at(99); },
                            error::argument_overflow);

    std::cout << "error paths: ok\n";
  }

  return ok;
}

}  // namespace series_tests

int main() {
  bool ok = series_tests::run();
  std::cout << (ok ? "PASS" : "FAIL") << '\n';
  return ok ? 0 : 1;
}
