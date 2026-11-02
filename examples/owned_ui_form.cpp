#include "winapi_ui_owned.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace ui = winapi_ui_static;
namespace owned = winapi_ui_owned;

namespace {
inline constexpr char class_name[] = "MetaLooper.Owned.GreetingForm";
inline constexpr char title[] = "Greeting composer | meta_looper";
inline constexpr UINT ready_message = WM_APP + 31;
enum ids : int { name_edit = 101, format_combo, upper_check, apply_button,
                 close_button, history_list, status_label, name_label,
                 format_label, options_group, history_label };

struct observations {
  bool create_returned{};
  bool synchronous_create{};
  bool verified{};
  int destroyed{};
  HWND handle{};
};

struct app_state {
  bool automatic;
  std::shared_ptr<observations> observed;
  std::string last;
  int applications{};

  app_state(bool testing, std::shared_ptr<observations> seen)
      : automatic(testing), observed(std::move(seen)) {}
  app_state(app_state&&) = default;
  ~app_state() {
    if (observed) {
      if (IsWindow(observed->handle)) std::terminate();
      ++observed->destroyed;
    }
  }
};

using view = owned::context<app_state>;

void require(bool condition, char const* operation) {
  if (!condition) throw std::runtime_error(operation);
}

void apply(view form) {
  auto name = ui::text(ui::child(form.hwnd(), name_edit));
  if (name.empty()) name = "World";
  auto format = SendMessageA(ui::child(form.hwnd(), format_combo), CB_GETCURSEL, 0, 0);
  form.state.last = format == 1 ? "Welcome, " + name + "!" : "Hello, " + name;
  if (SendMessageA(ui::child(form.hwnd(), upper_check), BM_GETCHECK, 0, 0) == BST_CHECKED)
    std::transform(form.state.last.begin(), form.state.last.end(), form.state.last.begin(),
        [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
  ++form.state.applications;
  form.ui(ui::with_child(history_list, ui::list_items({form.state.last}),
                ui::list_selection(form.state.applications - 1)),
          ui::with_child(status_label, ui::set_text(
              std::to_string(form.state.applications) + " saved  |  " + form.state.last)));
}

bool build(view form) {
  form.state.observed->synchronous_create = !form.state.observed->create_returned;
  form.state.observed->handle = form.hwnd();
  form.ui(
      ui::place(ui::group_box(options_group, "Compose a greeting"), 16, 12, 488, 218),
      ui::place(ui::label(name_label, "Name"), 32, 48, 96, 24),
      ui::place(ui::edit(name_edit, "Ada"), 144, 44, 336, 26),
      ui::place(ui::label(format_label, "Greeting"), 32, 90, 96, 24),
      ui::place(ui::combo_box(format_combo), 144, 86, 336, 180),
      ui::place(ui::checkbox(upper_check, "Uppercase"), 144, 128, 300, 26),
      ui::place(ui::button(apply_button, "Apply"), 144, 178, 120, 30),
      ui::place(ui::button(close_button, "Close"), 280, 178, 120, 30),
      ui::place(ui::label(history_label, "History"), 16, 246, 488, 22),
      ui::place(ui::list_box(history_list), 16, 272, 488, 126),
      ui::place(ui::label(status_label, "Ready"), 16, 414, 488, 24),
      ui::with_child(format_combo, ui::combo_items({"Hello", "Welcome"}),
                     ui::combo_selection(0)));
  require(PostMessageA(form.hwnd(), ready_message, 0, 0), "post ready");
  return true;
}

std::optional<LRESULT> events(view form, owned::event event) {
  if (event.message == WM_COMMAND && event.notification() == BN_CLICKED && event.parameter) {
    if (event.id() == apply_button) { apply(form); return 0; }
    if (event.id() == close_button) { form.close(); return 0; }
  }
  if (event.message == ready_message) {
    form.ui(ui::with_child(name_edit, ui::focus_window()));
    if (form.state.automatic) {
      require(form.state.observed->create_returned &&
              form.state.observed->synchronous_create, "creation ordering");
      require(SetTimer(form.hwnd(), 1, 150, nullptr) != 0, "verification timer");
      require(SetTimer(form.hwnd(), 2, 3000, nullptr) != 0, "watchdog timer");
      form.ui(ui::with_child(name_edit, ui::set_text("Grace")));
      SendMessageA(ui::child(form.hwnd(), apply_button), BM_CLICK, 0, 0);
      form.ui(ui::with_child(name_edit, ui::set_text("Ada")),
              ui::with_child(format_combo, ui::combo_selection(1)),
              ui::with_child(upper_check, ui::check_box()));
      SendMessageA(ui::child(form.hwnd(), apply_button), BM_CLICK, 0, 0);
      require(PostMessageA(ui::child(form.hwnd(), name_edit), WM_KEYDOWN, VK_TAB, 0),
              "post Tab");
    }
    return 0;
  }
  if (event.message == WM_TIMER && form.state.automatic) {
    require(event.word != 2, "auto-test watchdog expired");
    require(form.state.last == "WELCOME, ADA!" && form.state.applications == 2,
            "persistent application state");
    require(ui::text(ui::child(form.hwnd(), status_label)) == "2 saved  |  WELCOME, ADA!",
            "status text");
    require(SendMessageA(ui::child(form.hwnd(), history_list), LB_GETCOUNT, 0, 0) == 2,
            "history count");
    require(GetFocus() == ui::child(form.hwnd(), format_combo), "Tab navigation");
    KillTimer(form.hwnd(), 1);
    KillTimer(form.hwnd(), 2);
    form.state.observed->verified = true;
    require(PostMessageA(form.hwnd(), WM_CLOSE, 0, 0), "post close");
    return 0;
  }
  return {};
}
}

int main(int argc, char** argv) {
  bool automatic = argc > 1 && std::strcmp(argv[1], "--auto-test") == 0;
  auto observed = std::make_shared<observations>();
  try {
    {
      auto form = owned::create<class_name, title>(
          app_state{automatic, observed}, build, events, {.width = 540, .height = 500});
      observed->create_returned = true;
      require(form.hwnd() != nullptr, "created window");
      require(ui::text(form.hwnd()) == title, "window title");
      require(form.run() == 0 && !form.hwnd(), "normal close");
    }
    WNDCLASSEXA description{};
    description.cbSize = sizeof(description);
    require(observed->destroyed == 1 && !IsWindow(observed->handle) &&
            !GetClassInfoExA(GetModuleHandleA(nullptr), class_name, &description), "cleanup");
    require(!automatic || observed->verified, "auto-test not completed");
    std::puts("owned_ui_form: PASS (sync create, post-arena state, input, actions, history, Tab, close, cleanup)");
    return 0;
  } catch (std::exception const& error) {
    std::fprintf(stderr, "owned_ui_form: FAIL: %s\n", error.what());
    return 1;
  }
}