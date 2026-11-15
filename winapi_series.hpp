#pragma once

// ============================================================================
// winapi_series.hpp: the WinAPI window-creation flow as composable function
// series.
//
// The point is not another wrapper layer, but named, reusable, concatenable
// series of raw WinAPI calls:
//
//   auto create = winapi_series::window_series(config);   // 4 stages:
//                                                         // module -> register
//                                                         // -> create -> show
//   create.bind(nullptr);          // argument of GetModuleHandleA
//   create._bind_continue();       // returns HWND
//
//   auto loop = winapi_series::window_loop_series(config); // 5 stages; the
//                                                          // last runs the loop
//   loop.bind(nullptr);
//   loop._bind_continue();         // returns the exit code
//
// Stages can also be combined by hand (series_cat lives in
// meta_function_series.hpp):
//
//   auto s = meta_bind::series_cat(
//       winapi_series::module_handle_series(),
//       winapi_series::register_class_series(config),
//       winapi_series::create_window_series(config),
//       winapi_series::show_window_series(config),
//       winapi_series::message_loop_series(config));
//
// Every stage is an ordinary binder: the argument stack is a
// type_safe_aligned_range and both the return value and the binder itself live
// in contiguous buffers - no std::function, no linked list, no per-slot
// shared_ptr. Strings are char const*, so window_config can be copied by value
// into each lambda without heap allocation; the strings themselves must
// outlive the series (string literals are the usual choice).
// Design notes: README section "winapi_series.hpp".
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

// Default window procedure: posts WM_QUIT on WM_TIMER (auto-exit for demos)
// or on WM_DESTROY, so message_loop_series can never block forever.
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

// HMODULE -> HMODULE: registers the window class, forwards the instance.
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

// HWND -> HWND: ShowWindow + UpdateWindow, handle passed through.
inline auto show_window_series(window_config config) {
  return meta_bind::link_f([config](HWND window) -> HWND {
    if (window != nullptr) {
      ShowWindow(window, config.show_command);
      UpdateWindow(window);
    }
    return window;
  });
}

// HWND -> int: message loop; the timer ends it, then the window is destroyed
// and the class is unregistered.
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

// HWND -> HWND: destroy only, no message loop.
inline auto destroy_window_series() {
  return meta_bind::link_f([](HWND window) -> HWND {
    if (window != nullptr) DestroyWindow(window);
    return window;
  });
}

// Common composition: module -> register -> create -> show.
inline auto window_series(window_config config) {
  return meta_bind::series_cat(
      module_handle_series(), register_class_series(config),
      create_window_series(config), show_window_series(config));
}

// Common composition: window_series followed by the message loop.
inline auto window_loop_series(window_config config) {
  return meta_bind::series_cat(window_series(config),
                               message_loop_series(config));
}

}  // namespace winapi_series

#endif  // _WIN32
