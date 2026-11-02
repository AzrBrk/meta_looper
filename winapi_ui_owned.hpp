#pragma once

#include "winapi_ui_static.hpp"

#if defined(_WIN32)
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace winapi_ui_owned {

struct window_options {
  int width{540};
  int height{500};
  int x{CW_USEDEFAULT};
  int y{CW_USEDEFAULT};
  DWORD style{WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX};
  DWORD ex_style{};
};

struct event {
  UINT message;
  WPARAM word;
  LPARAM parameter;
  int id() const noexcept { return LOWORD(word); }
  int notification() const noexcept { return HIWORD(word); }
};

template <class State>
class context {
  HWND const& window_;
  bool& closing_;
  DWORD thread_;

 public:
  State& state;
    context(HWND const& window, bool& closing, DWORD thread, State& value) noexcept
      : window_(window), closing_(closing), thread_(thread), state(value) {}
  HWND hwnd() const {
    if (GetCurrentThreadId() != thread_)
      throw std::logic_error("owned window: wrong thread");
    return window_;
  }
  void close() const {
    if (HWND window = hwnd()) {
      if (closing_) return;
      closing_ = true;
      BOOL destroyed = DestroyWindow(window);
      closing_ = false;
      if (!destroyed || window_)
        throw winapi_ui::api_error("DestroyWindow(owned)");
    }
  }
  template <class... Components>
  auto ui(Components&&... components) const {
    return winapi_ui_static::run(hwnd(), std::forward<Components>(components)...);
  }
};

namespace detail {

template <auto& ClassName>
struct registration {
  DWORD thread{GetCurrentThreadId()};
  HINSTANCE instance{GetModuleHandleA(nullptr)};
  ATOM atom{};

  HINSTANCE acquire(WNDPROC procedure) {
    WNDCLASSEXA description{};
    description.cbSize = sizeof(description);
    description.hInstance = instance;
    description.lpfnWndProc = procedure;
    description.lpszClassName = ClassName;
    description.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    description.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    atom = RegisterClassExA(&description);
    if (!atom) throw winapi_ui::api_error("RegisterClassExA(owned)");
    return instance;
  }
  ~registration() {
    if (GetCurrentThreadId() != thread) std::terminate();
    if (atom && !UnregisterClassA(ClassName, instance)) std::terminate();
  }
};

template <auto& ClassName, auto& Title, class State, class Create, class Event>
struct owner {
  registration<ClassName> registered;
  State state;
  Create on_create;
  Event on_event;
  window_options options;
  HWND window{};
  std::exception_ptr failure;
  unsigned callback_depth{};
  bool closing{};
  bool running{};
  bool consumed{};

  owner(State&& value, Create&& create, Event&& events, window_options settings)
      : state(std::move(value)), on_create(std::move(create)),
        on_event(std::move(events)), options(settings) {}
  owner(owner const&) = delete;
  owner& operator=(owner const&) = delete;
  ~owner() {
    if (callback_depth || GetCurrentThreadId() != registered.thread)
      std::terminate();
    close_noexcept();
  }
  context<State> view() { return {window, closing, registered.thread, state}; }
  void close_noexcept() noexcept {
    try { view().close(); } catch (...) { std::terminate(); }
  }
  void rethrow() const { if (failure) std::rethrow_exception(failure); }

  static LRESULT CALLBACK procedure(HWND target, UINT message,
                                    WPARAM word, LPARAM parameter) noexcept {
    auto* self = reinterpret_cast<owner*>(GetWindowLongPtrA(target, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      auto* creation = reinterpret_cast<CREATESTRUCTA*>(parameter);
      self = static_cast<owner*>(creation->lpCreateParams);
      if (!self || GetCurrentThreadId() != self->registered.thread) return FALSE;
      SetLastError(ERROR_SUCCESS);
      auto previous = SetWindowLongPtrA(target, GWLP_USERDATA,
                                        reinterpret_cast<LONG_PTR>(self));
      if (!previous && GetLastError() != ERROR_SUCCESS) return FALSE;
      self->window = target;
      return DefWindowProcA(target, message, word, parameter);
    }
    if (message == WM_NCDESTROY) {
      SetLastError(ERROR_SUCCESS);
      auto previous = SetWindowLongPtrA(target, GWLP_USERDATA, 0);
      if (!previous && GetLastError() != ERROR_SUCCESS) std::terminate();
      if (self) {
        self->window = nullptr;
        if (self->running && !PostThreadMessageA(self->registered.thread, WM_NULL, 0, 0))
          std::terminate();
      }
      return DefWindowProcA(target, message, word, parameter);
    }
    if (!self) return DefWindowProcA(target, message, word, parameter);
    struct callback_scope {
      owner& value;
      explicit callback_scope(owner& object) : value(object) { ++value.callback_depth; }
      ~callback_scope() { --value.callback_depth; }
    } scope{*self};
    try {
      auto view = self->view();
      if (message == WM_CREATE) return self->on_create(view) ? 0 : -1;
      if (message == WM_CLOSE) { view.close(); return 0; }
      if (message == WM_DESTROY) return DefWindowProcA(target, message, word, parameter);
      if (!self->failure && !self->closing) {
        std::optional<LRESULT> result = self->on_event(view, event{message, word, parameter});
        if (result) return *result;
      }
      if (!self->window) return 0;
      return DefWindowProcA(target, message, word, parameter);
    } catch (...) {
      if (!self->failure) self->failure = std::current_exception();
      if (message == WM_CREATE) return -1;
      self->close_noexcept();
      return 0;
    }
  }

  int run(int show, bool dialog_navigation) {
    auto current = view();
    current.hwnd();
    if (consumed || callback_depth)
      throw std::logic_error("owned window: run is one-shot and non-reentrant");
    consumed = true;
    struct running_scope {
      bool& value;
      explicit running_scope(bool& active) : value(active) { value = true; }
      ~running_scope() { value = false; }
    } active{running};
    try {
      rethrow();
      if (!window) return 0;
      auto show_action = winapi_ui::show_window();
      show_action.template set<winapi_ui::show_arg::command>(
          static_cast<winapi_ui::show_command>(show));
      current.ui(std::move(show_action));
      int exit_code{};
      MSG message{};
      while (window) {
        BOOL status = GetMessageA(&message, nullptr, 0, 0);
        if (status == -1) throw winapi_ui::api_error("GetMessageA(owned)");
        if (!status) { exit_code = static_cast<int>(message.wParam); break; }
        if (!window) break;
        if (dialog_navigation && IsDialogMessageA(window, &message)) continue;
        if (!window) break;
        TranslateMessage(&message);
        DispatchMessageA(&message);
      }
      current.close();
      rethrow();
      return exit_code;
    } catch (...) {
      close_noexcept();
      throw;
    }
  }
};

template <class Owner>
struct run_session {
  std::unique_ptr<Owner> owned;
  int operator()(int show, bool dialog_navigation) {
    return owned->run(show, dialog_navigation);
  }
};

}

template <class Owner>
class window {
  using session = detail::run_session<Owner>;
  using series = decltype(meta_bind::link_static_heap(std::declval<session>()));
  Owner* owner_;
  series run_series_;

 public:
  explicit window(std::unique_ptr<Owner> value)
      : owner_(value.get()),
        run_series_(meta_bind::link_static_heap(session{std::move(value)})) {
    run_series_.initialize(SW_SHOWNORMAL, true);
  }
  window(window const&) = delete;
  window(window&&) = delete;
  window& operator=(window const&) = delete;
  window& operator=(window&&) = delete;
  auto view() {
    auto current = owner_->view();
    current.hwnd();
    return current;
  }
  HWND hwnd() { return view().hwnd(); }
  auto& state() { view().hwnd(); return owner_->state; }
  void close() { view().close(); owner_->rethrow(); }
  int run(int show = SW_SHOWNORMAL, bool dialog_navigation = true) {
    view();
    if (owner_->callback_depth || owner_->consumed)
      throw std::logic_error("owned window: run is one-shot and non-reentrant");
    run_series_.template write<0>(show);
    run_series_.template write<1>(dialog_navigation);
    run_series_.execute();
    return run_series_.returns().template get<0>();
  }
};

template <auto& ClassName, auto& Title, class State, class Create, class Event>
auto create(State state, Create on_create, Event on_event,
            window_options options = {}) {
  static_assert(std::is_array_v<std::remove_reference_t<decltype(ClassName)>> &&
                std::is_array_v<std::remove_reference_t<decltype(Title)>>,
                "class name and title must be static char arrays");
  using owner = detail::owner<ClassName, Title, State, Create, Event>;
  auto owned = std::make_unique<owner>(std::move(state), std::move(on_create),
                                      std::move(on_event), options);
  HWND created{};
  DWORD error{};
  {
    auto create_series = meta_bind::link_static_heap(
        [stable = owned.get()]() -> HINSTANCE {
          return stable->registered.acquire(&owner::procedure);
        },
        [stable = owned.get()](HINSTANCE instance) -> HWND {
          auto const& settings = stable->options;
          return CreateWindowExA(settings.ex_style, ClassName, Title,
              settings.style, settings.x, settings.y, settings.width, settings.height,
              nullptr, nullptr, instance, stable);
        });
    create_series.initialize(HINSTANCE{});
    create_series.execute_feed();
    created = create_series.returns().template get<1>();
    error = GetLastError();
  }
  owned->rethrow();
  if (!created || created != owned->window)
    throw winapi_ui::api_error("CreateWindowExA(owned)", error ? error : ERROR_CANCELLED);
  return window<owner>{std::move(owned)};
}

}
#endif