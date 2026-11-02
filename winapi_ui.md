# winapi_ui Components

`winapi_ui.hpp` builds native Win32 UI from ordinary `function_binder`
components. Each component owns its default arguments; enum slots remain
available for customization. No event engine or new metaprogramming protocol
is introduced.

## Quick Start

Create a top-level window using `window_without_loop()`, or configure
`register_class()` and `create_window()` separately when supplying a WNDPROC.
Then attach a form to its HWND:

```cpp
auto startup = winapi_ui::window_without_loop();
startup._bind_continue();
HWND parent = startup.final_return();

winapi_ui::run(parent,
    winapi_ui::place(winapi_ui::label(101, "Name"), 16, 20, 90, 24),
    winapi_ui::place(winapi_ui::edit(102, "Ada"), 120, 16, 220, 26),
    winapi_ui::place(winapi_ui::combo_box(103), 120, 58, 220, 180),
    winapi_ui::place(winapi_ui::button(104, "Apply"), 120, 104, 100, 30),
    winapi_ui::with_child(103,
        winapi_ui::combo_items({"Hello", "Welcome"}),
        winapi_ui::combo_selection(0)));

int exit_code = winapi_ui::run(parent,
    winapi_ui::with_child(102, winapi_ui::focus_window()),
    winapi_ui::message_loop());
```

The message loop destroys the top-level window and unregisters its class on
exit. To build controls before first showing the window, use the separate
register/create components, as in `examples/winapi_ui_form.cpp`.

## Component Catalog

| Component | Purpose |
|---|---|
| `button(id, caption)` | Push button, `BN_CLICKED` notification |
| `label(id, caption)` | Left-aligned static text |
| `edit(id, initial)` | Single-line input with client edge |
| `checkbox(id, caption)` | Automatically toggling checkbox |
| `group_box(id, caption)` | Visual grouping, not a child HWND container |
| `combo_box(id)` | Noneditable dropdown, strings and vertical scrollbar |
| `list_box(id)` | Single-selection list, `LBN_SELCHANGE` notification |
| `control(class_name, style, width, height, ex_style)` | Generic native class factory |
| `set_text(value)` | Set target window/control text |
| `enable_window(enabled)` | Enable or disable target |
| `focus_window()` | Set keyboard focus after target is shown/enabled |
| `check_box(checked)` | Set checkbox state; unlike `checkbox`, creates nothing |
| `move_window(x, y, width, height)` | Move/resize an existing target |
| `combo_items({...})`, `list_items({...})` | Append owned strings to target |
| `combo_selection(index)`, `list_selection(index)` | Select item, `-1` clears |
| `show_window()`, `destroy_window()` | Existing show/hide and destroy components |

All creation components accept `control_arg::{parent,id,text,x,y,width,height,
style,ex_style}`. `place(component, x, y, width, height)` sets the four geometry
slots at once. Set the style slot to a complete style mask; `WS_CHILD` is always
added. Native controls use the borrowed stock `DEFAULT_GUI_FONT`.
Dropdown height includes the expanded list, not just its collapsed field.

Actions accept `text_arg`, `enable_arg`, `check_arg`, `move_arg`, `items_arg` or
`selection_arg` slots as applicable. `items_arg::items` stores
`std::vector<std::string>`; item factories accept initializer lists.

## Handle Flow

- Creating a control consumes the parent HWND in slot zero and returns that
  same parent. Sibling creation therefore chains without manual rebinding.
- Ordinary action components consume and return the target HWND.
- `with_child(id, actions...)` looks up a child, runs the action chain on it,
  then returns the parent. Use it to change multiple controls in one chain.
- `child(parent, id)` returns the actual child HWND; `text(hwnd)` reads its text.
- `run(hwnd, components...)` executes a temporary chain and returns its final
  result by value. Compatible, fully bound components use a unified static
  argument stack; other components retain the legacy execution path below.
  For repeated calls, retain a `link_static(...)` or `link(...)` owner.
- A freshly default-bound component may have a full binding cursor. Reset a
  manually assembled series before calling `bind(hwnd)`. `run` handles this.
- Use `_bind_continue()` or `drive()` when passing handles between stages.
  `_continue()` only invokes already-bound stages; it does not forward results.

## Static Indices

When a slot is known at compile time, use the template-index setter:

```cpp
auto creation = winapi_ui::create_window();
creation.set<winapi_ui::create_arg::title>("Native form");
creation.set<winapi_ui::create_arg::width>(520);
```

`set<Index>(value)` accepts an enum or integer constant and delegates to
`bind_at<I>(value)`. It preserves construction, reference-slot handling,
liveness and binding-cursor updates. Out-of-range indices and incompatible
argument types fail at compile time. `set(enum_value, value)` remains available
for a runtime-selected slot, including runtime bounds/type checks. Passing an
enum literal as a function argument does not make it a template index, although
an optimizer may still fold its dispatch.

For series, `stage<I>()`, `_return<I>()` and `at<I>()` already use static indices.
`at<I>()._bind_continue()` explicitly executes from I through the end, regardless
of the owner's current execution cursor; it is not a replacement for resuming
a partially executed chain. The legacy `run` fallback and `with_child` start at zero,
so they use `stage<0>().bind_at<0>()` and `at<0>()._bind_continue()` internally.
`place` and standard-control factories use static enum slots internally.

The series reset implementation now traverses binder storage through
`meta_states::run<stream_states_co<>, meta_aligned_iterator<>,
meta_istream<binders_list>>`. Each expanded callback receives a typed constant
offset cursor. It replaces handwritten index recursion without adding a new
layout calculator, runtime iterator, or generic executor. Runtime execution
cursors and runtime-index APIs remain unchanged. No changes to the looper or
aligned-storage implementation are needed for this composition.

## Static Execution Path

`winapi_ui::link_static(components...)` delegates to
`meta_bind::link_static_binders`. It imports the actual default/configured slots
and callable objects into an initialized static owner. It does not retain old
per-binder stacks behind parameterless wrappers. Factories and both `set` APIs
remain ordinary binders; configure them before importing:

```cpp
auto caption = winapi_ui::set_text("Ready");
caption.set<winapi_ui::text_arg::text>("Updated");
auto actions = winapi_ui::link_static(caption, winapi_ui::enable_window());
actions.write<0>(winapi_ui::child(parent, 101));
actions.execute_feed();
HWND target = actions.returns().get<1>();
```

After import, use flattened `write<Offset>()`/`argument<I>()` and
`returns().get<I>()`, not legacy `stage`, `reset`, or `_bind_continue` APIs.
Repeat `execute_feed()` without a cursor reset. Owned lvalue configurations
are copied; explicitly moved binders are consumed. Non-const reference slots
retain their external referents. See `examples/static_series.md` for ownership,
cache restrictions and exception guarantees.

`run` automatically selects this path when every component is fully bound,
its required assignments/caches are supported, and every effective feed edge
uses the consumer's runtime `front_feed` flag. It writes the incoming HWND into
the unified first slot and runs native static feed execution. A void producer
or zero-argument consumer disables only the adjacent feed edge. Partial binding,
append/cursor feed policies, final void results and unsupported static types
use the previous reset/bind/continue implementation instead. The explicit
`link_static` factory rejects unsupported feed policies rather than falling back.

`link`, `window`, and `window_without_loop` still return the legacy series, so
their stage setters and execution APIs remain unchanged. `with_child` still
owns a movable legacy action chain internally; it can itself be imported as a
callable stage by static `run`. Its nested argument stack is **not** flattened
into the outer static stack. The static owner remains nonmovable because
reference feeds may point into its return cache. This is a staged integration,
not a full migration of nested chains, and makes no performance claim.

## Events And Keyboard

Use the standard WNDPROC supplied through `register_arg::window_proc`.
For `WM_COMMAND`, inspect `LOWORD(wparam)` (ID), `HIWORD(wparam)` (notification)
and `lparam` (control HWND). For example:

```cpp
if (message == WM_COMMAND && LOWORD(wparam) == 104 &&
    HIWORD(wparam) == BN_CLICKED && lparam != 0) {
  auto name = winapi_ui::text(winapi_ui::child(window, 102));
  winapi_ui::run(window,
      winapi_ui::with_child(101, winapi_ui::set_text(name)));
  return 0;
}
```

Do not allow C++ exceptions to escape a WNDPROC. The complete form example
catches them, reports failure and quits its message loop.

`message_loop()` defaults `loop_arg::dialog_navigation` to `true`, routing
messages through `IsDialogMessageA` before normal translation/dispatch. Native
Tab/Shift+Tab order follows control creation order and `WS_TABSTOP`; standard
dialog-key processing may send IDOK/IDCANCEL commands. Handle those commands
in the WNDPROC when desired. Set this slot to `false` for plain message dispatch.
The existing default timer behavior is unchanged: WM_TIMER ends the loop.

## Errors And Ownership

New control/action helpers validate the target HWND. Invalid/missing IDs,
duplicate IDs, creation failures and out-of-range selection throw `api_error`,
which contains an operation name (`what()`) and Win32/synthetic `code`.
IDs must be unique among siblings and in `[1, 65535]`. Apply checkbox/list/combo
actions to the matching control class and compatible styles; these are not
runtime type-checked. Multi-selection/owner-draw controls need their own actions.
Appending items does not clear an existing list; partial additions remain on
failure. `EnableWindow` returning zero is not treated as an error because it
reports the previous disabled state.

Parents own native child lifetimes. Destroying a parent destroys its controls;
binders do not destroy HWNDs when destructed. Strings/items are owned by binders
and copied into native controls. A sequence failure does not roll back earlier
creations; callers must retain and clean up their parent window. Existing
top-level register/create components retain their null-handle failure convention.

This first component set follows the existing ANSI (`...A`) API. Strings use
the process ANSI code page, not UTF-8. Geometry is explicit client-coordinate
pixels, with no automatic layout, resizing or DPI scaling. Group boxes are
decorative siblings; create their grouped controls under the form parent.

## Full Static Composition

`winapi_ui_static.hpp` provides a separate `winapi_ui_static` namespace. It
reuses the same default-bound component factories, enums, strings and native
operations, but supplies its own `link`, `run`, `with_child`, `window` and
`window_without_loop`. Existing `winapi_ui` APIs are unchanged.

```cpp
namespace ui = winapi_ui_static;
auto chain = ui::link(
  ui::edit(101, "Input"),
  ui::with_child(101, ui::set_text("Updated"), ui::enable_window()));
chain.write<0>(parent);
chain.execute_feed();
HWND result = chain.returns().get<decltype(chain)::stage_count - 1>();
```

`with_child` is a movable configuration description, not an executing binder.
It expands to child lookup, its actions (including nested child descriptions),
and parent restoration with GetParent. `link` imports all expanded components
into one static owner's argument buffer, callable buffer and return cache.
There is no captured legacy series executing the nested actions. Flat binder
packs import directly without the description layer. Component lvalues copy;
rvalues move. Descriptions can be copied when their captured components can,
or consumed explicitly with `std::move`.

The resulting owner is still noncopyable/nonmovable and is directly constructed
through guaranteed copy elision. Do not retain views after owner destruction.
This avoids relocating internal reference feeds, rather than pretending raw
byte relocation is safe. No heap indirection is introduced by this layer.

Within a child scope, actions must preserve the target HWND and its parent
relationship; reparenting/destruction or arbitrary non-HWND returns are not
supported. Scope exit requires a live parent. A failure propagates normally,
does not roll back prior native changes, and does not execute later stages.
Use the ordinary legacy API for its broader dynamic policies. Static composition
requires fully bound components and compatible front-feed edges, with no silent
fallback. `run` requires a live initial HWND and a non-void final result.

`write<I>` addresses the flattened arguments, including scope entry/exit slots;
`returns().get<I>` addresses expanded stages, not description positions. For
custom windows, configure the ordinary register/create/show/loop components
before passing them to static `link`. `window()` defaults retain the usual
four stages and message-loop behavior, without a legacy `stage()` facade.

The full static form is `examples/static_ui_form.cpp`, sharing the old form's
source and visual layout. Tests in `examples/static_ui_smoke.cpp` prove two-level
scope expansion, movable descriptions, direct nested-slot writes, repeated
execution, missing-child errors, and the complete default window chain.
See `bench/static_ui_compare_bench.md` for matched workloads, reproducible
commands and measured results. Synthetic overhead improves in this build;
native workflows have not demonstrated a reliable speedup.

## Build And Verify

From the repository root with GCC/MinGW:

```powershell
& G:/mingw64-16.2/bin/g++.exe -std=c++23 -O2 -Wall -Wextra -pedantic-errors -I. examples/winapi_ui_form.cpp -o output/winapi_ui_form.exe -luser32 -lgdi32
./output/winapi_ui_form.exe
./output/winapi_ui_form.exe --auto-test
```

Link GDI32 for the stock GUI font. The normal form stays open until closed;
`--auto-test` opens the real form, triggers a button click and verifies output,
list state and Tab navigation, then exits automatically. Further error/state
coverage is in `examples/winapi_ui_controls_smoke.cpp`, built with the same flags.