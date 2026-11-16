# Migration Note — Phase 4: Ordinary Non-GC Allocation

**Version:** `0.551-beta`
**Scope:** MMM Phase 4. No new syntax, no second allocator, no GC removal.

Phase 3 completed borrow and lifetime enforcement. Phase 4 separates ordinary
owned allocation from the collector and makes that separation a checked compiler
contract.

## What now holds

- `memory.allocate`, `memory.reallocate`, `memory.owned_array`,
  `memory.owned_array_aligned` and `memory.secure_allocate` use the ordinary,
  explicit or secure allocator. Their release paths are the ordinary destructors.
- Ordinary owned storage has no GC object header, root frame, mutator region,
  safepoint, write barrier or collection bookkeeping.
- Bounded non-escaping arrays and raw allocations may still be promoted to the
  frame. Address-taken, escaping or returned storage stays on the heap.
- Deterministic destruction is unchanged: `array.owned_release`,
  `aggregate.owned_drop`, `memory.deallocate`, secure release and resource
  destructors discharge exactly one obligation.
- A payload containing `managed<T>` remains the deliberate mixed case. The
  storage is ordinary, but the managed references are registered as typed roots
  and removed at final release.
- `array.allocate<T>` and `memory.owned_soa<T>` remain collector-backed.
- The TIR verifier now checks allocation strategy: ordinary allocation must be
  `owned` with plain pointer/owned-array storage, collector allocation must be
  `gc_managed`, and shared allocation must be `shared`. Ordinary releases reject
  collector-managed values; `tir.drop` rejects a collector value entirely.

## What changed for existing programs

No valid program changes meaning. The observable difference is architectural:
owned-only IR contains no `nrq_gc` reference at `-O0` or `-O2`, and a malformed
TIR allocation contract is rejected with `NRQ4006`. The previous Phase 3 migration
note said ordinary allocation remained GC-backed; that statement was too broad.
The ordinary allocator was already non-GC, and the remaining unconditional owned
release unregister was removed during the pre-MMM Phase 9 hardening. Phase 4
audits, tests and verifies the complete separation rather than introducing a new
allocator.

## What did not change

- The collector remains fully operational for explicit managed arrays, SoA
  containers, managed fields and cyclic graphs.
- Secure storage still rejects managed payloads because it has no rooted-release
  contract.
- Shared allocation still uses a control block and runtime refcounts only for
  plain payloads; managed payloads use the rooted shared path.
- Strings from `file.read_text` remain borrowed process-lifetime storage.
- No arena lifetime semantics, weak references, FFI or exception unwinding were
  added. Phase 5 and later were not started.

## Verification

`313` unit tests and `554` full-run cases pass. The IR fixtures cover owned heap
pointers, returned owned arrays, borrowed heap views, ordinary pointer growth,
secure allocation and shared allocation at `-O0` and `-O2`; every one asserts
that the entire `nrq_gc` namespace is absent. Existing owned aggregate, owned
array, stack array and view fixtures now make the same whole-namespace assertion.
