# Win32 Series Lifetime Proof

`window_owner_series_smoke.cpp` is a hidden-window, self-terminating proof using
real Win32 messages and `meta_bind::link_static_heap`. It does not change the UI
factories or the series executor.

## Ownership Contract

1. Namespace-scope `inline constexpr wchar_t[]` objects hold the class name and
   window title. No series owns these strings. C++ guarantees static storage
   duration, not a particular executable section such as `.rodata` or `.rdata`.
2. `registered_class` outlives both series and all windows. The local
   `WNDCLASSEXW` is a registration descriptor, not persistent application state.
   Win32 copies the registration information; the class name remains static,
   and the window procedure is a free function with program lifetime.
3. Before creation, a `unique_ptr<WindowOwner>` owns control state and the
   persistent callback. The create series only borrows its raw pointer.
4. `CreateWindowExW` receives that pointer through `lpParam`. `WM_NCCREATE`
   installs it in `GWLP_USERDATA`, before synchronous `WM_CREATE` invokes the
   callback. No pointer to a series slot or callable is installed in the window.
5. After execution, the HWND is copied out of the return cache. The entire create
   series, including its arena, is destroyed before the function returns
   `created_window { HWND, unique_ptr<WindowOwner> }`.
6. Moving the pointer into the run callable transfers ownership without moving
   the `WindowOwner` object. The run series owns that callable and covers the
   entire message loop. `GWLP_USERDATA` is nonowning and never deletes the owner.
7. `WM_NCDESTROY` clears `GWLP_USERDATA` and the owner's HWND. When the run series
   is destroyed, its unique pointer deletes the owner. If a live HWND remains
   during exception unwinding, the owner's destructor destroys the window while
   its callback and control state are still alive.

This example is single-threaded: creation, message dispatch, ownership transfer,
and destruction occur on the window's creating thread. Moving the series is not
permission to move its window to another thread. Callbacks must not delete their
own owner or retain references to the create series. Exceptions are caught inside
the window procedure rather than escaping across the Win32 callback boundary.

The observation object is test instrumentation outside both series and outlives
all owners. The class-registration guard is also deliberately outside the two
series: the registration action is temporary, but registration lifetime is not.

## Executable Checks

- `WM_CREATE` executes while `CreateWindowExW` is still on the stack and updates
  persistent state through the owner's callback.
- After create-arena destruction, a posted message reads the same state through
  `GWLP_USERDATA`, invokes the callback again, and requests normal closure.
- `WM_NCDESTROY` detaches the pointer before the owner is deleted exactly once.
- Returning `-1` from `WM_CREATE` leaves no HWND and preserves the unique owner
  until the caller releases it.
- An injected run-stage exception destroys the still-live window before deleting
  its owner, and then unregisters the class.
- A two-second window timer makes a stalled normal-path message sequence fail
  rather than silently count as success.

These are focused lifetime/ordering assertions, not a sanitizer proof of every
possible Win32 application or arbitrary user callback.

```powershell
& G:/mingw64-16.2/bin/g++.exe -std=c++23 -Wall -Wextra -Werror -pedantic-errors -O2 -I. examples/window_owner_series_smoke.cpp -o output/window_owner_series_smoke.exe -luser32
& ./output/window_owner_series_smoke.exe
```