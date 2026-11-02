# Shared Argument Stack Experiment

Include `meta_shared_series.hpp` for the separate `link_shared` API. Existing
`function_binder`, `link_f`, and `series_bind` retain their storage and behavior.

```cpp
auto chain = meta_bind::link_shared(
    [](int first, double second) { return first + second; },
    [](std::string const& text) { return text.size(); });
meta_bind::series_initialize(chain, 21, 0.5, "text");
auto result = chain.call<0>();
auto length = chain.call<1>();
chain.arguments().get<0>() = 7;
```

Parameter lists are flattened by `meta_jostream`. One aligned layout covers
all actual stored argument types, across stage boundaries. A
`shared_function_binder<Range, Begin, F, Parameters, StageId>` stores only F;
its `argument_begin` / `argument_end` encode its half-open slot interval.
Each call receives the current owner's range, and the binder expands constant
`get<Begin + I>()` accesses itself. No binder holds an independent stack or
an interior pointer into its owner. Repeated callable types and zero-argument
stages remain distinct via StageId. `get` retains its existing launder behavior.

## Initialization

`series_initialize(series, args...)` requires exactly all argument values in
stage order. It also works with the original `link_f` series. Only the entry
checks emptiness: construction uses direct placement construction without
per-slot live tests or assignment-vs-reconstruction dispatch. Reference slots
retain the original binder convention (`T&` stores T*, `const T&` owns a copy).

The original series rejects any live argument slot. `reset()` does not make
slots empty, so it does not permit initialization again. Ordinary rebinding
still uses `series_bind` or `bind_at`. Failure clears the newly constructed
arguments in reverse stage/slot order; return caches and callables are untouched.
Its existing binder relocation constraints still apply: `link_f` requires
movable or copyable slot types even when constructing an empty binder. The
shared version does not relocate argument slots and supports immovable values
constructed directly from initializer arguments.

The shared version rejects any second successful initialization, including a
zero-argument series. Its successful-construction prefix count is used for
reverse cleanup, not per-slot live checking. On failure it returns to empty
and may be retried. Values are constructed directly from arguments; they need
not be default constructible, assignable, copyable or movable.

## Scope

The shared version is an isolated prototype, not a drop-in replacement for
`function_series`. It offers `call<I>()` and a borrowed `arguments()` view,
not return caches, dynamic stage cursors, automatic return feeding, or
copy/move of the owning series. Guaranteed copy elision permits `link_shared`
to return this owner. Calls before successful initialization throw
`not_fully_bound`; repeated initialization throws `argument_overflow`.
Borrowed views must not outlive the owner. Do not manually destroy or reconstruct
slots. An all-zero-argument owner uses an internal unconstructed placeholder;
it must not be accessed through `arguments()`.

Tests cover reference writeback, owned const-reference strings, move-only
arguments, over-alignment, zero parameters, repeated function pointers,
failed construction cleanup/retry, and old-series initialization regression.

## Measurement

```powershell
g++ -std=c++23 -O2 -Wall -Wextra -Werror -pedantic-errors -falign-functions=64 -falign-loops=32 -I. bench/shared_series_bench.cpp -o output/shared_series_bench.exe
./output/shared_series_bench.exe
```

This GCC/Clang harness rotates three cases over seven rounds and reports their
median with checksum verification. Input and full owner materialization
barriers prevent disappearing stack storage. It includes construction,
initialization, two calls, and destruction. The original cases also contain
their existing return cache and execution bookkeeping; the shared prototype
returns directly. This measures the current implementations, NOT a pure
stack-layout speedup or a benchmark of complete equivalent series APIs.
Compare original bind vs original initialize for the narrower initialization
question. Rebuild with `-DMETA_BIND_STRICT_LAUNDER=1` to test the existing
series access option separately. Do not infer universal gains from one run.

GCC 16.2, Windows x64, -O2 with the alignment flags above (three runs):

| Case | Median ns/op |
| --- | ---: |
| Original bind | 5.990-6.023 |
| Original initialize | 5.978-6.013 |
| Shared initialize | 2.222-2.232 |

The two-stage owner is 96 bytes originally and 48 bytes in the prototype;
the common parameter stack is 32 bytes and each empty callable binder is
1 byte. No default-path initialization gain is established. A separate single
strict-launder run gave 6.824 / 6.385 / 2.242 ns respectively, not a general
claim about launder or a pure layout improvement. Size depends on signatures,
callable captures and alignment, not a fixed factor.