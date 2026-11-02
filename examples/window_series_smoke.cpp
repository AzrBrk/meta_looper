// 把 WinAPI 的窗口创建流程做成可组合的 function_series 的 smoke 测试。
//
//   window_series      : module -> register -> create -> show          (4 段)
//   window_loop_series : window_series + message_loop                  (5 段)
//
// 消息循环用一个 20~50ms 的 timer 自动投递 WM_QUIT，所以测试不会阻塞。
// 非 Windows 构建直接 skipped/PASS。

#include "winapi_series.hpp"

#include <cstdio>

#if defined(_WIN32)

int main() {
  bool ok = true;

  // ---- 1) 4 段：创建 + show，不跑消息循环 ----
  {
    winapi_series::window_config config{};
    config.class_name = "meta_looper_series_window";
    config.title = "meta_looper series";
    config.width = 320;
    config.height = 200;
    config.show_command = SW_HIDE;

    auto series = winapi_series::window_series(config);
    static_assert(decltype(series)::stage_count == 4);
    static_assert(decltype(series)::total_args == 4);  // 每段一个槽位

    series.bind(nullptr);  // GetModuleHandleA 的参数
    series._bind_continue();

    HWND window = series.final_return();
    ok = ok && window != nullptr && IsWindow(window) != 0;
    std::printf("window_series: hwnd=%p stages=%zu sizeof=%zu\n",
                static_cast<void*>(window), decltype(series)::stage_count,
                sizeof(series));

    if (window != nullptr) {
      DestroyWindow(window);
      UnregisterClassA(config.class_name, GetModuleHandleA(nullptr));
    }
  }

  // ---- 2) 5 段：window_series + 消息循环（timer 自动退出）----
  {
    winapi_series::window_config config{};
    config.class_name = "meta_looper_series_loop";
    config.title = "meta_looper loop";
    config.width = 240;
    config.height = 160;
    config.show_command = SW_HIDE;
    config.timer_ms = 20;

    auto series = winapi_series::window_loop_series(config);
    static_assert(decltype(series)::stage_count == 5);

    series.bind(nullptr);
    series._bind_continue();

    int const exit_code = series.final_return();
    ok = ok && exit_code == 0;
    ok = ok && series._return<0>() != nullptr;  // GetModuleHandleA
    ok = ok && series._return<2>() != nullptr;  // CreateWindowExA
    ok = ok && series._return<3>() != nullptr;  // ShowWindow 原样传递
    ok = ok && series._return<4>() == 0;        // 消息循环退出码

    std::printf("window_loop_series: exit=%d hwnd=%p stages=%zu sizeof=%zu\n",
                exit_code, static_cast<void*>(series._return<2>()),
                decltype(series)::stage_count, sizeof(series));
  }

  // ---- 3) 手工 series_cat 组合：和 window_loop_series 等价 ----
  {
    winapi_series::window_config config{};
    config.class_name = "meta_looper_series_cat";
    config.title = "meta_looper cat";
    config.width = 200;
    config.height = 120;
    config.show_command = SW_HIDE;
    config.timer_ms = 20;

    auto series = meta_bind::series_cat(
        winapi_series::module_handle_series(),
        winapi_series::register_class_series(config),
        winapi_series::create_window_series(config),
        winapi_series::show_window_series(config),
        winapi_series::message_loop_series(config));
    static_assert(decltype(series)::stage_count == 5);

    series.bind(nullptr);
    series._bind_continue();
    ok = ok && series.final_return() == 0;
    std::printf("series_cat combination: ok (sizeof=%zu)\n", sizeof(series));
  }

  return ok ? 0 : 1;
}

#else

int main() {
  std::printf("window_series_smoke: skipped (not a Windows build)\n");
  return 0;
}

#endif
