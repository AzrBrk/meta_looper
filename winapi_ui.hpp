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
//   s._bind_continue();     // register -> create -> show -> message loop
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
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "meta_function_series.hpp"
#include "meta_static_function_series.hpp"

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
  dialog_navigation = 3,
};

enum class destroy_arg : std::size_t {
  window = 0,
};

enum class control_arg : std::size_t {
  parent = 0,
  id = 1,
  text = 2,
  x = 3,
  y = 4,
  width = 5,
  height = 6,
  style = 7,
  ex_style = 8,
};

enum class text_arg : std::size_t { window = 0, text = 1 };
enum class enable_arg : std::size_t { window = 0, enabled = 1 };
enum class check_arg : std::size_t { window = 0, checked = 1 };
enum class move_arg : std::size_t {
  window = 0, x = 1, y = 2, width = 3, height = 4, repaint = 5,
};
enum class items_arg : std::size_t { window = 0, items = 1 };
enum class selection_arg : std::size_t { window = 0, index = 1 };

class api_error : public std::runtime_error {
 public:
  DWORD const code;
  explicit api_error(char const* operation, DWORD error = GetLastError())
      : std::runtime_error(operation), code(error) {}
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
  using signature = int(HWND, UINT, UINT, bool);
  auto component = meta_bind::bind_as<signature>(
      [](HWND window, UINT timer_id, UINT timer_ms, bool dialog_navigation) -> int {
        if (window == nullptr) return -1;

        char class_name[256]{};
        GetClassNameA(window, class_name, static_cast<int>(sizeof(class_name)));
        HINSTANCE instance = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrA(window, GWLP_HINSTANCE));

        if (timer_ms != 0) SetTimer(window, timer_id, timer_ms, nullptr);

        MSG message{};
        int get_result = 0;
        while ((get_result = GetMessageA(&message, nullptr, 0, 0)) > 0) {
          if (dialog_navigation && IsDialogMessageA(window, &message)) continue;
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
  component.bind_a_lot(nullptr, 1u, 0u, true);
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
  return component;
}

inline HWND require_window(HWND window) {
  if (!IsWindow(window)) throw api_error("invalid window", ERROR_INVALID_WINDOW_HANDLE);
  return window;
}

inline HWND child(HWND parent, int id) {
  require_window(parent);
  HWND result = GetDlgItem(parent, id);
  if (!result) throw api_error("child ID not found", ERROR_NOT_FOUND);
  return result;
}

inline std::string text(HWND window) {
  require_window(window);
  SetLastError(ERROR_SUCCESS);
  int length = GetWindowTextLengthA(window);
  if (!length && GetLastError() != ERROR_SUCCESS) throw api_error("GetWindowTextLengthA");
  std::string result(static_cast<std::size_t>(length) + 1, '\0');
  SetLastError(ERROR_SUCCESS);
  int copied = GetWindowTextA(window, result.data(), static_cast<int>(result.size()));
  if (!copied && GetLastError() != ERROR_SUCCESS) throw api_error("GetWindowTextA");
  result.resize(static_cast<std::size_t>(copied));
  return result;
}

inline auto control(char const* class_name, DWORD default_style,
                    int width = 120, int height = 28, DWORD default_ex_style = 0) {
  using signature = HWND(HWND, int, std::string, int, int, int, int, DWORD, DWORD);
  auto component = meta_bind::bind_as<signature>(
      [class_name = std::string{class_name}](HWND parent, int id, std::string text,
          int x, int y, int width, int height, DWORD style, DWORD ex_style) -> HWND {
        require_window(parent);
        if (id <= 0 || id > 0xffff)
          throw api_error("control ID must be in [1, 65535]", ERROR_INVALID_PARAMETER);
        if (GetDlgItem(parent, id))
          throw api_error("duplicate control ID", ERROR_ALREADY_EXISTS);
        auto module = reinterpret_cast<HINSTANCE>(GetWindowLongPtrA(parent, GWLP_HINSTANCE));
        HWND created = CreateWindowExA(ex_style, class_name.c_str(), text.c_str(),
            style | WS_CHILD, x, y, width, height, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module, nullptr);
        if (!created) throw api_error("CreateWindowExA(control)");
        SendMessageA(created, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return parent;
      });
  component.bind_a_lot(nullptr, 1, std::string{}, 12, 12, width, height,
                       default_style, default_ex_style);
  component.as_front_feed();
  return component;
}

inline auto button(int id = 1, std::string caption = "Button") {
  auto component = control("BUTTON", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON);
  component.set<control_arg::id>(id);
  component.set<control_arg::text>(std::move(caption));
  return component;
}

inline auto label(int id = 1, std::string caption = "") {
  auto component = control("STATIC", WS_VISIBLE | SS_LEFT, 160, 24);
  component.set<control_arg::id>(id);
  component.set<control_arg::text>(std::move(caption));
  return component;
}

inline auto edit(int id = 1, std::string initial = "") {
  auto component = control("EDIT", WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                           200, 26, WS_EX_CLIENTEDGE);
  component.set<control_arg::id>(id);
  component.set<control_arg::text>(std::move(initial));
  return component;
}

inline auto checkbox(int id = 1, std::string caption = "Check") {
  auto component = control("BUTTON", WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                           180, 26);
  component.set<control_arg::id>(id);
  component.set<control_arg::text>(std::move(caption));
  return component;
}

inline auto group_box(int id = 1, std::string caption = "Group") {
  auto component = control("BUTTON", WS_VISIBLE | BS_GROUPBOX, 280, 160);
  component.set<control_arg::id>(id);
  component.set<control_arg::text>(std::move(caption));
  return component;
}

inline auto combo_box(int id = 1) {
  auto component = control("COMBOBOX", WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                           CBS_DROPDOWNLIST | CBS_HASSTRINGS, 200, 180);
  component.set<control_arg::id>(id);
  return component;
}

inline auto list_box(int id = 1) {
  auto component = control("LISTBOX", WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                           LBS_NOTIFY | LBS_HASSTRINGS, 200, 120, WS_EX_CLIENTEDGE);
  component.set<control_arg::id>(id);
  return component;
}

template <class Component>
auto place(Component component, int x, int y, int width, int height) {
  component.template set<control_arg::x>(x);
  component.template set<control_arg::y>(y);
  component.template set<control_arg::width>(width);
  component.template set<control_arg::height>(height);
  return component;
}

inline auto set_text(std::string value = "") {
  using signature = HWND(HWND, std::string);
  auto component = meta_bind::bind_as<signature>([](HWND window, std::string value) {
    require_window(window);
    if (!SetWindowTextA(window, value.c_str())) throw api_error("SetWindowTextA");
    return window;
  });
  component.bind_a_lot(nullptr, std::move(value));
  component.as_front_feed();
  return component;
}

inline auto enable_window(bool enabled = true) {
  using signature = HWND(HWND, bool);
  auto component = meta_bind::bind_as<signature>([](HWND window, bool enabled) {
    EnableWindow(require_window(window), enabled);
    return window;
  });
  component.bind_a_lot(nullptr, enabled);
  component.as_front_feed();
  return component;
}

inline auto focus_window() {
  using signature = HWND(HWND);
  auto component = meta_bind::bind_as<signature>([](HWND window) {
    SetFocus(require_window(window));
    if (GetFocus() != window) throw api_error("SetFocus failed");
    return window;
  });
  component.bind(nullptr);
  component.as_front_feed();
  return component;
}

inline auto check_box(bool checked = true) {
  using signature = HWND(HWND, bool);
  auto component = meta_bind::bind_as<signature>([](HWND window, bool checked) {
    SendMessageA(require_window(window), BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return window;
  });
  component.bind_a_lot(nullptr, checked);
  component.as_front_feed();
  return component;
}

inline auto move_window(int x = 0, int y = 0, int width = 120, int height = 28) {
  using signature = HWND(HWND, int, int, int, int, bool);
  auto component = meta_bind::bind_as<signature>(
      [](HWND window, int x, int y, int width, int height, bool repaint) {
        require_window(window);
        if (!MoveWindow(window, x, y, width, height, repaint)) throw api_error("MoveWindow");
        return window;
      });
  component.bind_a_lot(nullptr, x, y, width, height, true);
  component.as_front_feed();
  return component;
}

template <UINT Message>
auto append_items(std::initializer_list<std::string> items = {}) {
  using signature = HWND(HWND, std::vector<std::string>);
  auto component = meta_bind::bind_as<signature>(
      [](HWND window, std::vector<std::string> items) {
        require_window(window);
        for (auto const& item : items) {
          auto result = SendMessageA(window, Message, 0, reinterpret_cast<LPARAM>(item.c_str()));
          if (result == -1 || result == -2)
            throw api_error("append item failed", ERROR_NOT_ENOUGH_MEMORY);
        }
        return window;
      });
  component.bind_a_lot(nullptr, std::vector<std::string>{items});
  component.as_front_feed();
  return component;
}

inline auto combo_items(std::initializer_list<std::string> items = {}) {
  return append_items<CB_ADDSTRING>(items);
}

inline auto list_items(std::initializer_list<std::string> items = {}) {
  return append_items<LB_ADDSTRING>(items);
}

template <UINT Message>
auto select_item(int index = 0) {
  using signature = HWND(HWND, int);
  auto component = meta_bind::bind_as<signature>([](HWND window, int index) {
    require_window(window);
    auto result = SendMessageA(window, Message, static_cast<WPARAM>(index), 0);
    if (result == -1 && index != -1)
      throw api_error("selection index out of range", ERROR_INVALID_INDEX);
    return window;
  });
  component.bind_a_lot(nullptr, index);
  component.as_front_feed();
  return component;
}

inline auto combo_selection(int index = 0) { return select_item<CB_SETCURSEL>(index); }
inline auto list_selection(int index = 0) { return select_item<LB_SETCURSEL>(index); }

template <class... Components>
auto with_child(int id, Components&&... components) {
  auto actions = meta_bind::link_efb(std::forward<Components>(components)...);
  using signature = HWND(HWND);
  auto component = meta_bind::bind_as<signature>(
      [id, actions = std::move(actions)](HWND parent) mutable {
        HWND target = child(parent, id);
        actions.reset();
        actions.template stage<0>().template bind_at<0>(target);
        actions.template at<0>()._bind_continue();
        return parent;
      });
  component.bind(nullptr);
  component.as_front_feed();
  return component;
}

// link(components...)：直接拼成 function_series，参数栈/返回值缓冲连续存放。
template <class... Components>
auto link(Components&&... components) {
  return meta_bind::link_efb(std::forward<Components>(components)...);
}

template <class... Components>
auto link_static(Components&&... components) {
  return meta_bind::link_static_binders(std::forward<Components>(components)...);
}

namespace static_run_detail {

using exp_utilities::exp_list;
using exp_utilities::exp_select;

template <class Parameter, class Value>
constexpr bool writable = [] {
  if constexpr (meta_bind::detail::is_ref_slot_v<Parameter>)
    return meta_bind::detail::slot_bindable_v<Parameter, Value>;
  else
    return std::is_assignable_v<meta_bind::detail::slot_type_t<Parameter>&, Value>;
}();

template <class Component>
constexpr bool cache_supported = [] {
  using result = typename Component::return_type;
  if constexpr (std::is_void_v<result>) return true;
  else return !std::is_reference_v<result> &&
      std::is_default_constructible_v<result> && std::is_assignable_v<result&, result>;
}();

template <class... Components>
constexpr bool supported = []<std::size_t... Index>(std::index_sequence<Index...>) {
  using types = exp_list<std::remove_cvref_t<Components>...>;
  using first = exp_select<0, types>;
  using last = exp_select<sizeof...(Components) - 1, types>;
  if constexpr (first::argument_count == 0 ||
                std::is_void_v<typename last::return_type>) return false;
  else return writable<exp_select<0, typename first::argument_types>, HWND> &&
      (cache_supported<std::remove_cvref_t<Components>> && ...) &&
      ([] {
        if constexpr (Index == 0) return true;
        else {
          using previous = typename exp_select<Index - 1, types>::return_type;
          using current = exp_select<Index, types>;
          if constexpr (std::is_void_v<previous> || current::argument_count == 0)
            return true;
          else {
            using parameter = exp_select<0, typename current::argument_types>;
            return writable<parameter, previous&> || writable<parameter, previous&&>;
          }
        }
      }() && ...);
}(std::index_sequence_for<Components...>{});

template <class... Components>
bool ready(Components const&... components) {
  bool result = ((components.bound_count() ==
      std::remove_cvref_t<Components>::argument_count) && ...);
  using types = exp_list<std::remove_cvref_t<Components>...>;
  meta_states::run<meta_ios::stream_states_co<>, meta_iostream::meta_iterator,
      meta_iostream::index_sequence_istream<sizeof...(Components)>>::for_each(
      [&](auto stream) {
    constexpr auto index = decltype(stream)::to_t::value;
    if constexpr (index > 0) {
      using previous = typename exp_select<index - 1, types>::return_type;
      using current = exp_select<index, types>;
      if constexpr (!std::is_void_v<previous> && current::argument_count > 0)
        result = result && meta_bind::static_series_detail::select_argument<index>(
            components...).front_feed();
    }
  });
  return result;
}

}

template <class... Components>
auto run(HWND window, Components&&... components) {
  if constexpr (static_run_detail::supported<Components...>) {
    if (static_run_detail::ready(components...)) {
      auto chain = winapi_ui::link_static(std::forward<Components>(components)...);
      chain.template write<0>(require_window(window));
      chain.execute_feed();
      return chain.returns().template get<sizeof...(Components) - 1>();
    }
  }
  auto chain = link(std::forward<Components>(components)...);
  chain.reset();
  chain.template stage<0>().template bind_at<0>(require_window(window));
  chain.template at<0>()._bind_continue();
  return chain.final_return();
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
