#include "winapi_ui_owned.hpp"

#include <cassert>
#include <cstdio>
#include <thread>

namespace {
inline constexpr char class_name[] = "MetaLooper.Owned.Smoke";
inline constexpr char title[] = "Owned window smoke";
inline constexpr UINT exercise = WM_APP + 17;

struct observations {
  int creates{};
  int events{};
  int destroyed{};
  bool creating{true};
  bool reject{};
  bool throw_create{};
  bool throw_event{};
  bool close_inside{};
  bool close_create{};
  bool suppress_exercise{};
  HANDLE sent_close_ready{};
  HWND handle{};
};

struct state {
  observations* observed;
  std::unique_ptr<int> value{std::make_unique<int>(41)};
  state(observations& value) : observed(&value) {}
  state(state&&) = default;
  ~state() {
    if (value) {
      assert(!IsWindow(observed->handle));
      ++observed->destroyed;
    }
  }
};

auto create(observations& observed) {
  return winapi_ui_owned::create<class_name, title>(state{observed},
      [](auto view) {
        auto& seen = *view.state.observed;
        assert(seen.creating);
        seen.handle = view.hwnd();
        ++seen.creates;
        ++*view.state.value;
        if (seen.throw_create) throw std::runtime_error("create callback");
        if (seen.close_create) { view.close(); return true; }
        if (!SetTimer(view.hwnd(), 1, 2000, nullptr))
          throw winapi_ui::api_error("SetTimer(watchdog)");
        if (seen.suppress_exercise) return !seen.reject;
        if (!PostMessageA(view.hwnd(), exercise, 0, 0))
          throw winapi_ui::api_error("PostMessageA");
        return !seen.reject;
      },
      [](auto view, winapi_ui_owned::event event) -> std::optional<LRESULT> {
        if (event.message == WM_TIMER) throw std::runtime_error("watchdog expired");
        if (event.message != exercise) return {};
        auto& seen = *view.state.observed;
        assert(!seen.creating && *view.state.value == 42);
        ++seen.events;
        if (seen.sent_close_ready) {
          if (!SetEvent(seen.sent_close_ready)) throw winapi_ui::api_error("SetEvent");
          return 0;
        }
        if (seen.throw_event) throw std::runtime_error("event callback");
        if (seen.close_inside) view.close();
        else if (!PostMessageA(view.hwnd(), WM_CLOSE, 0, 0))
          throw winapi_ui::api_error("PostMessageA(close)");
        return 0;
      });
}

void normal(bool reentrant_close) {
  observations seen;
  seen.close_inside = reentrant_close;
  {
    auto form = create(seen);
    seen.creating = false;
    assert(seen.creates == 1 && *form.state().value == 42);
    assert(winapi_ui::text(form.hwnd()) == title);
    bool wrong_thread{};
    std::thread other([&] {
      try { form.hwnd(); } catch (std::logic_error const&) { wrong_thread = true; }
    });
    other.join();
    assert(wrong_thread);
    assert(form.run(SW_HIDE) == 0);
    assert(!form.hwnd() && seen.events == 1 && seen.destroyed == 0);
    bool twice{};
    try { form.run(); } catch (std::logic_error const&) { twice = true; }
    assert(twice);
  }
  assert(seen.destroyed == 1);
}

void failures() {
  for (int mode = 0; mode < 5; ++mode) {
    observations seen;
    seen.reject = mode == 0;
    seen.throw_create = mode == 1;
    seen.throw_event = mode == 2;
    seen.close_create = mode == 4;
    bool caught{};
    try {
      auto form = create(seen);
      seen.creating = false;
      if (mode == 3) throw std::runtime_error("caller before run");
      form.run(SW_HIDE);
    } catch (std::runtime_error const&) { caught = true; }
    assert(caught && seen.destroyed == 1 && !IsWindow(seen.handle));
    WNDCLASSEXA description{};
    description.cbSize = sizeof(description);
    assert(!GetClassInfoExA(GetModuleHandleA(nullptr), class_name, &description));
  }
}

void registration_conflict_and_quit() {
  observations first;
  first.suppress_exercise = true;
  {
    auto form = create(first);
    first.creating = false;
    observations second;
    bool conflict{};
    try { auto duplicate = create(second); }
    catch (winapi_ui::api_error const& error) {
      conflict = error.code == ERROR_CLASS_ALREADY_EXISTS;
    }
    assert(conflict && second.creates == 0 && second.destroyed == 1);
    assert(IsWindow(form.hwnd()));
    PostQuitMessage(7);
    assert(form.run(SW_HIDE) == 7);
    assert(!form.hwnd());
  }
  assert(first.destroyed == 1);
}

void sent_close_while_waiting() {
  observations seen;
  HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE finished = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  assert(ready && finished);
  seen.sent_close_ready = ready;
  bool watchdog{};
  {
    auto form = create(seen);
    seen.creating = false;
    HWND target = form.hwnd();
    DWORD thread = GetCurrentThreadId();
    std::thread sender([&] {
      if (WaitForSingleObject(ready, 2000) != WAIT_OBJECT_0) watchdog = true;
      DWORD_PTR result{};
      if (!SendMessageTimeoutA(target, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, &result))
        watchdog = true;
      if (WaitForSingleObject(finished, 2000) != WAIT_OBJECT_0) {
        watchdog = true;
        PostThreadMessageA(thread, WM_NULL, 0, 0);
      }
    });
    int result = form.run(SW_HIDE);
    SetEvent(finished);
    sender.join();
    assert(!watchdog && result == 0 && !form.hwnd());
  }
  CloseHandle(finished);
  CloseHandle(ready);
  assert(seen.destroyed == 1);
}
}

int main() {
  normal(false);
  normal(true);
  failures();
  registration_conflict_and_quit();
  sent_close_while_waiting();
  std::puts("owned_ui_smoke: PASS (sync create, stable state, close, exceptions, thread, unregister)");
}