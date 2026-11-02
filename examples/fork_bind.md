# Dual Aligned Fork Experiment

`fork_bind_smoke.cpp` tests the existing two-chain fork without changing the
pipe engine or introducing multi-fork.

## Schedule

The source is `meta_repeat_istream<arguments>`, not a finite list. Two
`meta_aligned_iterator<>` adapters project `arguments::first` and
`arguments::second` from each shared frame. The first lane starts at zero;
the second lane starts after the first argument's cursor, so its double is
aligned exactly as the binder's second slot.

`node_con<receive_count<N>, Advance>` limits each lane to N receives. Only the
main lane advances the shared source. The next pre-generator window closes
each node, leaving its final receiver intact. `wait_main<N>` keeps main alive
until its node mask is zero, checks the final main cursor offset, then stops.
The executable statically verifies one and two rounds using this infinite
source. N must be positive in this experimental fixture.

For N=1, final main/branch cursor types are statically checked against
`slot_cursor_t<0/1, type_safe_aligned_range<0, int, double>>`. The shared callback
then obtains each address using `view.main().address_of(storage.data())` and
the branch equivalent, explicitly casts to the cursor's `type*`, and assigns
21 and 0.5. `storage` is the binder's `live_arguments()` view obtained
after an initial `bind_a_lot(0, 0.0)` constructs the slots. A real invocation
must return 21.5; repeated binding/invocation must return 8.25.

The two aligned cursors directly assign into binder-owned live objects;
there are no `bind_at` calls on this write path. Initial binding owns construction
and liveness bookkeeping, while destruction remains with the binder. Direct
assignment leaves the binding cursor and live bitmap unchanged. Do not retain
the borrowed view across binder clear, assignment, move or destruction, and
do not manually destroy/reconstruct its slots. The writes occur in one statically
expanded callback, not on concurrent threads.

`fork_write_strict` and `fork_write_raw` provide otherwise identical externally
visible functions for GCC assembly comparison. The former uses `get` with
launder; the latter uses `address_of` plus a caller-owned `reinterpret_cast`.
The caller is responsible for type, alignment, lifetime and pointer validity.
This small scalar case is not a
representative benchmark of a heterogeneous function series.

## What This Does Not Prove

The current pipe main condition receives the main receiver and only the main
live mask during its invocation. It does not receive the fork pair or a joint
completion bitmap. Branch completion is therefore checked in the final fork
view, not independently awaited by main. Equal counts/shared source make the
two schedules match in this test. Unequal-latency branches and arbitrary join
are NOT established by this experiment.

N=2 verifies independent cursor progression/counting only. Its two lanes then
point at successive int and double slots in their own lanes; that is not a
general interleaved four-argument binder layout. Actual binder writes are
intentionally limited to the proven N=1 two-argument layout.

Before adding more forks, the next design question is whether main should
receive a combined completion view, how waiting interacts with main death,
and when an aggregate result becomes eligible for a callback. That is a pipe
protocol change, not something demonstrated by this fixture.

## Run

```powershell
g++ -std=c++23 -O2 -Wall -Wextra -Werror -pedantic-errors -I. examples/fork_bind_smoke.cpp -o output/fork_bind_smoke.exe
./output/fork_bind_smoke.exe
```