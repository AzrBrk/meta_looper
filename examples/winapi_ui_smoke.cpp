// 用 function_binder 组件拼 UI 的 smoke 测试。
//
// 组件全部带默认值，用户只 set() 自己关心的槽位，然后 link：
//   register_class -> create_window -> show_window -> message_loop

#include "winapi_ui.hpp"

#include <cstdio>

#if defined(_WIN32)

int main() {
  bool ok = true;

  // ---- 1) 手工组件 + 自定义值 ----
  {
    auto create = winapi_ui::create_window();
    create.set(winapi_ui::create_arg::title, "component window");
    create[winapi_ui::create_arg::width] = 300;  // enum operator[]
    create.set(winapi_ui::create_arg::height, 180);

    auto show = winapi_ui::show_window();
    show.set(winapi_ui::show_arg::command, winapi_ui::show_command::hide);

    auto loop = winapi_ui::message_loop();
    loop.set(winapi_ui::loop_arg::timer_ms, 20u);  // 测试自动退出

    auto series = winapi_ui::link(winapi_ui::register_class(), create, show,
                                  loop);
    static_assert(decltype(series)::stage_count == 4);

    series._bind_continue();
    int const exit_code = series.final_return();
    ok = ok && exit_code == 0;
    ok = ok && series._return<0>() != nullptr;  // register -> HMODULE
    ok = ok && series._return<1>() != nullptr;  // create -> HWND
    ok = ok && series._return<2>() != nullptr;  // show -> HWND
    ok = ok && series._return<3>() == 0;        // message loop exit

    std::printf("components: exit=%d hwnd=%p stages=%zu sizeof=%zu\n",
                exit_code, static_cast<void*>(series._return<1>()),
                decltype(series)::stage_count, sizeof(series));
  }

  // ---- 2) window() 默认四段，只改想要的值 ----
  {
    auto series = winapi_ui::window();
    constexpr auto create_stage =
        static_cast<std::size_t>(winapi_ui::window_stage::create);
    constexpr auto show_stage =
        static_cast<std::size_t>(winapi_ui::window_stage::show);
    constexpr auto loop_stage =
        static_cast<std::size_t>(winapi_ui::window_stage::message_loop);

    series.stage<create_stage>().set(winapi_ui::create_arg::title,
                                     "window() defaults");
    series.stage<create_stage>().set(winapi_ui::create_arg::width, 260);
    series.stage<show_stage>().set(winapi_ui::show_arg::command,
                                   winapi_ui::show_command::hide);
    series.stage<loop_stage>().set(winapi_ui::loop_arg::timer_ms, 20u);

    series._bind_continue();
    ok = ok && series.final_return() == 0;
    std::printf("window(): exit=%d hwnd=%p sizeof=%zu\n",
                series.final_return(),
                static_cast<void*>(series._return<1>()), sizeof(series));
  }

  // ---- 3) 不带消息循环：register -> create -> show ----
  {
    auto series = winapi_ui::window_without_loop();
    constexpr auto show_stage = 2;
    series.stage<show_stage>().set(winapi_ui::show_arg::command,
                                   winapi_ui::show_command::hide);
    series._bind_continue();

    HWND window = series.final_return();
    ok = ok && window != nullptr && IsWindow(window) != 0;
    std::printf("window_without_loop: hwnd=%p sizeof=%zu\n",
                static_cast<void*>(window), sizeof(series));

    if (window != nullptr) {
      DestroyWindow(window);
      UnregisterClassA("meta_looper_ui", GetModuleHandleA(nullptr));
    }
  }

  return ok ? 0 : 1;
}

#else

int main() {
  std::printf("winapi_ui_smoke: skipped (not a Windows build)\n");
  return 0;
}

#endif

