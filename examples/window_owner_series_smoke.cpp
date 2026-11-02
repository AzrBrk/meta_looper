#include "meta_static_function_series.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

inline constexpr wchar_t class_name[] = L"MetaLooper.OwnerSeries.Proof";
inline constexpr wchar_t window_title[] = L"Series lifetime proof";
inline constexpr UINT exercise_message = WM_APP + 1;

struct observations {
  bool inside_create{};
  bool create_series_destroyed{};
  bool reject_create{};
  bool callback_failed{};
  bool watchdog_expired{};
  int synchronous_creates{};
  int later_messages{};
  int detached{};
  int owners_destroyed{};
};

struct WindowOwner {
  observations& observed;
  HWND window{};
  bool running{};
  int control_value{41};
  std::function<void(WindowOwner&)> callback;

  explicit WindowOwner(observations& state) : observed(state) {
    callback = [](WindowOwner& owner) { ++owner.control_value; };
  }
  WindowOwner(WindowOwner const&) = delete;
  WindowOwner& operator=(WindowOwner const&) = delete;
  ~WindowOwner() {
    if (window) {
      running = false;
      BOOL destroyed = DestroyWindow(window);
      assert(destroyed);
      static_cast<void>(destroyed);
    }
    assert(!window);
    ++observed.owners_destroyed;
  }
};

LRESULT CALLBACK window_proc(HWND window, UINT message,
                             WPARAM word, LPARAM parameter) noexcept {
  auto* owner = reinterpret_cast<WindowOwner*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* creation = reinterpret_cast<CREATESTRUCTW*>(parameter);
    owner = static_cast<WindowOwner*>(creation->lpCreateParams);
    if (!owner) return FALSE;
    SetLastError(ERROR_SUCCESS);
    LONG_PTR previous = SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(owner));
    if (!previous && GetLastError() != ERROR_SUCCESS) return FALSE;
    owner->window = window;
    return TRUE;
  }
  if (message == WM_NCDESTROY) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    if (owner) {
      owner->window = nullptr;
      ++owner->observed.detached;
    }
    return DefWindowProcW(window, message, word, parameter);
  }
  if (!owner) return DefWindowProcW(window, message, word, parameter);
  try {
    switch (message) {
      case WM_CREATE:
        assert(owner->observed.inside_create);
        assert(!owner->observed.create_series_destroyed);
        ++owner->observed.synchronous_creates;
        owner->callback(*owner);
        return owner->observed.reject_create ? -1 : 0;
      case exercise_message:
        assert(owner->running);
        assert(owner->observed.create_series_destroyed);
        assert(owner->control_value == 42);
        owner->callback(*owner);
        ++owner->observed.later_messages;
        if (!PostMessageW(window, WM_CLOSE, 0, 0))
          throw std::runtime_error("PostMessageW(close)");
        return 0;
      case WM_TIMER:
        owner->observed.watchdog_expired = true;
        DestroyWindow(window);
        return 0;
      case WM_DESTROY:
        if (owner->running) PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(window, message, word, parameter);
    }
  } catch (...) {
    owner->observed.callback_failed = true;
    if (message == WM_CREATE) return -1;
    DestroyWindow(window);
    return 0;
  }
}

struct registered_class {
  HINSTANCE instance{GetModuleHandleW(nullptr)};
  ATOM atom{};

  ATOM register_now() {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.hInstance = instance;
    description.lpfnWndProc = window_proc;
    description.lpszClassName = class_name;
    atom = RegisterClassExW(&description);
    if (!atom) throw std::runtime_error("RegisterClassExW");
    return atom;
  }
  ~registered_class() {
    if (atom) {
      BOOL removed = UnregisterClassW(class_name, instance);
      assert(removed);
      static_cast<void>(removed);
    }
  }
};

struct created_window {
  HWND handle{};
  std::unique_ptr<WindowOwner> owner;
};

created_window create_window(registered_class& registration,
                             observations& observed) {
  auto owner = std::make_unique<WindowOwner>(observed);
  HWND window{};
  {
    auto create_series = meta_bind::link_static_heap(
        [&registration]() -> ATOM { return registration.register_now(); },
        [](ATOM atom, WindowOwner* state) -> HWND {
          assert(atom);
          state->observed.inside_create = true;
          HWND result = CreateWindowExW(0, class_name, window_title,
              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 400, 240,
              nullptr, nullptr, GetModuleHandleW(nullptr), state);
          state->observed.inside_create = false;
          return result;
        });
    create_series.initialize(ATOM{}, owner.get());
    create_series.execute_feed();
    window = create_series.returns().get<1>();
  }
  observed.create_series_destroyed = true;
  assert(window == owner->window);
  return {window, std::move(owner)};
}

struct run_window {
  std::unique_ptr<WindowOwner> owner;
  bool reject_run{};

  int operator()(HWND window) {
    if (!window || window != owner->window)
      throw std::runtime_error("invalid window handoff");
    if (reject_run) throw std::runtime_error("injected run failure");
    assert(owner->observed.create_series_destroyed);
    assert(reinterpret_cast<WindowOwner*>(
        GetWindowLongPtrW(window, GWLP_USERDATA)) == owner.get());
    owner->running = true;
    if (!SetTimer(window, 1, 2000, nullptr))
      throw std::runtime_error("SetTimer");
    if (!PostMessageW(window, exercise_message, 0, 0))
      throw std::runtime_error("PostMessageW(exercise)");
    MSG message{};
    BOOL status{};
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    owner->running = false;
    if (status == -1) throw std::runtime_error("GetMessageW");
    return static_cast<int>(message.wParam);
  }
};

void normal_lifetime() {
  observations observed;
  registered_class registration;
  {
    auto created = create_window(registration, observed);
    assert(created.handle);
    assert(observed.synchronous_creates == 1);
    assert(created.owner->control_value == 42);
    auto* stable_address = created.owner.get();
    auto run_series = meta_bind::link_static_heap(
        run_window{std::move(created.owner)});
    assert(!created.owner);
    assert(reinterpret_cast<WindowOwner*>(GetWindowLongPtrW(
        created.handle, GWLP_USERDATA)) == stable_address);
    run_series.initialize(created.handle);
    run_series.execute();
    assert(run_series.returns().get<0>() == 0);
    assert(observed.later_messages == 1);
    assert(observed.detached == 1);
    assert(observed.owners_destroyed == 0);
    assert(!observed.callback_failed && !observed.watchdog_expired);
  }
  assert(observed.owners_destroyed == 1);
}

void rejected_creation() {
  observations observed;
  observed.reject_create = true;
  registered_class registration;
  {
    auto created = create_window(registration, observed);
    assert(!created.handle);
    assert(created.owner && !created.owner->window);
    assert(observed.synchronous_creates == 1);
    assert(observed.detached == 1);
    assert(observed.owners_destroyed == 0);
  }
  assert(observed.owners_destroyed == 1);
}

void rejected_run() {
  observations observed;
  registered_class registration;
  bool rejected{};
  try {
    auto created = create_window(registration, observed);
    assert(created.handle);
    auto run_series = meta_bind::link_static_heap(
        run_window{std::move(created.owner), true});
    run_series.initialize(created.handle);
    run_series.execute();
  } catch (std::runtime_error const&) {
    rejected = true;
  }
  assert(rejected);
  assert(observed.synchronous_creates == 1);
  assert(observed.detached == 1);
  assert(observed.owners_destroyed == 1);
}

}

int main() {
  normal_lifetime();
  rejected_creation();
  rejected_run();
  std::puts("window_owner_series: PASS (WM_CREATE, post-arena dispatch, creation/run failure, RAII)");
}