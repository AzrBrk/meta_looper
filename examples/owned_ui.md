# Owned Native UI

`winapi_ui_owned.hpp` is a small, independent ownership layer over
`winapi_ui_static.hpp` and `meta_bind::link_static_heap`. The existing UI factories,
legacy message loop, and metaprogramming foundations are unchanged.

Run `output/owned_ui_form.exe` for the persistent native greeting composer.
`--auto-test` exercises the visible form and closes it automatically. The older
`winapi_ui_form.cpp` and its static comparison remain unchanged.

## Interface

Class names and titles are static arrays, passed by reference as template
arguments. For example, with the demo's `app_state`, `build`, and `events`:

```cpp
inline constexpr char class_name[] = "MyApp.Greeting";
inline constexpr char title[] = "Greeting composer";

auto form = winapi_ui_owned::create<class_name, title>(
    app_state{automatic, observed}, build, events,
    {.width = 540, .height = 500});
int exit_code = form.run();
```

The normal application code has three separate parts:

- State: a concrete, movable type, including owned dynamic input/model data.
- Build: `bool(context<State>)`, called synchronously during `WM_CREATE`.
  Use `form.ui(ui::place(...), ui::with_child(...))` to build/configure controls.
  Return false to reject creation; throwing also rejects it.
- Events: `std::optional<LRESULT>(context<State>, event)`. Return an empty
  optional for default processing, or a result for a handled message.
  `event` contains `message`, `word`, `parameter`, `id()`, and `notification()`.

Callbacks are concrete callables stored directly in the owner, not type-erased.
They can be functions, lambdas, or movable function objects. The owner's state
and callbacks are complete before `CreateWindowExA` is called. Ordinary messages
can arrive synchronously during creation, so event handlers should only act on
the message types they understand.

`context<State>` provides `state`, `hwnd()`, `ui(...)`, and `close()`. The HWND is
read from the current owner each time, not copied into a long-lived context.
After a reentrant close, `hwnd()` returns null; do not keep using previously
copied HWNDs. `ui(...)` delegates to the existing flat static UI composition.
Sibling creation, child scopes, controls, and actions retain their existing APIs.

The returned window exposes `state()`, `view()`, `hwnd()`, `close()`, and
`run(int show = SW_SHOWNORMAL, bool dialog_navigation = true)`. `run(SW_HIDE)`
supports hidden tests. A window is deliberately neither copyable nor movable;
its heap owner remains at one address. State is still readable after closure
until the window object leaves scope. Running is one-shot and non-reentrant.

## Lifetime and Failures

1. A unique owner is allocated before registration/creation. It owns state,
   callbacks, the registration guard, and the current HWND.
2. A short heap create series registers the class and calls `CreateWindowExA`.
   Its callables only borrow the owner. `lpParam` points to the owner, never to
   a series slot, binder, temporary string, or callable.
3. `WM_NCCREATE` attaches the borrowed pointer to `GWLP_USERDATA` before the
  synchronous build callback, then calls `DefWindowProcA` to complete native
  initialization (including the title). `WM_NCDESTROY` clears both userdata and the live
   HWND. The procedure never deletes the owner.
4. The complete create series and arena are destroyed before returning the
   public window. A separate heap run series owns a callable containing the
   `unique_ptr`. Failure to allocate or initialize this second series also
   destroys the window before releasing its state.
5. Normal close destroys the HWND but keeps C++ state alive. Leaving scope,
   including before `run()` or during exception unwinding, closes any live HWND
   before destroying state and callbacks, then unregisters the class.

Creation, all access, dispatch, and destruction must stay on the creating thread.
Checked wrong-thread access throws `logic_error`; destruction on another thread
terminates. A callback must not destroy its own C++ window/owner object. This is
checked with callback depth and fails fast. Calling `context::close()` from a
callback is supported; it only destroys the native window, not C++ state.
Nested close requests during owned destruction do not call `DestroyWindow`
again. User events are suppressed during owner-initiated native teardown.

No C++ exception escapes the procedure. The first exception is saved; creation
rethrows it after the create arena is released, and running closes the HWND and
rethrows it outside dispatch. `close()` also rethrows saved callback failures.
Unhandled default processing does not target an HWND detached by a reentrant
callback. Failure to destroy, clear userdata, or unregister is **fail-fast**,
not silent release of memory still referenced by Win32.

Class registration is exclusive: an already registered name is an error, even
if its procedure happens to match. One active owned window per class name is
supported. A failed duplicate never unregisters the original registration.
Use distinct static names for independent windows.

`WM_NCCREATE`, `WM_CREATE`, `WM_CLOSE`, `WM_DESTROY`, and `WM_NCDESTROY` belong to
the wrapper, not the event callback. It does not post `WM_QUIT` on destruction.
Its thread-wide loop stops when its live HWND detaches. An external `WM_QUIT`
is consumed, its exit code returned, and the owned window closed. This is a
single top-level-window loop, not a multi-window application dispatcher.
During a run, detachment also posts a thread `WM_NULL`: synchronous sent messages
can destroy a window inside `GetMessage` without otherwise returning from that
wait. This wakeup ensures the next live-HWND check runs. An unused `WM_NULL` may
remain in the thread queue after a queued close; it is harmless and is not a
quit signal. Failure to post the wakeup is fail-fast.

The older `message_loop()` is intentionally not reused: it destroys a cached
HWND and unregisters a class itself. Combining that cleanup policy with this
owner would duplicate responsibilities and risk stale-handle use. The owned
loop reuses the existing show action and dialog navigation, while observing the
owner's live HWND before each dispatch step.

## Resources and Limits

- Class/title arrays must have static storage duration and remain unchanged.
  The examples use namespace-scope `inline constexpr char[]` definitions.
- A context, state reference, and returned HWND are borrowed views, not lifetime
  extensions. Do not retain them beyond the owner or manipulate userdata.
- Win32 controls copy their labels/items. Dynamic user/model text belongs in
  application state. Child HWNDs belong to the parent; do not independently
  unregister this class or transfer its HWND to another thread.
- User-created GDI objects, fonts, handles, and other resources remain the user's
  RAII responsibility. The wrapper only borrows a system cursor/background brush.
- Current controls use ANSI Win32 APIs and explicit pixel geometry, matching the
  existing UI layer. Unicode, DPI-responsive layout, resize management, multiple
  coordinated top-level windows, and arbitrary foreign message pumps are not
  supplied here. No performance claim is made.

## Validation

```powershell
& G:/mingw64-16.2/bin/g++.exe -std=c++23 -Wall -Wextra -Werror -pedantic-errors -O2 -I. examples/owned_ui_smoke.cpp -o output/owned_ui_smoke.exe -luser32 -lgdi32
& ./output/owned_ui_smoke.exe
& G:/mingw64-16.2/bin/g++.exe -std=c++23 -Wall -Wextra -Werror -pedantic-errors -O2 -I. examples/owned_ui_form.cpp -o output/owned_ui_form.exe -luser32 -lgdi32
& ./output/owned_ui_form.exe --auto-test
& ./output/owned_ui_form.exe
```

The focused smoke checks synchronous creation, move-only owned state, dispatch
after create-arena release, normal and callback-initiated close, rejected build,
create/event exceptions, caller exceptions before running, close during create,
wrong-thread access, repeated-run rejection, registration collision, external
quit, native title initialization, and class removal. A window timer detects a
stalled dispatch sequence. A separate cross-thread `SendMessageTimeoutA` test
checks synchronous close while waiting, with an event-based bounded watchdog.

The form auto-test checks synchronous build, post-create state access, editable
input, two Apply actions, combo/check changes, history/status, Tab navigation,
normal `WM_CLOSE`, state destruction, and class removal. These are concrete
lifetime/ordering checks, not sanitizer proof of arbitrary user callbacks.