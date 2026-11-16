# Phase 6 audit — explicit shared ownership

**Starting version:** `0.563-beta`  
**Starting commit:** `2306fdf` (`docs: add the Phase 5 completion report`)  
**Branch:** `main`  
**Working tree:** clean except the pre-existing uncommitted Phase 10 addition in
`project_context/MMM_PLAN.md` (555 inserted lines), which this phase preserves.

## 1. Existing representation

There is no separate `SharedOwned` class. Shared ownership is represented by:

- `ValueOwnership::Shared` in `compiler/include/nirqoba/Common/Type.h`;
- `TypeKind::SharedPointer` for `shared ptr T` fields;
- the source spellings `shared T*`, `memory.shared_allocate<T>`,
  `memory.shared_retain`, `memory.shared_release` and `memory.shared_count`.

`is_owning_ownership` and `is_transferable_ownership` classify `Shared` with
`Owned`; `is_borrowing_ownership` excludes it. `Weak` exists only as a reserved
ownership category and is never produced by the parser, Sema or builder.

### 1.1 Allocation and release

`nrq_memory_shared_allocate` calls the ordinary allocator, inserts the pointer in
a process-wide `shared_reference_counts` map with count 1, and marks the
allocation record `shared = true`. The map is protected by the global
`allocator_mutex`.

- `nrq_memory_shared_retain` looks up the map and increments the count.
- `nrq_memory_shared_release` decrements while the count is greater than one. At
  one, it reserves the final release by storing zero and marking the allocation
  record in flight, removes a collector root if present, deallocates the payload,
  and erases the record.
- `nrq_memory_shared_count` reads the map or returns zero.
- `nrq_memory_shared_allocate_rooted` additionally registers a typed root for
  managed fields and records the root metadata in `shared_roots`.

The pointer itself is the payload address; there is no control block in the
payload and no per-object lock. Retain, release and count all serialize on the
global allocator mutex.

### 1.2 Sema and TIR

Sema propagates `shared_allocation` through declarations, parameters, returns and
aggregate fields. It selects `memory.shared_release` as the cleanup callee and:

- rejects shared values passed to owning parameters;
- rejects non-shared values passed to shared parameters;
- rejects shared values through ordinary deallocation;
- rejects shared locals returned from non-shared functions and vice versa;
- rejects shared fields in explicit storage whose release cannot walk fields;
- rejects shared allocations whose payload requires deterministic cleanup.

TIR records `ValueOwnership::Shared` on allocation, retain, aggregate retain and
release values. The verifier enforces the shared allocation contract, shared
parameter matching, shared-release ownership, rejection of shared values through
`memory.deallocate`, and the aggregate retain/release family. Moves preserve the
Shared category; a move does not emit a retain.

Aggregate copy emits `aggregate.shared_retain`; aggregate destruction emits
`aggregate.shared_release` in reverse field order. Partial initialization is
zeroed and skips null shared fields.

### 1.3 Existing tests

The repository already covers:

- `tests/integration/positive/shared_memory`: retain, count, parameter transfer;
- shared fields: construction, copy retain, move without retain, nested fields,
  partial initialization, GC roots;
- shared GC isolation at `-O0` and `-O2`;
- negative cases for unique-parameter transfer, wrong release, owned payloads,
  shared fields in explicit/bulk storage, and return-domain mismatches;
- `tests/unit/allocator_test.cpp`: concurrent retain/release, final release,
  rooted metadata inline/overflow, and final release not holding the global
  registry lock while a deallocation callback blocks;
- `tests/unit/ownership_test.cpp`: shared move representation, allocation
  strategy contract, reserved-return rejection.

The existing suite therefore proves the strong-count semantics for one process
thread and a shared C++ stress case, but not an independent-object contention
benchmark, overflow exhaustion, or any weak-reference contract.

## 2. Gap analysis

| Area | Current state | Phase 6 risk |
|---|---|---|
| Strong counting | Correct for supported single-process paths | Global mutex serializes every retain/release and unrelated allocator traffic |
| Count storage | `uint64_t` under global mutex | No atomic ordering to document; overflow returns null but is not surfaced |
| Control block | None; map entry only | No defined layout/lifetime after the map; weak refs impossible |
| Thread safety | Mutex-protected | Correct but a global bottleneck; no contention measurement |
| Weak ownership | Reserved enum only | No creation, promotion, expiration or control-block lifetime |
| Cycles | No weak refs; shared payloads reject shared fields | A supported shared payload cannot hold another shared owner, so a shared-to-shared cycle is not constructible through that path |
| Final destruction | One release at count one | Correct in existing tests; needs concurrent-final-owner coverage |
| Compiler verification | Shared contracts exist | No malformed-TIR cases for shared retain/release beyond allocation strategy |
| GC isolation | Verified for plain shared allocations | Must remain true after any runtime change |
| Arena/pool | Explicit storage rejects shared fields; shared allocation rejects cleanup payloads | No silent promotion; must preserve |

## 3. Design decision

### 3.1 Strong ownership

Keep the existing pointer-as-token ABI and the existing allocation registry.
Replace the single `shared_reference_counts` map with a sharded control registry:

- each shared pointer has a `SharedControl` with an atomic strong count;
- shards are selected by pointer hash, so unrelated objects do not share a lock;
- retain/release/count take one shard lock, not `allocator_mutex`;
- the final 1→0 transition removes the control entry before deallocation, so a
  concurrent retain cannot resurrect the object;
- the allocation record remains the authority for allocator identity and
  in-flight protection;
- root metadata remains keyed by pointer and is removed before root
  unregistration.

The atomic count uses acquire/release ordering on the final transition and
relaxed reads inside the shard lock. The shard lock supplies the map/control
lifetime; the atomic count supplies a defined overflow/CAS contract and makes
the published count explicit. No global lock is taken by an ordinary retain or
a non-final release.

This is an internal runtime change. It does not add syntax, change the pointer
representation, or make ordinary owned values reference-counted.

### 3.2 Weak references and cycles

Weak references are **not** implemented in Phase 6. The original target
architecture already says weak references require a concrete standard-library
requirement and a testable expiration/destruction contract. There is no `weak`
source type, parser production, library container, or existing tests that need
one. Adding a runtime-only weak API without a language surface would not provide
a usable ownership-cycle mechanism.

Cycle policy:

- A `memory.shared_allocate<T>` payload that contains a shared field is rejected
  because the final release frees raw storage without walking field destructors.
- Therefore a supported shared allocation cannot contain an owning edge back to
  another shared allocation. The only other shared owners are lexical
  `shared ptr` fields and bindings; those are not stored inside the shared
  payload.
- Cyclic graphs remain the domain of the existing explicit collector
  (`array.allocate`, SoA, managed fields). No cycle collector is added and no GC
  behavior is changed.

This is a scoped decision, not a claim that reference counting collects cycles.
If a future container permits shared fields inside shared payloads, that change
must first provide field destruction and then weak references or a documented
cycle strategy.

### 3.3 Phase 5 limitation review

| Limitation | Decision | Reason |
|---|---|---|
| Arena bulk release does not run destructors | Deferred; keep rejection | A correct destructor path needs typed cleanup metadata and a reverse-order log. None exists, and adding a per-object registry is not justified without measurement. The compiler already rejects payloads whose cleanup cannot be performed. |
| Arena reset releases backing chunks | Investigate and implement bounded reuse | Retaining only the initial chunk preserves bounded capacity, lets reset reuse storage, and still releases growth chunks. Final destruction must free the retained chunk. |
| Arena identity in MLIR/LLVM | Deferred; no per-object tags | The runtime handle and allocation call preserve the required contract. Identity is needed for Sema/TIR validation, not for machine-code execution. Per-object tags would add overhead without a correctness requirement. |
| FFI pointer laundering | Fundamentally unprovable; no FFI surface | No FFI or unsafe scope exists. The supported contract is that foreign pointer transfer is outside the language; this phase makes no FFI safety claim. |
| Weak references | Deferred as above | No source surface or concrete requirement; GC remains the cycle mechanism. |

## 4. Implementation scope

1. Sharded atomic shared-control registry and final-release path.
2. Overflow-safe retain and explicit runtime failure result.
3. Bounded arena reset capacity reuse.
4. Tests for multiple owners, copy/move, final release, concurrency, malformed
   TIR, GC isolation and arena reset retention.
5. Benchmarks for shared versus ordinary allocation, retain/release, move versus
   copy, uncontended and contended operations, and reset retention.
6. Documentation, progress records, migration note and version log.

## 5. Non-goals

- No Phase 7 GC strategy work.
- No new weak syntax or cycle collector.
- No universal reference counting.
- No automatic shared promotion.
- No FFI contract or unsafe scope.
- No rewrite of completed Phase 1–5 ownership rules.
- No change to collector semantics or the production/debug runtime split.
