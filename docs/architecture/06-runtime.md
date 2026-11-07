# 06 â€” Runtime Architecture

## 1. Runtime ABI Table

The Phase 1 runtime interface is defined in `runtime/include/nirqoba/runtime/`:

| Function | Signature | Purpose |
|---|---|---|
| `nrq_print_string` | `int32_t(const char* utf8, size_t byte_len)` | Writes UTF-8 bytes + `\n` to stdout handle |
| `nrq_runtime_init` | `void()` | Initialises runtime state; GC root boundary in Phase 6.5 |
| `nrq_runtime_shutdown` | `void()` | Flushes stdout buffers; GC cleanup boundary in Phase 6.5 |
| `nrq_runtime_fatal` | `void(const char* message)` | Writes error message to stderr and exits with code 70 |
| `nrq_runtime_version` | `const char*()` | Returns runtime version string (`0.251-beta-phase6`) |
| `nrq_gc_root_register_frame` | `void(void* const* slots, size_t count, const size_t* descriptor)` | Publishes an activation's ordinary roots in one call from a read-only frame descriptor |

## 2. Direct Win32 I/O

The Windows implementation in `runtime/src/io_windows.cpp` deliberately uses `WriteFile` rather than CRT `stdio` (`printf`, `puts`):
- Avoids hidden CRT buffering and internal locking issues.
- Leaves the file handle and buffer management ready for GC heap ownership in Phase 6.5.
- Handles partial writes via a dedicated loop.

## 3. GC Root Publication

Generated code registers roots before any operation that can collect, and the
collector discovers them through the thread's shadow stack.

- **Per-slot registration.** `nrq_gc_root_register` /
  `nrq_gc_root_register_typed` publish one slot with its object size and
  reference layout. They remain the entry points for hand-written native code,
  for a lone root, and wherever the layout is only known at run time.
- **Frame registration.** An activation's root set is a compile-time fact, so the
  lowering emits one `private constant` descriptor per root set and calls
  `nrq_gc_root_register_frame` once with the descriptor and an array of slot
  addresses. The descriptor is interned per module, so functions that register the
  same layouts share one table in `.rodata`, and an activation pays one barrier
  region instead of one per root. The runtime validates the whole table before it
  publishes any slot, keeps the same per-slot duplicate counts, and marks a root
  against an in-flight snapshot exactly as the per-slot entry point does.

### 3.1 Invariants that keep a live object from being swept

Three properties are load-bearing, and each has an artifact that fails if it is
broken:

1. **One description of a root set.** The descriptor words and the roots they
   describe come from a single layout walk, so the table an activation publishes
   cannot disagree with the slots it registers. Every slot declared at any point
   in the activation belongs to that one table, including a slot declared after a
   collection already ran
   (`tests/integration/positive/gc_root_completeness`).
2. **A table states its own length.** Word 0 is the slot count and word 1 is the
   word count, so a truncated descriptor is rejected before the walk can read past
   it, and the marking path marks from the same validated table rather than from
   offsets it recomputed. Publication is all-or-nothing
   (`tests/unit/gc_test.cpp`, the frame registration cases).
3. **Registration is never elided, only described.** A type the lowering reports as
   managed publishes an offset; if its layout walk ever returned nothing, the slot
   is registered at offset 0 rather than skipped. A program with no managed
   storage elides the protocol entirely — no descriptor, no mutator entry, no poll
   (`tests/integration/positive/gc_root_elision_owned`), and every optimization
   level keeps registration (`tests/integration/positive/gc_root_at_O0`).

### 3.2 The safepoint bound

A managed loop polls on its back edge through a countdown in an entry-block slot,
reloading the interval on the polled path only. The comparison is *at or below
zero*, not equality: an equality test only fires on the value the emission
intended, so a counter that reached zero without taking the polled path, or that
wrapped past it, would stop the loop observing collection requests permanently
while the code still looked correct. The bound is that a request is observed
within one interval of crossings, and it holds for every counter value
(`tests/integration/positive/gc_safepoint_bound` asserts the polled back edge and
the comparison in the optimized module).

### 3.5 Lock order and collection progress

The runtime holds four locks and two handshakes. The order is
`collection_cycle_mutex` then `mutator_mutex` then `gc_mutex`, with a shadow stack's
own mutex taken last and never held across a lock higher in the order. Anything that
needs a lock below `mutator_mutex` while already holding `gc_mutex` would invert it,
so no path does.

Where each requirement is enforced:

- **No `gc_mutex` while a fast-barrier region is open, if the collector waits for
  that region.** The collector publishes `Preparing` under `gc_mutex`, releases it,
  and only then waits for barrier depth to reach zero (`begin_cycle` â†’
  `wait_for_fast_barriers`). No collector path holds `gc_mutex` while waiting, which
  is what makes a barrier region that takes `gc_mutex` (a TLAB reservation inside
  `nrq_gc_allocate_rooted`) block instead of deadlock.
- **A mutator never waits for a cycle while registered and unparked.**
  `acquire_collection_cycle` parks before blocking on the cycle lock, because the
  active collector is waiting for exactly that mutator to park.
- **A recursive function reaches a safepoint within the documented bound.** The poll
  is emitted at every loop backedge of a function whose call graph can allocate, and
  a recursive call is a call, so the bound is per activation, not per program
  (`tests/integration/positive/gc_region_recursive`).
- **TLAB publication and transfer-pin draining precede pinned-root enumeration.**
  `prepare_cycle_locked` publishes every parked mutator's TLAB, drains remembered
  references, folds buffered transfer pins into the table, and only then calls
  `enqueue_roots_locked`; `GcTest.BufferedPinsAndTlabObjectsSurviveCollectionBeforeRootEnumeration`
  keeps a buffered pin and a batch-resident allocation alive across a cycle.
- **No mutator leaves the registry with state it still owns.** The thread's cleanup
  publishes its TLAB, drains remembered references and transfer pins under
  `gc_mutex`, and only then removes itself from the registry, so the removal cannot
  drop work the collector would otherwise have seen;
  `GcTest.ThreadExitPublishesPendingStateBeforeRegistryRemoval` exits a thread with
  an unpublished batch and a live root slot and requires every object it held to be
  reclaimed without leaving a phantom registration behind.

### 3.4 Root-liveness audit and cost

The root plan decides, per function, which stack slots and values are registered
with the collector and which stores can skip the write barrier. It has to be
conservative where it cannot prove otherwise, and it runs on every function, so both
its answers and its cost are worth pinning down.

Cases audited against the plan, each with a fixture that asserts runtime output and
the emitted root protocol (`nrq_gc_root_frame_begin` / `nrq_gc_root_register_typed`):

| Case | Fixture |
| --- | --- |
| managed parameter live across a collecting call | `gc_root_audit_values` |
| call result used after another collection point | `gc_root_audit_values` |
| aggregate holding two managed references | `gc_root_audit_values` |
| derived addresses of a managed stack slot | `gc_root_audit_addresses` |
| values loaded from a managed slot | `gc_root_audit_addresses` |
| array whose element layout holds managed references | `gc_root_audit_addresses` |
| loop backedges as collection points | `gc_root_audit_control_flow` |
| a parent activation live across a recursive child | `gc_root_audit_control_flow` |
| explicit `gc.collect()` | `gc_root_audit_values`, `gc_frame_roots` |
| value dead before the next collection point | `gc_root_elision_owned`, `gc_region_transitive` |

Indirect calls are not expressible in the language today. The plan does not depend on
resolving a target: `call_may_collect` treats every callee it cannot prove
collection-free as a collection point, so an unresolved call would be handled
conservatively on that path.

Cost. `--verbose` reports what the analysis did: functions, slot addresses, loads,
liveness probes and address scans. Attribution and load indexing are single passes in
program order (TIR is SSA), and the root decision walks the live set at each
collection point once instead of asking liveness per candidate per point. On a
synthetic function with N managed slots the per-slot rescanning that this replaced
grew four times per doubling (1,847 scans at N=8, 100,407 at N=64); the analysis now
touches each operation a fixed number of times, and the numbers reported for the same
programs are address scans 34/66/130/258 for N=8/16/32/64.

### 3.3 Mutator regions

Every managed value is created, moved and collected inside a mutator region, and the
protocol charges for the boundaries: leaving a region publishes the thread's pending
allocation batch and drains its buffers. A program that leaves a region per helper
call therefore pays per helper call, which is why regions are decided per call graph
rather than per function.

- **Who needs a region.** A function needs one when it holds a managed value, or when
  it calls a function that does, propagated to a fixed point over direct calls.
  Runtime entry points are not propagated: querying or toggling collection needs no
  region of its own. A region need is not a root need — a driver loop that only calls
  an allocating helper opens a region and registers no roots
  (`tests/integration/positive/gc_region_transitive`).
- **Who establishes one.** The entry wrapper the C runtime calls is the outermost
  point of a managed program, so it opens the region and closes it once
  (`tests/integration/positive/gc_region_transitive`), and functions reachable only
  through calls drop their own pairs.
- **Recursion.** A recursive function always has itself as a caller, so coverage is
  computed from the optimistic direction and narrowed: an entry survives only while
  every caller is an establisher or also covered. Direct recursion and mutual
  recursion therefore share the outer region instead of duplicating it
  (`tests/integration/positive/gc_region_recursive`,
  `tests/integration/positive/gc_region_mutual`).
- **What is never removed.** A function that can be entered without a region keeps
  one: `main`, a function whose address was taken, and a function with external
  linkage. A function that reaches no allocation keeps none, and the safepoint poll
  stays in loops of any function that may reach one.

The same poll is why a managed loop is not vectorized: a call the collector can run
through cannot be assumed not to touch the heap, so vectorizing the body would
either drop the poll or let the vectorizer assume a stable heap
(`tests/integration/positive/opt_vectorize_managed_blocked`).
## 4. Production and development configurations

The runtime is built twice from one set of sources. `nirqoba_runtime` is the
production library and the default every build links. `nirqoba_runtime_debug` is
the development library, selected by `--debug-enabled` on `build`, `run` and
`check`. The choice is made at link time: there is no runtime switch, no
environment variable and no hot-path branch, so a production binary contains no
counter, buffer or symbol for anything it does not use.

**One rule decides what belongs to each configuration.** Anything that changes
what a program observes as behaviour stays in production; only recording that no
production code reads is compiled out. Collection semantics, the barrier
handshake, root publication, promotion, reclamation policy and every fatal
invariant check are therefore identical in both libraries, as are
`gc.get_statistics()` and the query surface a program can call
(`memory.get_ref_count`, `gc.page_reuse_count`, `memory.pin_count`, ...). A
development build can be slower; it must never collect differently, and a
production build must never lose a check. A measurement that changed behaviour
would describe a collector that is not the one being shipped.

The classification of every diagnostic mechanism in the runtime, and the rule
that decides which side of this split a new one belongs to, is in
`07-diagnostics-classification.md`.

### 4.1 What the development library adds

- **Event stream.** Allocation, cycle begin/end, promotion, sweep chunk and root
  frame open/close, each with a globally ordered sequence number and a monotonic
  timestamp. Each thread appends to its own fixed ring; a consumer drains them
  with `nrq_gc_debug_drain_events`. A ring that fills drops records and counts
  them (`nrq_gc_debug_dropped_events`) instead of growing or blocking, so tracing
  never turns a workload into a different workload.
- **Allocation-site tags.** With the flag, each lowered managed allocation
  declares its source position and tags the object it produced
  (`nrq_gc_debug_declare_site`, `nrq_gc_debug_tag_allocation`). The site id is a
  stable FNV-1a hash of file, line and column, so any development build maps a
  trace back to the same source line without the compiler carrying a table. The
  runtime keeps the mapping in a fixed, lock-free table, so re-entering an
  allocation site costs one atomic load.
- **Root snapshot.** `nrq_gc_debug_snapshot_roots` reports every registered root
  slot, its layout and the object it currently holds. It reuses the
  reference-diagnostic handshake: it publishes that a reader is active, drains
  the lock-free root-publication regions, then walks the roots. It is a coherent
  view, not a stop-the-world freeze.
- **Slot-index probe counters.** Probe length and rehash/growth/compaction
  counts are read only by tests and by this tooling, so the production library no
  longer records them at all.

### 4.2 Cost in the development build

Nothing is recorded until a consumer sets a mask
(`nrq_gc_debug_set_event_mask`), so an ordinary `--debug-enabled` run pays one
relaxed load of a read-mostly word per candidate event. With a mask set, an
allocation appends to its thread's ring without taking the collector lock, so
the recording path cannot serialise mutators and cannot change the order in
which they acquire the heap. Ring capacity, drop counters and the single
consumer rule are documented in `runtime/include/nirqoba/runtime/gc_debug.h`.

### 4.3 The configuration agreement is a link-time check

A development object calls entry points the production library does not define,
while almost every other symbol the two libraries export has the same name. A
mismatched link would therefore either fail late or, worse, succeed and produce a
program that observes the heap differently from the way it was compiled to. Both
outcomes are link-time facts, so both are enforced there:

- **One identifier per configuration.** `nrq_runtime_abi_v1_production` is
  defined only by the production library and `nrq_runtime_abi_v1_development`
  only by the development library. The compiler emits a call to the one matching
  its own configuration into the program's entry point, so every object
  references exactly one of them, including a program that never touches managed
  memory. Linking against the other library leaves the symbol unresolved
  (`tests/unit/mlir_pipeline_test.cpp` pins which symbol the lowering emits).
- **One marker both libraries define.** `nrq_runtime_abi_configuration_marker`
  has external linkage in each library and is never read. Pulling an object from
  each configuration into one link is then a duplicate definition instead of a
  silent first-definition-wins, which is the only failure mode that would let
  half a program run against a runtime that disagrees with the other half.

The failure modes are deterministic and their messages are actionable: a
development object linked against the production library reports
`undefined symbol: nrq_runtime_abi_v1_development`, and a link that names both
libraries reports `duplicate symbol: nrq_runtime_abi_configuration_marker`.
### 4.4 One value decides the configuration

The three facts a configuration consists of - what the lowering emits, which ABI
symbol the module requires, and which library the link names - come from one
place on each side of the boundary:

- **Compiler.** `--debug-enabled` is read once and mapped to
  `RuntimeConfiguration`; `runtime_abi_symbol()`, `runtime_library_name()` and
  `runtime_library_target()` are derived from that value, and
  `translate_tir_to_mlir` receives the value rather than a boolean per fact.
  There is no call path on which a caller can pick two of the three.
- **Runtime.** `NRQ_RUNTIME_DEBUG_ENABLED` is set `PUBLIC` on the runtime target,
  so the definition travels with the library: anything linking
  `nirqoba_runtime_debug` compiles against the same macro its objects were built
  with, and the production target states `0` instead of relying on absence. The
  macro derives `runtime_debug_enabled`, the ABI symbol name and the library file
  name in one header.
- **Tests.** `GcDebugTest.RuntimeAndCompilerAgreeOnTheConfiguration` compares the
  compiler's enum-derived names against the runtime's macro-derived names in a
  binary that links both, so a rename on one side fails a unit test instead of a
  user's link.

### 4.5 The two configurations must not collect differently

A development build may be slower and may observe more. It may not synchronise
differently, and nothing in the observation may become a correctness dependency.
The list this is checked against:

- **GC synchronization.** One heap mutex, taken in the documented order
  (`mutator_mutex` then `gc_mutex`) and never across a barrier wait.
- **Barriers.** SBAT/SBOT fast regions are entered and exited through the same
  `begin_fast_barrier()`/`end_fast_barrier()` pair, and the per-mutator
  `barrier_depth` that `wait_for_fast_barriers()` drains is incremented once per
  region in both configurations.
- **Root-frame discipline.** Frames are opened with a marker, closed in LIFO
  order, and their slots removed by `root_depth`; the marker and the reserved
  depth are computed from the same descriptor in both configurations.
- **Parking.** Mutators are parked before roots are enumerated, in both
  configurations, and a parked mutator's TLab is published by the same call.
- **SBAT/SBOT and ownership.** TLab ownership, page ownership, pin ownership and
  the handoff of both are unchanged; instrumentation never holds an object.
- **Instrumentation is not a dependency.** Hooks are emitted after the state
  change they describe and touch only the debug ring or the debug site table,
  neither of which the collector, the mutator protocol or the barriered stores
  read. The production build does not call them at all: the lowering emits
  nothing without `--debug-enabled`, so there is no branch to skip. The single
  mechanism both configurations share is `diagnostics_reading_roots()`, which
  exists for reference counting rather than for observation (see
  `07-diagnostics-classification.md` Section 3.2).

The evidence is `scripts/compare_configuration_equivalence.py`: it builds every
integration program twice, runs both, and requires identical exit status, stdout
and stderr. A program whose own output is not reproducible is skipped, because a
difference there could not be attributed to the configuration. On the current
corpus: 119 programs agreed, 2 were skipped (socket cases whose plain link line
lacks `ws2_32` in either configuration, and which the harness only type-checks),
and none disagreed.

This is evidence over a corpus, not a proof; the structural argument above is the
reason to expect it outside the corpus. Both belong in the record, and the first
run already paid for itself: it found a lowering defect that made every
development build of a module with two literal-using functions fail, which is
fixed in `TirToMlir.cpp` by giving the literal table to the module instead of to
each function.
