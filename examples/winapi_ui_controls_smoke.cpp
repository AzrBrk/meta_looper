#include "winapi_ui.hpp"
#include <cstdio>

#if defined(_WIN32)
namespace {
int clicks{};
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_COMMAND && LOWORD(wparam) == 101 && HIWORD(wparam) == BN_CLICKED) {
    ++clicks;
    SetWindowTextA(winapi_ui::child(window, 102), "Clicked");
    return 0;
  }
  return DefWindowProcA(window, message, wparam, lparam);
}
}

int main() {
  HWND window{};
  bool ok = true;
  try {
    auto registration = winapi_ui::register_class();
    registration.set(winapi_ui::register_arg::class_name, "ui_controls_smoke");
    registration.set(winapi_ui::register_arg::window_proc, &procedure);
    auto creation = winapi_ui::create_window();
    creation.set(winapi_ui::create_arg::class_name, "ui_controls_smoke");
    auto chain = winapi_ui::link(std::move(registration), std::move(creation),
        winapi_ui::place(winapi_ui::button(101, "Apply"), 20, 10, 100, 26),
        winapi_ui::label(102, "Ready"),
        winapi_ui::edit(103, "Input"), winapi_ui::checkbox(104, "Enabled"),
        winapi_ui::group_box(105, "Options"), winapi_ui::combo_box(106),
        winapi_ui::list_box(107),
        winapi_ui::with_child(104, winapi_ui::check_box()),
        winapi_ui::with_child(106, winapi_ui::combo_items({"One", "Two"}),
                             winapi_ui::combo_selection(1)),
        winapi_ui::with_child(107, winapi_ui::list_items({"Red", "Green"}),
                             winapi_ui::list_selection(0)),
        winapi_ui::with_child(103, winapi_ui::set_text("Updated"),
                             winapi_ui::move_window(20, 40, 240, 26),
                             winapi_ui::enable_window(false)));
    chain._bind_continue();
    window = chain.final_return();
    for (int id = 101; id <= 107; ++id)
      ok = ok && GetParent(winapi_ui::child(window, id)) == window;
    ok = ok && winapi_ui::text(winapi_ui::child(window, 103)) == "Updated";
    ok = ok && !IsWindowEnabled(winapi_ui::child(window, 103));
        RECT bounds{};
        GetWindowRect(winapi_ui::child(window, 103), &bounds);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
        ok = ok && bounds.left == 20 && bounds.top == 40 &&
          bounds.right - bounds.left == 240 && bounds.bottom - bounds.top == 26;
    ok = ok && SendMessageA(winapi_ui::child(window, 104), BM_GETCHECK, 0, 0) == BST_CHECKED;
    ok = ok && SendMessageA(winapi_ui::child(window, 106), CB_GETCOUNT, 0, 0) == 2;
    ok = ok && SendMessageA(winapi_ui::child(window, 106), CB_GETCURSEL, 0, 0) == 1;
    ok = ok && SendMessageA(winapi_ui::child(window, 107), LB_GETCURSEL, 0, 0) == 0;
    SendMessageA(winapi_ui::child(window, 101), BM_CLICK, 0, 0);
    ok = ok && clicks == 1 && winapi_ui::text(winapi_ui::child(window, 102)) == "Clicked";
    bool rejected{};
    try { winapi_ui::child(window, 999); }
    catch (winapi_ui::api_error const&) { rejected = true; }
    ok = ok && rejected;
    rejected = false;
    try {
      auto duplicate = winapi_ui::button(101);
      duplicate.bind_front(window);
      duplicate();
    } catch (winapi_ui::api_error const&) { rejected = true; }
    ok = ok && rejected;
    auto enable = winapi_ui::with_child(103, winapi_ui::enable_window());
    enable.bind_front(window);
    enable();
    ok = ok && IsWindowEnabled(winapi_ui::child(window, 103));
    auto checked = winapi_ui::with_child(104, winapi_ui::check_box(false));
    checked.bind_front(window);
    checked();
    ok = ok && SendMessageA(winapi_ui::child(window, 104), BM_GETCHECK, 0, 0) == BST_UNCHECKED;
    auto deselect = winapi_ui::with_child(106, winapi_ui::combo_selection(-1));
    deselect.bind_front(window);
    deselect();
    ok = ok && SendMessageA(winapi_ui::child(window, 106), CB_GETCURSEL, 0, 0) == -1;
    rejected = false;
    try {
      auto invalid = winapi_ui::with_child(106, winapi_ui::combo_selection(100));
      invalid.bind_front(window);
      invalid();
    } catch (winapi_ui::api_error const&) { rejected = true; }
    ok = ok && rejected;
    winapi_ui::run(window, winapi_ui::with_child(102, winapi_ui::set_text("Run")));
    ok = ok && winapi_ui::text(winapi_ui::child(window, 102)) == "Run";
    auto label_text = winapi_ui::set_text("Static default");
    auto static_actions = winapi_ui::link_static(label_text,
      winapi_ui::enable_window(false));
    static_assert(decltype(static_actions)::total_args == 4);
    static_assert(winapi_ui::static_run_detail::supported<decltype(label_text)>);
    ok = ok && winapi_ui::static_run_detail::ready(label_text);
    static_actions.write<0>(winapi_ui::child(window, 102));
    static_actions.execute_feed();
    ok = ok && winapi_ui::text(winapi_ui::child(window, 102)) == "Static default";
    ok = ok && !IsWindowEnabled(winapi_ui::child(window, 102));
    ok = ok && label_text.live_arguments().get<1>() == "Static default";
    static_actions.write<1>(std::string("Reused"));
    static_actions.execute_feed();
    ok = ok && winapi_ui::text(winapi_ui::child(window, 102)) == "Reused";
    auto ordinary = winapi_ui::enable_window();
    ordinary.as_front_feed(false);
    auto before = winapi_ui::set_text("Fallback");
    ok = ok && !winapi_ui::static_run_detail::ready(before, ordinary);
    ok = ok && winapi_ui::run(winapi_ui::child(window, 102), before, ordinary) ==
      winapi_ui::child(window, 102);
    ok = ok && IsWindowEnabled(winapi_ui::child(window, 102));
    auto partial = meta_bind::bind_as<HWND(HWND)>([](HWND target) { return target; });
    ok = ok && winapi_ui::run(window, partial) == window;
    int effects = 0;
    auto effect = meta_bind::bind_as<void(HWND)>([&](HWND target) {
      if (target == window) ++effects;
    });
    effect.bind(nullptr);
    auto independent = meta_bind::bind_as<HWND(HWND)>([](HWND target) { return target; });
    independent.bind(window);
    ok = ok && winapi_ui::static_run_detail::ready(effect, independent);
    ok = ok && winapi_ui::run(window, effect, independent) == window && effects == 1;
    auto source = meta_bind::bind_as<int(HWND)>([](HWND) { return 7; });
    source.bind(nullptr);
    auto zero = meta_bind::bind_as<HWND()>([&] { return window; });
    ok = ok && winapi_ui::run(window, source, zero, winapi_ui::enable_window()) == window;
    rejected = false;
    try { winapi_ui::run(nullptr, winapi_ui::set_text("invalid")); }
    catch (winapi_ui::api_error const& error) {
      rejected = error.code == ERROR_INVALID_WINDOW_HANDLE;
    }
    ok = ok && rejected;
    rejected = false;
    try { winapi_ui::run(window, winapi_ui::button(0)); }
    catch (winapi_ui::api_error const& error) {
      rejected = error.code == ERROR_INVALID_PARAMETER;
    }
    ok = ok && rejected;
    DestroyWindow(window);
    window = nullptr;
    ok = UnregisterClassA("ui_controls_smoke", GetModuleHandleA(nullptr)) && ok;
  } catch (std::exception const& error) {
    std::fprintf(stderr, "%s\n", error.what());
    if (window) DestroyWindow(window);
    UnregisterClassA("ui_controls_smoke", GetModuleHandleA(nullptr));
    return 1;
  }
  std::printf("winapi_ui_controls_smoke: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
#else
int main() { return 0; }
#endif