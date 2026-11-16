# Phase 9 audit — optimization, compatibility, and stabilization

**Branch:** `main`  
**Starting version:** `0.583-beta`  
**Starting commit:** `9280b37` (clean tree)  
**Audit date:** 2026-11-16  
**Scope:** existing memory model only; no Phase 10 CDMM work

---

## 1. Preflight

| Check | Result |
|---|---|
| Branch | `main` |
| HEAD | `9280b37` |
| Version | `0.583-beta` |
| Phase label | 8 |
| Working tree | clean |
| Unit tests | 329/329 |
| Full runner | 624/624 |
| Phase 7 isolation | passed |
| Phase 8 manual tests | passed |

The starting point matched the Phase 9 specification. No pre-existing user
changes were discarded.

---

## 2. What exists today

### 2.1 Ordinary owned allocation

- Source: `memory.allocate<T>(bytes[, allocator])`, `memory.owned_array<T>(n)`.
- Sema gives the result `Owned`; copies are rejected, moves transfer, and a
  lexical destruction obligation is created.
- TIR: `tir.call @memory.allocate` with `own=owned`; destruction is a `Drop`
  whose release is `memory.deallocate` or `array.owned_release`.
- Lowering: allocation calls `nrq_memory_allocate` or
  `nrq_memory_allocate_from`; release calls `nrq_memory_deallocate_checked`.
  Fixed-size, non-escaping allocations can be promoted to entry-block stack
  storage by the existing escape analysis.
- Runtime: process-default general allocator uses the Windows process heap plus
  a 64-entry, 1 KiB thread-local cache; explicit handles go through the global
  allocator map and operation pin. A sharded ownership registry records
  allocator, bytes and alignment for every live block.

### 2.2 Borrowing

- Borrows are Sema/TIR-level relationships; there is no runtime borrow object,
  allocation, copy, reference count, or GC dependency.
- TIR `Borrow`/`EndBorrow` markers are consumed during lowering. Borrowed
  access becomes direct LLVM pointer access.
- LLVM function arguments receive `noalias`/`nocapture` and, when the body never
  stores through a shared borrow, `readonly`.
- Escape checks reject returning or storing a borrow whose source does not
  outlive it; mutable exclusivity is preserved.

### 2.3 Arena and pool

- `memory.arena` / `memory.pool` create allocator handles.
- Arena allocation is a bump pointer over heap chunks; reset retains only the
  initial capacity-bounded chunk and frees growth chunks.
- Pool uses fixed size-class buckets and free-slot vectors; each slot is
  recorded by pointer.
- Runtime allocation takes the global allocator map, pins the allocator, then
  serializes the strategy operation. Arena allocation is not inserted into the
  ownership registry; reset/destroy reject live registered records and in-flight
  operations.
- Managed payloads are rejected for tracked arena/pool storage.

### 2.4 Shared ownership

- `memory.shared_allocate` creates one control block in a 64-way pointer-sharded
  map. Retain/release take the shard mutex and use atomic `acq_rel` count
  transitions. The control is removed before final deallocation, preventing
  resurrection.
- Final release removes GC root metadata, marks the registry record in flight,
  frees through the original allocator, then erases the record.
- No weak references or cycle collector exists.

### 2.5 GC-managed storage

- `array.allocate<T>` and `managed<T>` select collector storage.
- TIR carries `gc_managed` ownership; lowering emits typed root registration,
  frame root tables, root stores (write barrier), safepoint polls, and mutator
  enter/leave only in functions whose transitive effects require them.
- The runtime has fast barriers with a reader handshake, thread-local shadow
  stacks, an O(1) slot index, a direct-mapped pointer-root cache, generation-
  checked thread-local allocation pages, and a production/debug split for
  descriptor validation.
- Phase 7 isolation remains explicit: ordinary, borrowed, arena, pool,
  plain-shared, and plain manual paths have no `nrq_gc` dependency.

### 2.6 Manual storage

- `memory.manual_allocate<T>` produces `Manual`; `memory.manual_release` is the
  only valid release.
- Allocator identity, alignment, moves, borrow exclusion, exactly-once release,
  lexical fallback, and managed-payload rooting are tracked.
- Manual storage remains function-local because no manual pointer type carries
  provenance through a signature.
- Reallocation, tracked arena/pool handles, deterministic-cleanup payloads, and
  aggregate fields are rejected.

### 2.7 Native resources

File, socket, mutex, mapping, and secure buffers are deterministic resource
domains with dedicated close/destroy entry points. They are not heap ownership
categories and were not changed in Phase 9.

---

## 3. Production and debug differences

| Area | Production | Debug |
|---|---|---|
| Ownership registry | always present | always present |
| GC tags/events | omitted | emitted |
| Root-frame validation | static descriptor trusted | descriptor walked |
| Safepoint polling | present where required | present + diagnostics |
| Allocator checks | invalid/double release, pin, LIFO | same |
| Manual checks | same Sema/TIR/runtime contract | same |

The debug runtime is expected to be slower. Phase 9 did not move debug-only
work into production and did not remove runtime safety checks.

---

## 4. Baseline measurements

Harness: `benchmarks/memory-model/phase-9/`, MSVC `/O2`, nine fresh-process
samples, `steady_clock` around each measured loop, checksum equality enforced.
Baseline file: `results-baseline.json`.

| Mode | Median ns/op | Peak RSS |
|---|---:|---:|
| owned allocate/release | 58.499 | 9,494,528 |
| owned retained allocation | 215.929 | 45,158,400 |
| owned release (retained set) | 108.320 | 45,158,400 |
| manual path allocation | 61.021 | 45,158,400 |
| borrowed read | 0.073 | 45,158,400 |
| arena allocation | 48.856 | 17,043,456 |
| arena reset | 194.400 | 17,121,280 |
| pool allocate/release | 152.806 | 9,154,560 |
| shared allocate + 8 retain/release | 644.529 | 8,986,624 |
| 4-thread shared retain/release | 76.474 | 9,334,784 |
| GC allocation | 56.254 | 50,921,472 |
| GC full collection | 6,393,800 | 50,982,912 |
| mixed five-strategy loop | 109.906 | 10,809,344 |

The manual C ABI path is the same checked allocation path, so its number
measures runtime overhead, not a separate heap. These numbers are not compared
to C++, Rust or Java.

---

## 5. Evidence-backed findings

### F1 — Default-aligned allocations paid for an alignment prefix they did not need

`heap_alloc_aligned` always allocated `bytes + alignment - 1 + sizeof(void*)`,
stored the raw pointer immediately before the result, and `heap_free_aligned`
always read that prefix. On x64, `HeapAlloc` already returns
`max_align_t`-aligned storage, so every ordinary `alignment <= 16` request paid
16+ bytes of slack and a pointer write/read for no alignment benefit.

Classification: **performance optimization and metadata simplification**.

Action: allocate directly for `alignment <= alignof(std::max_align_t)`, free
directly using the alignment already recorded by the ownership registry, and
retain the prefix path only for over-aligned requests. This does not remove the
ownership record: allocator identity, bytes, alignment and invalid-release
detection remain.

### F2 — Cached releases signaled a condition variable with no possible waiter

Allocation waits on `shard.changed` only when it finds an existing record with
`operation_in_flight`. The cache release path only handles records that were
never marked in flight and erases them. No thread can be waiting for that erase,
so `notify_all()` was unconditional overhead on every cached release.

Classification: **performance optimization**.

Action: remove the wakeup from the cached branch only. Slow-path release still
notifies because it clears the in-flight flag.

### F3 — Cleanup nulled an owner slot immediately before returning

TIR cleanup emits `constant_null` + `store` after discharging an obligation so
an outer scope cannot reload the owner. When that sequence is immediately
followed by `tir.return`, no later operation can read the slot. The clear is
dead code.

Classification: **metadata/code-generation simplification**.

Action: remove the dead clear pair in the lowering copy only, after verified TIR
and after `Drop` expansion. Obligations stay discharged; releases are untouched.

### F4 — Borrow lowering already has no runtime cost

Borrow markers disappear; access is direct. LLVM already receives
`noalias`/`nocapture`/`readonly` where the body proves them. There is no runtime
borrow table to remove and no hidden allocation/copy/refcount. No change was
justified.

Classification: **no change**.

### F5 — Arena allocation is already a bump pointer, but each call pays two locks

`nrq_arena_allocate` takes the global allocator map and operation pin, then the
allocator's serialization mutex. The bump itself is a few arithmetic operations.
The locks protect handle lifetime and concurrent chunk growth; removing either
would require a lifetime protocol (for example a generation-checked handle
cache) that is larger than this phase. The measured 48.9 ns/op is dominated by
that protocol, not by bump arithmetic.

Classification: **deferred work**. No speculative lock removal.

### F6 — Pool release searches bucket maps

Pool allocation/release measured 152.8 ns/op. The pool has one bucket in the
common single-size workload, so the bucket scan is short; release still performs
two hash lookups (bucket slot map and ownership registry). Replacing them with a
single index would add metadata and was not justified by the current
measurements.

Classification: **deferred work**.

### F7 — Shared retain/release is lock-protected, not atomic-only

The shard mutex protects control-block lifetime and prevents resurrection.
Every retain/release currently pays it. A lock-free fast path would need a
stable control address or an epoch/hazard protocol; neither exists. Atomic
ordering and overflow checks are already correct, and the 4-thread contention
result (76.5 ns/op) does not show a pathological collapse.

Classification: **deferred work**. No architecture replacement.

### F8 — GC isolation is already selective

Phase 7 tests and Phase 9 baseline confirm ordinary paths allocate no GC
metadata and touch no collector entry points. Root frames are batched, static
descriptors skip validation in production, root stores have a fast path, and
thread-local caches already avoid the global page lock. Removing further
instrumentation would weaken liveness or barrier correctness.

Classification: **no change**.

### F9 — Manual storage has no separate runtime path to optimize

Manual allocation uses the same allocator/registry entry points as ordinary
storage. F1 therefore benefits it too. Stack-promoting manual storage would
change allocator identity and explicit release semantics, so it is out of scope.

Classification: **deferred work**.

---

## 6. Compatibility findings

| Area | Finding |
|---|---|
| Ordinary programs | No source change required. No new GC dependency. |
| Borrow syntax | Unchanged; direct access preserved. |
| Arena/pool | Unchanged lifetime and release restrictions. |
| Shared | Unchanged retain/release and final-release behavior. |
| GC workloads | Unchanged roots/barriers/safepoints; examples still check. |
| Manual | Function-local restriction remains documented. |
| FFI | Still unsupported; no `extern`, foreign pointer, callback or ABI exists. |
| Aggregates | Manual fields still rejected by the existing field rule. |
| Reallocation | Manual reallocation still rejected. |

All five `examples/*.nrqba` entry points check successfully. The example set
contains ordinary, GC-managed and multi-file programs; no outdated memory
syntax was found.

---

## 7. Removal decisions

Kept deliberately:

- ownership shard records (allocator identity, bytes, alignment, in-flight
  state, shared flag);
- allocator operation pins and per-allocator mutexes for stateful strategies;
- GC root/barrier/safepoint metadata;
- debug configuration checks;
- manual provenance and release-family checks.

Removed:

- unnecessary alignment prefix for naturally aligned default heap blocks;
- one condition-variable wakeup per cached release;
- one dead null/store pair per returned owned slot.

Not removed:

- any release, obligation, root, barrier, or atomic operation;
- any runtime invalid/double-release check;
- any allocator identity record.

---

## 8. Verification plan

1. Unit tests for natural/over-aligned allocation and cache reuse.
2. TIR test for dead clear removal; verifier and full ownership suite.
3. Phase 9 O0/O2/production/debug fixture matrix with generated LLVM checks.
4. Full runner, Phase 7 isolation, Phase 8 manual suite.
5. Configuration equivalence for GC-filtered integration tests.
6. Re-run the Phase 9 harness with the same nine-sample method.
7. Report the release-cost regression and net retained-set cost honestly.

The benchmark is evidence for specific paths, not proof of correctness.
Correctness evidence comes from the test and generated-code checks.
