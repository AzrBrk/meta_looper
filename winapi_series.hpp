#pragma once

// ============================================================================
// winapi_series.hpp —— 把 WinAPI 的窗口创建流程拆成可组合的 function_series
//
// 目标不是“再包一层 wrapper”，而是让零散的 WinAPI 调用变成可命名、可复用、
// 可拼接的 series：
//
//   auto create = winapi_series::window_series(config);   // 4 段：module ->
//                                                         //  register -> create
//                                                         //  -> show
//   create.bind(nullptr);          // GetModuleHandleA 的参数
//   create._bind_continue();       // 返回 HWND
//
//   auto loop = winapi_series::window_loop_series(config); // 5 段，最后跑消息循环
//   loop.bind(nullptr);
//   loop._bind_continue();         // 返回退出码
//
// 也可以自己组合（series_cat 在 meta_function_series.hpp）：
//
//   auto s = meta_bind::series_cat(
//       winapi_series::module_handle_series(),
//       winapi_series::register_class_series(config),
//       winapi_series::create_window_series(config),
//       winapi_series::show_window_series(config),
//       winapi_series::message_loop_series(config));
//
// 每段都是普通 binder：参数栈是 type_safe_aligned_range，返回值和 binder
// 自身都在连续缓冲里，没有 std::function / 链表 / 每槽 shared_ptr。
// 字符串用 char const*，所以 window_config 可以按值复制进每个 lambda，
// 不产生堆分配；字符串本身必须活得比 series 长（通常用字面量）。
// ============================================================================

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "meta_function_series.hpp"

namespace winapi_series {

using namespace meta_bind;

struct window_config {
  char const* class_name{"meta_looper_window"};
  char const* title{"meta_looper"};
  DWORD ex_style{0};
  DWORD style{WS_OVERLAPPEDWINDOW};
  int x{CW_USEDEFAULT};
  int y{CW_USEDEFAULT};
  int width{640};
  int height{480};
  HWND parent{nullptr};
  HMENU menu{nullptr};
  LPVOID param{nullptr};
  int show_command{SW_SHOW};
  WNDPROC window_proc{nullptr};  // null -> default_window_proc
  UINT timer_id{1};
  UINT timer_ms{50};
};

// 默认窗口过程：收到 WM_TIMER（演示用自动退出）或 WM_DESTROY 时投递
// WM_QUIT，这样 message_loop_series 不会永久阻塞。
inline LRESULT CALLBACK default_window_proc(HWND window, UINT message,
                                            WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_TIMER:
      KillTimer(window, static_cast<UINT_PTR>(wparam));
      PostQuitMessage(0);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcA(window, message, wparam, lparam);
  }
}

// LPCSTR -> HMODULE
inline auto module_handle_series() {
  return meta_bind::link_f(GetModuleHandleA);
}

// HMODULE -> HMODULE（注册窗口类，把 instance 原样传给下一段）
inline auto register_class_series(window_config config) {
  return meta_bind::link_f([config](HMODULE module) -> HMODULE {
    WNDCLASSA window_class{};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc =
        config.window_proc != nullptr ? config.window_proc : &default_window_proc;
    window_class.hInstance = reinterpret_cast<HINSTANCE>(module);
    window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    window_class.lpszClassName = config.class_name;

    if (RegisterClassA(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return nullptr;
    }
    return module;
  });
}

// HMODULE -> HWND
inline auto create_window_series(window_config config) {
  return meta_bind::link_f([config](HMODULE module) -> HWND {
    return CreateWindowExA(
        config.ex_style, config.class_name, config.title, config.style, config.x,
        config.y, config.width, config.height, config.parent, config.menu,
        reinterpret_cast<HINSTANCE>(module), config.param);
  });
}

// HWND -> HWND（ShowWindow + UpdateWindow，原样传递句柄）
inline auto show_window_series(window_config config) {
  return meta_bind::link_f([config](HWND window) -> HWND {
    if (window != nullptr) {
      ShowWindow(window, config.show_command);
      UpdateWindow(window);
    }
    return window;
  });
}

// HWND -> int（消息循环；timer 自动退出，然后销毁窗口并注销类）
inline auto message_loop_series(window_config config) {
  return meta_bind::link_f([config](HWND window) -> int {
    if (window == nullptr) return -1;

    SetTimer(window, config.timer_id, config.timer_ms, nullptr);
    MSG message{};
    while (GetMessageA(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageA(&message);
    }

    int const exit_code = static_cast<int>(message.wParam);
    DestroyWindow(window);
    UnregisterClassA(config.class_name, GetModuleHandleA(nullptr));
    return exit_code;
  });
}

// HWND -> HWND（只销毁，不跑消息循环）
inline auto destroy_window_series() {
  return meta_bind::link_f([](HWND window) -> HWND {
    if (window != nullptr) DestroyWindow(window);
    return window;
  });
}

// 常用组合：module -> register -> create -> show
inline auto window_series(window_config config) {
  return meta_bind::series_cat(
      module_handle_series(), register_class_series(config),
      create_window_series(config), show_window_series(config));
}

// 常用组合：window_series + 消息循环
inline auto window_loop_series(window_config config) {
  return meta_bind::series_cat(window_series(config),
                               message_loop_series(config));
}

}  // namespace winapi_series

#endif  // _WIN32
