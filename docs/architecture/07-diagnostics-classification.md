# Diagnostic Mechanisms and the Configuration Split

The runtime is built in two configurations (see `06-runtime.md`). This document
classifies every diagnostic mechanism in it, states the rule that put each one
where it is, and defines what a new mechanism has to satisfy before it can join a
class.

A "diagnostic mechanism" here means anything that exists to report something:
an invariant check, a statistic, a counter, a trace. The classification is not
about how much a mechanism costs. It is about what breaks if it is missing.

## 1. The rule

Three questions, asked in order. The first one that answers "yes" decides the
class.

1. **Does a program's behaviour depend on it?** A wrong answer here is memory
   corruption, a crash, a wrong result, or a number the program itself reads.
   → **Essential** or **Accuracy-critical**: always enabled, in both
   configurations.
2. **Does any correctness decision or synchronization protocol depend on it?**
   Removing it would make a check unsound rather than merely unreported.
   → **Essential**: always enabled.
3. **Can only a developer tool observe it, and does its absence change nothing
   but what that tool sees?** → **Pure telemetry**: debug-only.

A mechanism that fails questions 1 and 2 but is nevertheless cheap is still
telemetry: cheap is not a reason to keep it in the product, and "we might want it
later" is not either. A mechanism that answers "yes" to question 1 is never
debug-only, however expensive it is; the answer to expense is a cheaper exact
mechanism, not a quieter one.

## 2. Essential correctness mechanisms (always enabled, both configurations)

These protect memory safety and heap integrity. Every one of them fails the
process through `nrq_runtime_fatal` when its condition does not hold, because
continuing would operate on a heap whose invariants are already broken. None of
them is derived from another, and none is reachable only through a debug path.

| Area | Checks | Why it cannot move |
|---|---|---|
| Root descriptors | invalid descriptor, out-of-range pointer offset, empty root frame, null slot, invalid array-root descriptor, storage already registered, unregistered-storage initialisation | The collector traces what the descriptor describes; a bad descriptor makes marking chase a non-reference and free a live object |
| Root bookkeeping | slot registration index failed to grow, incompatible re-registration, unregistration underflow, slot-index inconsistency, frame LIFO/marker validation | These are the invariants that keep a slot's layout memo and its registrations in agreement; a mismatch means a root is described as something it is not |
| Mutator protocol | enter/leave balance, nesting-depth overflow, mutator registry lost a thread, registration failures | The collector parks mutators and publishes their TLabs; a lost or unbalanced registration leaves roots invisible during a snapshot |
| Allocation accounting | pending byte/object/count overflow, thread-local batch overflow | An overflow silently wraps a live-object count, which is what sweep uses to decide page retirement |
| Page integrity | live-object count underflow/overflow, page retired while live | A wrong live count retires a page that still holds reachable objects |
| Remembered set / barriering | remembered-set overflow, insertion failure, failed TLab publication | An old-to-young edge that is not remembered lets a minor cycle free a live young object |
| Pinning | pin count overflow, pin registration failure | Pin counts decide reachability for borrowed resurrections; an overflow releases a pinned object while native code still holds it |
| Transfer pins | pin count overflow, registration failure | A negative delta that wraps inverts pinned reachability |

Two clarifications about this class:

- **"Proven redundant" is a proof obligation, not an opinion.** A check may only
  be removed from either configuration if its condition is decidable at compile
  time or if it is implied by a check that already ran on the same value in the
  same critical section. No check in the table above qualifies: each reads
  caller-supplied or runtime-computed state that no earlier check constrains.
- **Compile-time verifiable invariants are already compile-time.** Descriptor
  shape, slot layout and pointer-root requirements are validated once per
  descriptor and remembered per thread; the remembered verdict is reused only for
  a descriptor whose words still match, which is a property of the memo, not a
  weakened check.

### 2.1 Audit of every invariant check

Every `nrq_runtime_fatal` site in the runtime was audited for two things: whether
it can be moved, and whether its condition is decidable without running.

| Source | Sites | What the checks read |
|---|---|---|
| `runtime/src/gc.cpp` | 51 | Root descriptors, slot bookkeeping, mutator protocol, page live counts, remembered set, pin and transfer-pin counts, TLab accounting |
| `runtime/src/allocator_windows.cpp` | 29 | Allocator ownership records, in-flight pins, bounds and ranges, LIFO discipline, shared-release ownership |
| `runtime/src/sync_windows.cpp` | 7 | Lock ownership and recursive acquisition state |
| `runtime/src/mapping_windows.cpp` | 4 | Reservation and commit bookkeeping |
| `runtime/src/fatal_windows.cpp` | 1 | The failure path itself |
| **Total** | **92** | |

Findings:

- **No check sits inside a configuration guard.** `gc.cpp` has exactly one
  `#if NRQ_DEBUG_ENABLED` region, the root-snapshot entry point, and it contains
  no fatal. The remaining guarded files (`gc_debug.cpp`, `slot_index.h`) hold
  telemetry only.
- **Both libraries are compiled from one list.** `NIRQOBA_RUNTIME_SOURCES` feeds
  `nirqoba_add_runtime_target` twice, so a check cannot exist in one build and
  not the other by construction. `scripts/check_runtime_check_parity.py` enforces
  both properties on every build: a fatal inside a configuration guard, or a
  per-target source addition, fails the build.
- **No check is compile-time decidable.** Each reads caller-supplied state (a
  descriptor, a slot, a pin delta), heap state computed during the run (a live
  count, an ownership record), or both. The invariants that *are* decidable are
  already compile-time assertions rather than runtime checks:
  `sizeof(ObjectHeader) == 96`, `alignof(ObjectHeader) ==
  alignof(std::max_align_t)`, `sizeof(size_t) == sizeof(uintptr_t)`, and the
  published `NrqMemoryProfile` size.
- **Nothing was removed.** Two checks were examined as removal candidates and
  kept, and the reasons are the reason the class exists:

  1. `pending_count == SIZE_MAX` in `account_tlab_allocation` cannot be reached:
     every allocation adds at least one byte to `unflushed_bytes`, and the batch
     flushes at `kAccountingBatchBytes`, so the count between resets is bounded
     by that constant. The argument holds only because of the flush rule, not
     because of anything the check itself reads, so removing it would trade a
     compare for a dependency on a rule that a future change to batching could
     quietly break.
  2. The descriptor checks on a remembered path are skipped only when the
     descriptor's words are identical to the ones already validated. Removing the
     check would mean trusting the memo unconditionally, which is a weaker
     property than validating the descriptor.

  A check that protects memory safety or heap integrity is not removed on the
  strength of an argument about the code around it. The bar is compile-time
  decidability, and none of the 92 meets it.

## 3. Accuracy-critical functionality (always enabled, both configurations)

These are numbers a program can read. They are behaviour: a program that branches
on `gc.get_statistics()` produces a different result if the value is wrong, so
"exact in production" is a contract, not a convenience.

| Mechanism | Surface | Contract |
|---|---|---|
| `nrq_gc_get_stats()` and `nrq_gc_get_allocated_bytes` / `_collected_bytes` / `_collection_count` / `_live_objects` | `gc.get_statistics()` | Exact at the moment of the call, including allocations still in the calling thread's unpublished TLab (`GcTest.UnpublishedTlabAllocationsRemainVisibleInStats`) |
| `nrq_gc_page_count`, `nrq_gc_recycled_page_count`, `nrq_gc_page_reuse_count`, `nrq_gc_span_reuse_count` | `gc.*` page queries | Exact; they are the only view of reclamation locality a program has |
| `nrq_gc_object_header_bytes`, `nrq_gc_object_index_entries`, `nrq_gc_pointer_is_managed` | layout/audit queries | Exact; used by tests and by programs that audit their own heap |
| `nrq_gc_pin_count`, `nrq_gc_transfer_pin_fallbacks`, `nrq_gc_pin_violations` | pin diagnostics | Exact; `pin_violations` is the program's only record that the per-object pin limit refused a request |
| `nrq_gc_policy_adaptation_count`, `nrq_gc_last_reclaimed_bytes`, `nrq_gc_is_adaptive`, `nrq_gc_get_threshold`, `nrq_gc_get_minor_collection_count` / `_major_` | collection policy queries | Exact; these report the budget the collector will actually use next, so a program tuning against them must see the same numbers the allocator does |
| `nrq_gc_get_ref_count` with `nrq_gc_ref_count_start` / `_stop` | `memory.get_ref_count` | A language feature, not a debug facility: the counts are returned to the program |
| `reference_diagnostics_enabled` / `diagnostics_reading_roots()` | internal | Required by the row above: it is the handshake that makes the counts safe to read, not a report about them |
| `nrq_memory_profiler_*` | `memory.profiler.*` | A language feature. It is gated by its own explicit start/stop flag, which is an early return on the allocation path, so it costs nothing while unused |

### 3.1 Why exactness costs two stores, and what would be cheaper but wrong

`gc.get_statistics()` is exact for the calling thread's own unpublished
allocations as well (`GcTest.UnpublishedTlabAllocationsRemainVisibleInStats`),
and for a query made by any other thread
(`GcTest.StatisticsStayExactWhileAnotherThreadAllocates`). The accounting that
provides this sits on the allocation fast path, in `account_tlab_allocation`:

- two relaxed loads and two relaxed stores, on the allocating mutator's own
  `pending_bytes` and `pending_count`, which live on that mutator's cache line;
- an owner-only add to `unflushed_bytes`, which no other thread reads;
- one relaxed load of the shared `collection_threshold`, needed only to size the
  batch that decides when the pending pair is flushed under the heap lock.

Four cheaper schemes were considered and rejected before keeping this one:

1. **One global counter under the heap lock.** Every allocation would take
   `gc_mutex`; the mutator split exists precisely to avoid that.
2. **One global atomic counter, updated per allocation.** Same cost per
   allocation in instructions, but every thread writes the same cache line, so
   every allocation invalidates the line for every other allocating thread.
3. **Owner-only counters, published when the batch closes.** Cheapest option:
   two owner-only adds and no shared load. It fails the contract above - a query
   from another thread would report a heap that omits the allocating thread's
   entire batch, and a query from the owning thread would have to special-case
   its own unpublished state everywhere.
4. **Packing `(bytes, count)` into one 64-bit word.** Saves one load/store pair,
   but is unsound without a bound the language does not impose: a single
   allocation may be arbitrarily large, so no fixed split of the word holds the
   byte total, and a fixed split that reserved room for the count would force a
   flush - and therefore a lock acquisition - inside the accounting step exactly
   when the byte field was about to overflow.

The count cannot be derived from the byte total either, because objects are not
fixed-size.

So the current mechanism is the cheapest one that keeps the contract, and both
stores are relaxed writes to a line the thread already owns. No timing comparison
was run: benchmark runs require the owner's authorization, and nothing here is
claimed on the strength of a measurement. If one is authorized, the honest
comparison is the two rejected variants above against this code, not a variant
against itself.

### 3.2 `diagnostics_reading_roots()`: an audit of its five call sites

The function answers one question: may some thread walk a shadow stack right now
outside the collector's own enumeration? Fast-path root pushes write the stack
without `stack.mutex`, which is safe against the collector (it parks mutators and
takes the mutex) but not against a reader that is running concurrently with a
mutator that is not being parked. The flag is how a reader announces itself, and
the five call sites divide cleanly into three writers and two readers.

Writers - mutator side, each taking `stack.mutex` only when a reader could be
present:

| Site | Operation | Why the mutex is conditional |
|---|---|---|
| `nrq_gc_root_register` fast path | push one root | A reader walking the vector could otherwise see it mid-growth. Already inside `begin_fast_barrier()`/`end_fast_barrier()`, so the collector cannot snapshot between the check and the push |
| `nrq_gc_root_register_frame` fast path | reserve and push a whole frame | Same hazard at batch granularity; the mutex covers the reserve and every push, so no reader observes a half-installed frame |
| `nrq_gc_root_frame_end` fast path | shrink the root vector | `resize` is not atomic; a reader could observe a truncated vector while it is being rewritten |

Readers - each sets or observes the flag, drains the lock-free regions that are
still in flight, and only then walks:

| Site | Role |
|---|---|
| `nrq_gc_get_ref_count` | Returns `-1` immediately when no reference counting is active. Otherwise it drains in-flight fast-barrier regions (`wait_for_fast_barriers()`, skipping its own thread), takes `gc_mutex`, publishes its own TLab, then counts references over every object and every root |
| `nrq_gc_debug_snapshot_roots` | Development configuration only. It *sets* the flag around its walk, then restores the previous value, so a snapshot neither permanently enables reference diagnostics nor interrupts one that a program started |

Classification: **accuracy-critical, not telemetry.**

- The value `nrq_gc_get_ref_count` returns is published to the program by
  `memory.get_ref_count`, so the read has to be correct in the same sense
  `gc.get_statistics()` has to be exact.
- The flag is turned on by `nrq_gc_ref_count_start`, which exists for the
  reference-count feature rather than for observation; the debug snapshot sets it
  only for the duration of its own walk and restores the previous value. A
  production program that asks for a reference count gets the same handshake a
  development program does.
- The debug root snapshot is a *user* of the mechanism, not its owner. It would
  be deleted in the production configuration regardless; deleting it does not
  remove a single call site, because the three mutator sites and
  `nrq_gc_get_ref_count` remain. That is the correct direction of dependency:
  instrumentation leans on correctness machinery, never the reverse.
- It does not touch parking, SBAT/SBOT, safepoint placement, or root-frame
  discipline. Its only effect on a collection is that mutators take
  `stack.mutex` on a push while a reader is active - a lock the collector's own
  enumeration takes anyway.
- Cost when no program asks for reference counts: one acquire load of a
  read-mostly word in each of the three mutator fast paths, on x64 a plain load.

## 4. Pure observability (debug-only, development library only)

These answer a developer's question about the run. No program can read them and
no correctness decision consults them; the production library does not contain
their code, their buffers or their symbols.

| Mechanism | Where | Absent from production because |
|---|---|---|
| Event stream: allocation, cycle begin/end, promote, sweep chunk, root frame | `runtime/src/gc_debug.cpp`, emitted from `gc.cpp` | It is a recording of decisions the collector already made; nothing reads it to decide anything |
| Allocation-site tags and the site table | `gc_debug.cpp` + compiler emission in `TirToMlir.cpp` | Attribution only; a tag cannot change what is allocated or freed |
| Root snapshot (`nrq_gc_debug_snapshot_roots`) | `gc.cpp`, `NRQ_DEBUG_ENABLED` | Reports state; never used to compute it |
| Slot-index probe length and rehash/growth/compaction counts | `runtime/src/slot_index.h` | Written but never read outside tests; the production build does not record them at all |
| Instrumentation hooks in emitted code (site declaration, allocation tag) | compiler, `--debug-enabled` only | The production lowering emits nothing, so there is nothing to strip |

## 5. Compiler-side diagnostics

The compiler has no configuration split: it is one binary that produces both
kinds of module. Its diagnostics (`--remarks`, `--verbose` pass statistics,
GC-root analysis counters, error messages) are therefore always present and are
outside this classification. The one compiler mechanism that belongs to the split
is instrumentation emission, and it is listed in section 4 because the emitted
calls exist only in a development build.

## 6. Enforcement

Classification is only real if a mechanism cannot drift across the boundary:

- **Symbol absence.** A telemetry entry point exists only in the development
  library. A production program that calls one fails to link, which is a loud
  failure at the right time rather than a silent no-op.
- **Configuration identity.** The link is the proof that the module and the
  library were built for the same class of program (`06-runtime.md` §4.3).
- **Tests.** `tests/unit/gc_debug_test.cpp` covers the development-only surface;
  `tests/run_tests.py` covers the production path end to end; the invariant
  checks and the statistics contract are covered in both, because they are
  present in both.

## 7. Adding a mechanism

1. Ask the section 1 questions in order and write the answer down with the change.
2. If it is essential, it goes where every configuration sees it, and it fails
   fatally rather than returning a status.
3. If it is accuracy-critical, it must be exact on every call, in both
   configurations, including state that has not been published yet.
4. If it is telemetry, it goes in the development library behind
   `NRQ_DEBUG_ENABLED`, it may drop records but must count the loss, and it must
   not take a lock an allocator also takes.
5. Add a test for the class you chose. A telemetry test belongs in the
   development-runtime suite; an essential or accuracy test belongs in both.
