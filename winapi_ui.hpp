#pragma once

// ============================================================================
// winapi_ui.hpp —— 用 function_binder 组件拼一个最小 UI 库
//
// 每个组件都是普通 function_binder，构造时把所有参数绑成默认值；用户只需要
// 用 enum 槽位改自己关心的值，然后 link 起来：
//
//   auto create = winapi_ui::create_window();
//   create.set(winapi_ui::create_arg::title, "My Window");
//   create.set(winapi_ui::create_arg::width, 320);
//
//   auto s = winapi_ui::link(
//       winapi_ui::register_class(),
//       create,
//       winapi_ui::show_window(),
//       winapi_ui::message_loop());
//
//   s._continue();          // register -> create -> show -> message loop
//   int exit = s.final_return();
//
// 组件链的输入约定：带 front_feed 的组件把上一段返回值直接覆盖到 slot 0。
// 例如 create 的 slot 0 是 HMODULE（来自 register_class），show / message_loop
// 的 slot 0 是 HWND。其余参数都保留默认值，直到用户用 set() 改。
//
// 默认消息循环不装 timer（timer_ms = 0），窗口靠 WM_CLOSE/WM_DESTROY 退出；
// 测试/自动关闭可以 set(loop_arg::timer_ms, 20)，默认窗口过程会在 WM_TIMER
// 时 PostQuitMessage。
// ============================================================================

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <string>

#include "meta_function_series.hpp"

namespace winapi_ui {

// ---- 槽位号：用户改值时用的 enum ----
enum class register_arg : std::size_t {
  class_name = 0,
  window_proc = 1,
  module = 2,
  class_style = 3,
};

enum class create_arg : std::size_t {
  module = 0,
  class_name = 1,
  title = 2,
  x = 3,
  y = 4,
  width = 5,
  height = 6,
  style = 7,
  ex_style = 8,
};

enum class show_arg : std::size_t {
  window = 0,
  command = 1,
};

enum class loop_arg : std::size_t {
  window = 0,
  timer_id = 1,
  timer_ms = 2,
};

enum class destroy_arg : std::size_t {
  window = 0,
};

// window() 的固定阶段顺序。
enum class window_stage : std::size_t {
  register_class = 0,
  create = 1,
  show = 2,
  message_loop = 3,
};

// ---- 选项 enum（slot 本身就是 enum 类型，set() 直接吃 enum）----
enum class window_style : DWORD {
  overlapped = WS_OVERLAPPEDWINDOW,
  popup = WS_POPUP,
  child = WS_CHILD,
  borderless = WS_POPUP | WS_VISIBLE,
};

enum class window_style_ex : DWORD {
  none = 0,
  topmost = WS_EX_TOPMOST,
  tool_window = WS_EX_TOOLWINDOW,
  layered = WS_EX_LAYERED,
  client_edge = WS_EX_CLIENTEDGE,
};

enum class class_style : UINT {
  default_style = CS_HREDRAW | CS_VREDRAW,
  own_dc = CS_OWNDC,
  double_clicks = CS_DBLCLKS,
};

enum class show_command : int {
  hide = SW_HIDE,
  show = SW_SHOW,
  normal = SW_SHOWNORMAL,
  minimize = SW_MINIMIZE,
  maximize = SW_MAXIMIZE,
};

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

// HMODULE(std::string class_name, WNDPROC proc, HMODULE module,
//         class_style style)
// 默认全部绑好；用户可改 class_name / proc / style / module。
inline auto register_class() {
  using signature = HMODULE(std::string, WNDPROC, HMODULE, class_style);
  auto component = meta_bind::bind_as<signature>(
      [](std::string class_name, WNDPROC proc, HMODULE module,
         class_style style) -> HMODULE {
        HMODULE resolved = module != nullptr ? module : GetModuleHandleA(nullptr);
        if (resolved == nullptr) return nullptr;

        WNDCLASSA window_class{};
        window_class.style = static_cast<UINT>(style);
        window_class.lpfnWndProc =
            proc != nullptr ? proc : &default_window_proc;
        window_class.hInstance = reinterpret_cast<HINSTANCE>(resolved);
        window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        window_class.lpszClassName = class_name.c_str();

        if (RegisterClassA(&window_class) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
          return nullptr;
        }
        return resolved;
      });
  component.bind_a_lot(std::string{"meta_looper_ui"}, &default_window_proc,
                       nullptr, class_style::default_style);
  return component;
}

// HWND(HMODULE module, std::string class_name, std::string title,
//      int x, int y, int width, int height,
//      window_style style, window_style_ex ex_style)
// front_feed：register_class 的 HMODULE 覆盖 slot 0。
inline auto create_window() {
  using signature = HWND(HMODULE, std::string, std::string, int, int, int,
                         int, window_style, window_style_ex);
  auto component = meta_bind::bind_as<signature>(
      [](HMODULE module, std::string class_name, std::string title, int x,
         int y, int width, int height, window_style style,
         window_style_ex ex_style) -> HWND {
        HMODULE resolved = module != nullptr ? module : GetModuleHandleA(nullptr);
        return CreateWindowExA(
            static_cast<DWORD>(ex_style), class_name.c_str(), title.c_str(),
            static_cast<DWORD>(style), x, y, width, height, nullptr, nullptr,
            reinterpret_cast<HINSTANCE>(resolved), nullptr);
      });
  component.bind_a_lot(nullptr, std::string{"meta_looper_ui"},
                       std::string{"meta_looper UI"}, CW_USEDEFAULT,
                       CW_USEDEFAULT, 640, 480, window_style::overlapped,
                       window_style_ex::none);
  component.as_front_feed();
  return component;
}

// HWND(HWND window, show_command command)
// front_feed：上一段 HWND 覆盖 slot 0。
inline auto show_window() {
  using signature = HWND(HWND, show_command);
  auto component = meta_bind::bind_as<signature>(
      [](HWND window, show_command command) -> HWND {
        if (window != nullptr) {
          ShowWindow(window, static_cast<int>(command));
          UpdateWindow(window);
        }
        return window;
      });
  component.bind_a_lot(nullptr, show_command::show);
  component.as_front_feed();
  return component;
}

// int(HWND window, UINT timer_id, UINT timer_ms)
// timer_ms == 0 表示不装 timer；测试可 set(loop_arg::timer_ms, 20)。
inline auto message_loop() {
  using signature = int(HWND, UINT, UINT);
  auto component = meta_bind::bind_as<signature>(
      [](HWND window, UINT timer_id, UINT timer_ms) -> int {
        if (window == nullptr) return -1;

        char class_name[256]{};
        GetClassNameA(window, class_name, static_cast<int>(sizeof(class_name)));
        HINSTANCE instance = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrA(window, GWLP_HINSTANCE));

        if (timer_ms != 0) SetTimer(window, timer_id, timer_ms, nullptr);

        MSG message{};
        int get_result = 0;
        while ((get_result = GetMessageA(&message, nullptr, 0, 0)) > 0) {
          TranslateMessage(&message);
          DispatchMessageA(&message);
        }
        int exit_code = get_result == -1 ? -1 : static_cast<int>(message.wParam);

        DestroyWindow(window);
        if (class_name[0] != '\0') {
          UnregisterClassA(class_name, instance);
        }
        return exit_code;
      });
  component.bind_a_lot(nullptr, 1u, 0u);
  component.as_front_feed();
  return component;
}

// HWND(HWND window)
inline auto destroy_window() {
  using signature = HWND(HWND);
  auto component = meta_bind::bind_as<signature>(
      [](HWND window) -> HWND {
        if (window != nullptr) DestroyWindow(window);
        return window;
      });
  component.bind(nullptr);
  component.as_front_feed();
  return component;}

// link(components...)：直接拼成 function_series，参数栈/返回值缓冲连续存放。
template <class... Components>
auto link(Components&&... components) {
  return meta_bind::link_efb(std::forward<Components>(components)...);
}

// 默认四段：register -> create -> show -> message loop。
// 用户可以对 window().stage<window_stage::create>() 继续 set()。
inline auto window() {
  return link(register_class(), create_window(), show_window(), message_loop());
}

// 不带消息循环的三段：register -> create -> show，返回 HWND。
inline auto window_without_loop() {
  return link(register_class(), create_window(), show_window());
}

}  // namespace winapi_ui

#endif  // _WIN32
