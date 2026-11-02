# Full Static UI Comparison

## Reproduce

```powershell
./bench/run_static_ui_compare.ps1
```

The script builds with GCC 16.2, C++23, `-O2 -Wall -Wextra -Werror
-pedantic-errors -falign-functions=64 -falign-loops=32`, then runs three times.
Override `-Compiler` or `-Repeats` as needed. Logs are generated at
`output/static_ui_compare_1.txt` through `output/static_ui_compare_3.txt`.
Each run reports median/min/max of seven batch samples with rotating row order,
iteration counts, owner bytes and validated checksums. There is no message-loop
wait or visible presentation inside the timed workloads.

## Matched Workloads

- Synthetic: four/eight `HWND(HWND, std::string, uint32_t)` transforms. Handles
  are arithmetic tokens, never sent to WinAPI. Both paths use the same callable,
  default string, salt, fully initialized arguments, and per-stage return caches.
  All outputs are checked against integer reference calculations outside timing.
- Reuse: input assignment plus execution, with legacy cursor reset where needed.
  Construction/destruction are outside the timer. The static owner needs no reset.
- Build: the complete default-bound component construction, chain assembly,
  input assignment, execution and destruction are timed for every iteration.
- Native reuse/build: three child scopes updating edit text/enabled state,
  checkbox state, and combo selection. Root/control setup is outside the timer.
  Build reconstructs the action chain on each iteration; reuse keeps it alive.
  Final text, enabled state, check, item count and selection are verified.
- Native lifecycle: create a hidden root, edit, checkbox and combo; populate two
  combo items; execute the same actions; verify state; destroy the root and all
  children. Construction, validation and destruction are included on both sides.
  Built-in STATIC root class avoids registration-state differences and WM_QUIT.

Both implementations cross identical whole-object materialization barriers
after input assignment and after execution, and scalar input/output barriers.
These are compiler barriers, not CPU fences. They require real storage/cache
updates and deliberately limit whole-expression optimization.

The baseline always calls `winapi_ui::link` and its legacy nested `with_child`;
it never uses the partially static `winapi_ui::run`. The new path always uses
`winapi_ui_static::link` and flattened static child scopes. Both perform the
same GetDlgItem/GetParent and control operations. A final GetParent action is
explicitly added inside each legacy child scope to match static scope exit;
this is an operation-matched baseline, not a byte-for-byte unchanged old app.
Different scope bookkeeping/caching is part of the implementation comparison.

Bytes are inline owner sizes, not peak stack usage, allocation counts, native
window memory, or total memory consumption. Native lifecycle bytes sum its
control and action owners; native action rows show only the action owner.
Strings/items may allocate. Static description construction uses temporary
ordinary binders but no legacy nested executor or heap-owned static owner.

## Observed Results

Windows x64, GCC 16.2, 2026-11-02. Each cell reports the median of the three
per-run medians, followed by their minimum/maximum in parentheses. Ranges are
observations, not confidence intervals. These are final HWND-token results,
not the earlier uint32-only or pre-fast-path exploratory measurements.

| Workload | Legacy | Static | Observation |
| --- | ---: | ---: | --- |
| Four-stage reuse, ns | 21.068 (20.211-21.119) | 18.354 (18.185-19.456) | 12.9% lower time |
| Four-stage build, ns | 134.177 (131.993-135.510) | 91.442 (90.822-92.306) | 31.9% lower time |
| Eight-stage reuse, ns | 64.168 (63.987-64.678) | 53.562 (53.055-54.458) | 16.5% lower time |
| Eight-stage build, ns | 294.394 (287.048-303.603) | 199.638 (198.161-201.763) | 32.2% lower time |
| Native action reuse, us | 19.156 (18.723-19.743) | 19.571 (18.508-21.811) | No reliable gain |
| Native action build, us | 20.050 (19.217-20.419) | 20.876 (19.014-22.573) | No reliable gain |
| Native lifecycle, ms | 5.010 (5.009-5.115) | 5.154 (4.999-5.227) | No reliable gain |

Owner sizes: four-stage 336 -> 288 bytes; eight-stage 656 -> 544 bytes;
native action 464 -> 288 bytes; native control+action 1048 -> 768 bytes.
All three runs passed checksums and native state checks.

The static path measurably reduces materialized synthetic chain overhead in
this build. WinAPI work dominates the native workflows and sample spread is
large; there is no evidence here of a faster real UI. Small native differences
must not be promoted into a speed claim. This does not measure renderer speed,
user-perceived responsiveness, or all possible callable/string/chain shapes.

An initial prototype copied/moved ordinary flat components through composition
descriptors and made rebuild slower. The shipped `link` directly imports flat
binders; only nested descriptions need composition expansion. This matters for
construction cost and is not an owner relocation or low-level engine change.

## Usable Form And Regression

```powershell
& G:/mingw64-16.2/bin/g++.exe -std=c++23 -O2 -Wall -Wextra -Werror -pedantic-errors -I. examples/static_ui_form.cpp -o output/static_ui_form.exe -luser32 -lgdi32
./output/static_ui_form.exe --auto-test
./output/static_ui_form.exe
```

The form shares its exact source/layout with `examples/winapi_ui_form.cpp`.
Only composition namespace and startup result access differ. The static build
uses static startup, control construction, nested event actions, focus and
message-loop composition. The auto-test sends native clicks, checks greeting
and history, and checks Tab navigation; timing it is intentionally avoided.