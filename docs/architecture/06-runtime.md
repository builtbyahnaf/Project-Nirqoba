# 06 â€” Runtime Architecture

## 1. Runtime ABI Table

The Phase 1 runtime interface is defined in `runtime/include/nirqoba/runtime/`:

| Function | Signature | Purpose |
|---|---|---|
| `nrq_print_string` | `int32_t(const char* utf8, size_t byte_len)` | Writes UTF-8 bytes + `\n` to stdout handle |
| `nrq_runtime_init` | `void()` | Initialises runtime state; GC root boundary in Phase 6.5 |
| `nrq_runtime_shutdown` | `void()` | Flushes stdout buffers; GC cleanup boundary in Phase 6.5 |
| `nrq_runtime_fatal` | `void(const char* message)` | Writes error message to stderr and exits with code 70 |
| `nrq_runtime_version` | `const char*()` | Returns runtime version string (`0.235-beta-phase6`) |
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
