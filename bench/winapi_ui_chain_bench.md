# WinAPI UI Chain Benchmarks

## Build And Run

From the repository root in PowerShell:

```powershell
./bench/run_ui_bench.ps1 -WithImGui
```

The script defaults to GCC at `G:/mingw64-16.2/bin/g++.exe` and an existing
Dear ImGui checkout at `G:/imgui-master`. Override these with `-Compiler` and
`-ImGuiRoot`. ImGui is optional; omit `-WithImGui` to build only the main bench.
No third-party source is copied into this repository.

The main executable prints results, then opens a real GDI window using
`winapi_ui::window()`. The window closes after five seconds, or on a key press,
click, or close command. Its client area contains all 25 measured rows and bars.
Painting validates visibility and a bar pixel. The minimum window size keeps
the table readable.

```powershell
./output/winapi_ui_chain_bench.exe --no-display
./output/winapi_ui_chain_bench.exe --close-on-paint
./output/imgui_frame_bench.exe
```

The corresponding script switches are `-NoDisplay` and `-CloseOnPaint`.
An invalid checksum, failed lifecycle, or failed display validation exits nonzero.
The script temporarily adds the compiler directory to PATH for runtime DLLs.

## What Is Measured

- **A, synthetic:** four/eight unsigned integer transforms, checked against a
  direct reference. Runtime inputs and outputs cross compiler barriers.
  Series objects additionally cross an object-materialization barrier after
  binding, so a locally reconstructed object cannot disappear completely.
  This measures materialized binder storage, not the best possible elimination
  of an entirely local expression. The barrier emits no instructions but affects
  optimization. Reuse includes reset and input binding; rebuild includes creation,
  execution and destruction. The `_continue` row computes/binds every stage's
  reference input first, since `_continue` does not forward results. It is not
  the same workload as `_bind_continue` or `drive`; do not rank these APIs by it.
  The `static entry+feed` row uses `stage<0>().bind_at<0>(input)` and
  `at<0>()._bind_continue()`, with the same reset and object barrier as the
  dynamic-entry `reset+feed` row. It always starts at stage zero, not at a
  saved execution cursor.
- **B, lifecycle:** register class, create a hidden 320x180 popup, call
  ShowWindow(SW_HIDE)/UpdateWindow, destroy, and unregister. Every iteration
  registers successfully; the class is not left registered between iterations.
  A test WNDPROC avoids posting stale WM_QUIT messages. UI component paths retain
  their actual string ownership/copy behavior. The `winapi_series` path has a
  separate module stage (five stages instead of four). Per-path failure checks
  are included; this is an end-to-end comparison, not isolated dispatch timing.
- **ImGui, separate executable:** hidden real Win32 window plus D3D11 renderer,
  hardware first and WARP fallback. Reports initialization, CPU construction of
  200 text rows, CPU driver submission/Present, and teardown independently.
  Submission is not GPU execution time. Its 640x3072 render target keeps 200 rows
  in view; this is intentionally a different rendering workload from the GDI
  results window. It does not measure ImGui window lifetime against WinAPI
  window lifetime and is not an apples-to-apples framework speed ranking.

`std::function` and virtual handlers are dispatch-shape baselines, not real UI
frameworks. Dear ImGui is the actual third-party framework reference.
Reported bytes are inline chain/handler-array sizes, excluding shared contexts,
heap nodes and the measurement harness; they are not total memory consumption.
`ns/stage` is simply chain latency divided by stage count, not independent-call
throughput or a per-stage incremental-overhead estimate. The lifecycle table
prints zero in that column because kernel/window work dominates.

## Sampling

The main harness calibrates each path to approximately 50 ms, then collects five
samples, rotating traversal order each round. It reports best and median;
ratios and GDI bars use medians. Calibration and correctness checks are outside
the samples. A single steady-clock query surrounds a batch, not each invocation.
GCC builds use `-O2 -falign-functions=64 -falign-loops=32`. The ImGui harness uses
32 warmup frames and 256 measured frames and reports its own statistics.

## Observed Locally

Initial baseline: Windows, GCC 16.2, optimized build, three consecutive runs on 2026-11-02,
before the static-entry row and aligned-stream reset were added.
Ranges below are the per-run medians, not confidence intervals.

| Path | Four stages, ns/chain | Eight stages, ns/chain |
|---|---:|---:|
| Direct | 1.509-1.522 | 3.160-3.170 |
| Series reset + feed | 7.547-7.816 | 13.518-13.576 |
| Series reset + drive | 6.597-6.801 | 12.954-13.030 |
| Series rebuild + feed | 7.582-7.873 | 22.901-23.153 |
| std::function | 5.913-5.975 | 13.436-13.505 |
| Virtual handlers | 5.911-5.921 | 13.392-13.555 |

Hidden lifecycle medians were approximately 1.5-1.8 ms across paths. Differences
changed across runs and cannot isolate a nanosecond-level dispatch cost. The
eight-stage series and erased baselines are effectively tied in these samples;
the four-stage series is not faster than those baselines. Do not extrapolate
these results to arbitrary binders, chain lengths, compilers or hardware.

Three ImGui runs reported CPU-frame means of 64.5-65.1 us, submission means of
195.0-219.0 us, and initialization of 208-255 ms. These are context only, not a
claim that the UI wrapper is a multiple faster than a renderer. Third-party
Win32 backend function-pointer cast warnings remain in its own source; both new
benchmark translation units pass strict syntax checks.

## Static Entry Follow-Up

After switching UI fixed slots to `set<Enum>()`, UI entry paths to
`stage<0>().bind_at<0>()` plus `at<0>()`, and series reset to an existing
`meta_aligned_iterator` receiver/type-input loop, three further optimized runs
gave the following per-run median ranges:

| Path | Four stages, ns/chain | Eight stages, ns/chain |
|---|---:|---:|
| Dynamic entry reset + feed | 6.664-6.717 | 13.870-14.045 |
| Static entry reset + feed | 6.537-6.795 | 13.262-13.354 |

The four-stage difference is not stable. The eight-stage static entry is
modestly faster in this batch (about 4-6%), not a general speed guarantee.
Replacing the reset recursion is primarily reuse of an existing typed traversal,
not evidence of an independent runtime speedup. Measurements from different
binary layouts should not be subtracted as an isolated optimization cost.