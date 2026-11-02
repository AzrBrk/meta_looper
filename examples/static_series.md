# Independent Static Function Series

`meta_static_function_series.hpp` provides `meta_bind::static_function_series<F...>`
and the deducing factory `meta_bind::link_static(f...)`. It is independent of
the existing series implementations. Include the new header directly.

```cpp
#include "meta_static_function_series.hpp"
#include <string>

int main() {
  auto series = meta_bind::link_static(
      [](int input) { return input + 1; },
      [](int first, int scale) { return first * scale; },
      [](int input) { return std::to_string(input); });
  series.initialize(2, 0, 4, 0);
  series.execute_feed();
  auto [incremented, scaled, text] = series.returns();
  return incremented == 3 && scaled == 12 && text == "12" ? 0 : 1;
}
```

## Storage and Lifetime

The default inline owner contains separate naturally aligned byte buffers for flattened
arguments, callable objects, and cached returns. Layout calculations use the
existing aligned storage primitives. There is no per-slot allocation by the
infrastructure; stored objects may allocate internally.

Construction of the owner only constructs callable objects. Call
`initialize(args...)` once to directly construct argument slots, then
value-initialize every return cache. A supplied prefix is allowed: remaining
value slots are value-initialized. Missing non-default-constructible slots and
missing non-const reference arguments are compile-time errors. Full initialization
does not require argument slots to be default-, copy-, or move-constructible when
they can be constructed directly from the supplied inputs.

If argument or cache construction throws, all completed constructions are
destroyed in reverse order. The owner remains uninitialized and can be retried.
If callable construction throws, completed callable slots are also destroyed.
Successful initialization makes every argument and return slot live until owner
destruction. A zero-argument layout has one live, internal empty placeholder;
`total_args` remains zero. Its placeholder is not a user argument.

Copy/move construction and copy/move assignment of the inline owner are **deleted**.
`link_static` returns a prvalue using guaranteed copy elision. This is not a
relocatable owner; views and cached references must not outlive it. In particular,
reference feeds may store pointers to its own cached values.

## Explicit Heap Arena

`meta_bind::heap_static_function_series<F...>` and
`meta_bind::link_static_heap(f...)` use the same executor with a different storage
policy. `link_static`, the inline type, and all existing UI factories retain their
default inline storage. The heap bridge is
`meta_bind::link_static_heap_binders(binders...)`; it imports bound components
under the same signature, lvalue-copy/rvalue-move, validation and feed rules as
`link_static_binders`, returning an already initialized heap owner.

```cpp
auto first = meta_bind::link_static_heap(
  [] { return 8; }, [](int& value) { return ++value; });
int initial = 0;
first.initialize(initial);
first.execute_feed();
auto view = first.returns();
auto next = std::move(first);
next.call<1>();
assert(view.get<0>() == 10);
```

The infrastructure makes exactly one allocation per heap owner: one concrete
over-aligned aggregate containing a byte array. A `std::unique_ptr` owns that
aggregate and uses matching typed `delete`, including aligned deletion when
required. The aggregate is default-initialized directly by `new arena`, without
a temporary aggregate or a value-initialized large stack buffer. Callable
factory arguments are forwarded directly into slots, not passed through a
by-value callable pack. Callable expressions supplied by the caller can still
create their own temporaries. Stored objects may perform their own allocations.

The arena regions are ordered **callables, arguments, return caches**. Existing
`aligned_slot_layout` facilities calculate both each region's internal layout
and the complete layout of its aligned byte-buffer types. No recursive type-list
layout calculator or per-region owning pointer is added. The arena's alignment
is the maximum region alignment. Slot construction uses the standard
`new (void*) T(...)` placement form: this constructs in existing storage and
**does not allocate**. Slot destruction and arena release are separate: only
constructed slots are destroyed, then the owning pointer releases the allocation.
There is never an individual `delete` for a slot.

Heap owners are noncopyable and `noexcept` movable. Moving transfers ownership
without moving any callable, argument, or return-cache object. Source live counts
and initialized state are reset. Existing views and references, including feed
pointers into the owner's own cache, keep their addresses and follow the new
owner. Move assignment first destroys the destination's old caches, arguments
and callables, then releases its old allocation and adopts the source allocation.
Self-move assignment is a no-op. A moved-from owner rejects all operational
methods, including `initialize` and empty execution/write operations, with
`error::not_fully_bound`; it can be destroyed or assigned another owner.

Allocation failure propagates `std::bad_alloc`. Callable construction failure
destroys completed callables and releases the arena. Argument/cache construction
failure rolls back completed slots but retains the arena and callables for an
initialization retry. Import failure destroys completed imported slots and
callables before releasing its arena. These guarantees do not make throwing
calls, assignments, feeds, or moves from imported source objects transactional.

Views into a destination's previous arena become invalid on move assignment;
all views become invalid when the owner currently holding their arena is
destroyed or has that arena replaced. Moving an arena to a new owner alone does
not invalidate them. This is a **single-series arena**, not a shared arena or
cross-window allocation pool; no UI heap policy or pooling is introduced here.

The heap smoke test checks 64/128-byte alignment, exact region offsets, a small
owner with an 8 KiB callable, move-stable views and cache reference feeds,
initialized destination cleanup, self-move, moved-from rejection, move-only
objects, binder imports, construction rollback/retry and allocation failure.
Its isolated test executable replaces aligned allocation/deallocation to count
over-aligned arenas; it also checks object lifetime counters. This is scoped
lifetime/allocation testing, not a sanitizer-backed memory-safety guarantee.

```powershell
& G:/mingw64-16.2/bin/g++.exe -std=c++23 -Wall -Wextra -Werror -pedantic-errors -O2 -I. examples/heap_static_series_smoke.cpp -o output/heap_static_series_smoke.exe
& ./output/heap_static_series_smoke.exe
& G:/mingw64-16.2/bin/g++.exe -std=c++23 -Wall -Wextra -Werror -pedantic-errors -O0 -I. examples/heap_static_series_smoke.cpp -o output/heap_static_series_smoke_O0.exe
& ./output/heap_static_series_smoke_O0.exe
```

## Arguments and Calls

- `write<Begin = 0>(args...)` assigns consecutive **live** slots, without
  destroying or reconstructing them. Empty writes and writes ending at the last
  argument are valid. Out-of-bounds static ranges are rejected by constraints.
- `argument<I>() &` returns the exact stored slot reference.
- `arguments() &` returns a type-safe aligned range view by value. Structured
  bindings refer to the live slots, not copies of their contents.
- `call<I>()` invokes a statically selected callable. Non-void results are assigned
  into cache slot I and returned as a stable reference. Void calls return void.
- `returns() &` returns a type-safe aligned range view over all cache slots.
  Void stages retain an empty placeholder, so the number and order of structured
  bindings always match the number and order of stages.
- `execute<Begin = 0>()` invokes the suffix of stages in order, updating caches.
  `Begin == stage_count` is a checked no-op.
- `total_args` and `stage_count` expose the flattened argument count and stage count.

These operations require successful initialization and otherwise throw
`meta_bind::exception(error::not_fully_bound)`, including empty writes and empty
execution suffixes. Initializing twice throws `error::argument_overflow`.

Parameter storage and invocation follow the existing binder mapping:

| Parameter | Stored slot | Call argument |
| --- | --- | --- |
| `T` | `T` | lvalue, or rvalue if not copy-constructible |
| `T&` | `T*` | dereferenced pointer |
| `T const&` | owned `T` | reference to the owned copy |
| `T&&` | owned `T` | rvalue |

Reference initialization/writes accept compatible lvalues, `std::ref`, or pointers
under the binder rules. Their referents must remain alive; pointers must be valid
and non-null when invoked. `argument<I>()` exposes the pointer for a `T&` slot,
not the referent. Move-only value arguments and rvalue-reference arguments may
be consumed by calls; assign a replacement before reusing a consumed value.

Callable signatures must be deducible using the existing function traits
(ordinary/non-overloaded call operators or function pointers, including noexcept
function pointers). At least one callable is required. Generic/overloaded
callables need a non-generic adapter. All buffer layouts retain the aligned-range
type-count limits.

## Feed Policy

`execute_feed<Begin>()` invokes each selected stage, then assigns its cached result
to the **first argument slot of the immediately next stage**, before invoking that
next stage. It never feeds from a stage before Begin. Other argument slots remain
unchanged. A void producer or zero-argument consumer disables that edge; it does
not search across stages. An empty suffix does nothing after checking state.

Assignable lvalue caches are copied, not unconditionally moved. A non-const
reference consumer binds to the cached object. A value consumer requiring an
rvalue (for example `std::unique_ptr`) moves from the cache. Incompatible feed
edges are compile-time errors; use `execute` for independent stages.

Reference-returning functions are rejected at compile time; an explicit value
adapter is required rather than silently converting reference semantics to ownership.
Non-void caches must be default
constructible and assignable from the invocation result. Void caches have an
empty placeholder. Cache construction and argument construction have rollback;
`write`, callable execution, cache assignment, and feed assignment provide only
the stored type's normal exception guarantee, not a transactional rollback of
earlier assignments or calls.

## Importing Bound Components

`meta_bind::link_static_binders(binders...)` imports fully bound ordinary
`function_binder` objects and returns an **already initialized** static owner:

```cpp
auto component = meta_bind::bind_as<int(int, int)>(
  [](auto input, int scale) { return input * scale; });
component.bind_a_lot(3, 4);
auto series = meta_bind::link_static_binders(component);
series.write<0>(5);
series.execute_feed();
int result = series.returns().get<0>(); // 20
```

The bridge extracts each binder's `callable_type`, `return_type`, and public
`argument_types`. Even a generic or overloaded callable works when its original
binder has an explicit signature. Only the callable is retained, not a binder
or its old stack. All actual live argument slots are constructed consecutively
in the new owner's unified stack using the existing pipe/cursor dispatcher.
Reference slots copy their stored pointers without taking another address;
const-reference parameters retain their owned-value behavior. No byte copying
of objects or parameterless per-binder executor is involved.

Lvalue binders, including const lvalues, copy both callable and owned slots;
they are never implicitly consumed. Explicit rvalue binders move those objects.
Noncopyable configuration therefore requires an explicit move. The source
binding cursor does not matter. All sources are checked through `live_arguments`
for complete binding **before any source callable or slot is consumed**.
An incomplete source throws `error::not_fully_bound`. Construction failure
destroys all completed destination slots and callables; moved sources have only
their types' normal moved-from guarantees, not transactional rollback.

Every non-void producer followed immediately by a nonempty consumer requires
the consumer's runtime `front_feed()` flag to be true. Unsupported append/cursor
policies throw `std::invalid_argument` before consuming sources. Void producers
and zero-argument consumers disable only that edge and impose no feed flag.
This is a fixed first-slot policy snapshot, not an emulation of binder cursors.
Execute with `execute_feed()`; `execute()` instead runs the imported defaults
independently. The inline owner remains noncopyable and nonmovable, with the same
cache and feed type restrictions as native `link_static`.

## Actual Pipe Integration

The path is owner -> complete argument layout -> pair dispatcher -> slot operation.
For at least two inputs, the dispatcher runs a real
`meta_pipe<index_sequence_istream<pair_count>>::all_to<main>::fork<branch>::run<>`.
Both receivers map the same finite source index to a `pair_frame`. Its cursors
come from `Layout::cursor<Begin + 2 * pair>` and the adjacent slot. These are
**absolute offsets into the complete layout**, not offsets recomputed separately
for each lane. Padding and a nonzero Begin therefore retain the original layout.
The ordinary `for_each` receives the entire forwarded runtime argument pack
as extra callback arguments. The callback runs the main slot operation followed
by the branch operation, passing that pack to each; each slot lambda selects
only its own input using the existing args_at-style protocol. Forwarding the
pack does not itself move its elements; only the selected construction or
assignment consumes an input.

Zero inputs instantiate no pipe or cursor. One input uses the tail cursor only.
An odd tail is dispatched separately with `if constexpr`, so an absent second
cursor is never instantiated. The same dispatcher handles direct construction
and assignment. Selection of forwarded runtime inputs uses the existing
finite looper `for_each_forward` protocol with a local selection condition,
not a value-pack container or a dependency on example headers.

There are real integration costs: the pipe probes receiver initialization with a
no-arguments sentinel, so both receivers must reject that probe. The public fork
API uses two small receiver adapters rather than directly accepting a pair of
layout cursors. Repeated argument selection can add compile-time work and
should be profiled for very large packs before introducing another abstraction.
Execution uses the existing finite index stream and `meta_iterator`; callback
indices must come from the receiver (`to_t`), since the generator (`from_t`) has
already advanced. This implementation changes none of those core protocols.

## Validation

From the repository root:

```powershell
& G:/mingw64-16.2/bin/g++.exe -std=c++23 -Wall -Wextra -Werror -pedantic-errors -O2 -I. examples/test_static_series.cpp -o output/test_static_series.exe
$env:PATH = 'G:/mingw64-16.2/bin;' + $env:PATH
& ./output/test_static_series.exe
& G:/mingw64-16.2/bin/g++.exe -std=c++23 -Wall -Wextra -Werror -pedantic-errors -O2 -I. examples/static_binder_bridge_smoke.cpp -o output/static_binder_bridge_smoke.exe
& ./output/static_binder_bridge_smoke.exe
```

The test covers four-slot pairs, three-slot odd tails, nonzero Begin, strings,
zero/one argument paths, default suffix initialization, non-default and immovable
direct construction, 64-byte alignment and padding, references, move-only inputs
and callables, stable return references, structured bindings including void,
execution suffixes, copy/move/reference feeds, skipped feed edges, initialization
checks, and argument/cache construction rollback with successful retry.