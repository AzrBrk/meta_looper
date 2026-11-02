#include "winapi_ui_static.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#if defined(META_UI_STATIC_FORM)
namespace form_ui = winapi_ui_static;
#else
namespace form_ui = winapi_ui;
#endif
namespace {
enum ids : int { name_edit = 101, format_combo, upper_check, apply_button,
                 close_button, history_list, status_label, name_label,
                 format_label, options_group };
bool automatic{};
bool failed{};
bool verified{};

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  try {
    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED && lparam != 0) {
      if (LOWORD(wparam) == close_button) {
        PostMessageA(window, WM_CLOSE, 0, 0);
        return 0;
      }
      if (LOWORD(wparam) == apply_button) {
        auto name = form_ui::text(form_ui::child(window, name_edit));
        if (name.empty()) name = "World";
        auto index = SendMessageA(form_ui::child(window, format_combo), CB_GETCURSEL, 0, 0);
        std::string output = index == 1 ? "Welcome, " + name + "!" : "Hello, " + name;
        if (SendMessageA(form_ui::child(window, upper_check), BM_GETCHECK, 0, 0) == BST_CHECKED)
          std::transform(output.begin(), output.end(), output.begin(),
                         [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
        form_ui::run(window,
          form_ui::with_child(status_label, form_ui::set_text(output)),
          form_ui::with_child(history_list, form_ui::list_items({output})));
        return 0;
      }
    }
    if (message == WM_TIMER && automatic) {
        verified = form_ui::text(form_ui::child(window, status_label)) == "WELCOME, ADA!" &&
          SendMessageA(form_ui::child(window, history_list), LB_GETCOUNT, 0, 0) == 1 &&
          GetFocus() == form_ui::child(window, format_combo);
      KillTimer(window, static_cast<UINT_PTR>(wparam));
      PostQuitMessage(verified ? 0 : 1);
      return 0;
    }
    return form_ui::default_window_proc(window, message, wparam, lparam);
  } catch (std::exception const& error) {
    std::fprintf(stderr, "window callback: %s\n", error.what());
    failed = true;
    PostQuitMessage(1);
    return 0;
  }
}

struct window_owner {
  HWND handle{};
  ~window_owner() {
    if (IsWindow(handle)) DestroyWindow(handle);
    UnregisterClassA("winapi_ui_form", GetModuleHandleA(nullptr));
  }
};
}

int main(int argc, char** argv) {
  automatic = argc > 1 && std::strcmp(argv[1], "--auto-test") == 0;
  window_owner owner;
  try {
    auto registration = form_ui::register_class();
    registration.set(form_ui::register_arg::class_name, "winapi_ui_form");
    registration.set(form_ui::register_arg::window_proc, &procedure);
    auto creation = form_ui::create_window();
    creation.set(form_ui::create_arg::class_name, "winapi_ui_form");
    creation.set(form_ui::create_arg::title, "winapi_ui | Native form");
    creation.set(form_ui::create_arg::width, 520);
    creation.set(form_ui::create_arg::height, 480);
    creation.set(form_ui::create_arg::style,
      static_cast<form_ui::window_style>(WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX));
    auto startup = form_ui::link(std::move(registration), std::move(creation));
  #if defined(META_UI_STATIC_FORM)
    startup.execute_feed();
    owner.handle = startup.returns().get<1>();
  #else
    startup._bind_continue();
    owner.handle = startup.final_return();
  #endif
    form_ui::require_window(owner.handle);

    form_ui::run(owner.handle,
      form_ui::place(form_ui::group_box(options_group, "Greeting"), 12, 12, 480, 206),
      form_ui::place(form_ui::label(name_label, "Name"), 28, 42, 100, 24),
      form_ui::place(form_ui::edit(name_edit, "Ada"), 150, 38, 310, 26),
      form_ui::place(form_ui::label(format_label, "Format"), 28, 86, 100, 24),
      form_ui::place(form_ui::combo_box(format_combo), 150, 82, 310, 180),
      form_ui::place(form_ui::checkbox(upper_check, "Uppercase"), 150, 124, 300, 26),
      form_ui::place(form_ui::button(apply_button, "Apply"), 150, 166, 120, 30),
      form_ui::place(form_ui::button(close_button, "Close"), 286, 166, 120, 30),
      form_ui::place(form_ui::list_box(history_list), 12, 236, 480, 130),
      form_ui::place(form_ui::label(status_label, "Ready"), 12, 384, 480, 24),
      form_ui::with_child(format_combo, form_ui::combo_items({"Hello", "Welcome"}),
                 form_ui::combo_selection(0)));
    auto loop = form_ui::message_loop();
    if (automatic) loop.set(form_ui::loop_arg::timer_ms, 150u);
    if (automatic) {
        form_ui::run(owner.handle,
          form_ui::with_child(format_combo, form_ui::combo_selection(1)),
          form_ui::with_child(upper_check, form_ui::check_box()));
      SendMessageA(form_ui::child(owner.handle, apply_button), BM_CLICK, 0, 0);
      PostMessageA(form_ui::child(owner.handle, name_edit), WM_KEYDOWN, VK_TAB, 0);
    }
    int exit_code = form_ui::run(owner.handle, form_ui::show_window(),
      form_ui::with_child(name_edit, form_ui::focus_window()), std::move(loop));
    bool ok = !failed && exit_code == 0 && (!automatic || verified);
    std::printf("winapi_ui_form: %s%s\n", ok ? "PASS" : "FAIL",
                automatic ? " (click, text, history, Tab navigation)" : "");
    return ok ? 0 : 1;
  } catch (std::exception const& error) {
    std::fprintf(stderr, "form: %s\n", error.what());
    return 1;
  }
}
#else
int main() { return 0; }
#endif