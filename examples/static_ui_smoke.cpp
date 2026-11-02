#include "winapi_ui_static.hpp"
#include <cassert>
#include <cstdio>

#if defined(_WIN32)
int main() {
  namespace ui = winapi_ui_static;
  HWND root = CreateWindowExA(0, "STATIC", "root", WS_POPUP, 0, 0, 320, 200,
                              nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  assert(root);
  try {
    ui::run(root, ui::group_box(101, "Group"),
        ui::with_child(101, ui::edit(102, "Before"),
            ui::with_child(102, ui::set_text("Nested"), ui::enable_window(false))),
        ui::label(103, "Sibling"));
    HWND group = ui::child(root, 101);
    HWND edit = ui::child(group, 102);
    assert(ui::text(edit) == "Nested" && !IsWindowEnabled(edit));
    assert(GetParent(ui::child(root, 103)) == root);
    auto config = ui::with_child(101,
        ui::with_child(102, ui::set_text("Reused"), ui::enable_window()));
    auto moved = std::move(config);
    auto chain = ui::link(std::move(moved), ui::set_text("Root"));
    static_assert(decltype(chain)::stage_count == 7);
    static_assert(decltype(chain)::total_args == 12);
    static_assert(!std::is_move_constructible_v<decltype(chain)>);
    chain.write<5>(std::string{"Unified"});
    for (int repeat = 0; repeat != 3; ++repeat) {
      chain.write<0>(root);
      chain.execute_feed();
      assert(chain.returns().get<6>() == root);
      assert(ui::text(root) == "Root" && ui::text(edit) == "Unified");
    }
    bool rejected = false;
    try { ui::run(root, ui::with_child(999, ui::set_text("Missing"))); }
    catch (ui::api_error const&) { rejected = true; }
    assert(rejected);
    assert(IsWindowEnabled(edit));
    DestroyWindow(root);
    auto complete = ui::window();
    complete.write<14>(ui::show_command::hide);
    complete.write<17>(10u);
    complete.execute_feed();
    assert(complete.returns().get<3>() == 0);
    std::puts("static_ui_smoke: PASS (flattened nested storage, moved config, reuse, errors)");
    return 0;
  } catch (...) {
    DestroyWindow(root);
    throw;
  }
}
#else
int main() { return 0; }
#endif