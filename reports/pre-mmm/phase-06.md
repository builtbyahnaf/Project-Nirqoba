# Pre-MMM Phase 6 — Compiler lowering and optimization integration

**Version:** `0.473-beta`  
**Date:** `2026-11-14`  
**Baseline:** `569a8ab` (`0.469-beta`)  
**Scope:** aggregate shared-pointer lowering; ownership/GC-preserving root and
write-barrier elision; optimization-level comparisons.

## 1. Verdict

Phase 6 is complete for the supported compiler and optimization surface. Two
implementation upgrades were made:

1. Shared-pointer aggregate fields now resolve their pointee layout instead of
   lowering through an opaque identified struct. This fixes the Phase 5 compiler
   crash and the associated out-of-bounds GEP.
2. Stack-placed owned storage that cannot be observed by a collection no longer
   publishes a GC root or executes a write barrier. The analysis retains both
   operations whenever a call may collect or a loop backedge can reach the
   runtime safepoint.

Ownership attributes, cleanup ordering, exactly-once destruction, heap roots,
shared roots, and observable stack roots remain intact through `-O0`, `-O2`, and
the full suite.

## 2. Upgrade 1 — shared pointer aggregate resolution

### Root cause

`resolve_aggregate_type` expanded arrays, tuples, owned arrays, owned pointers,
array views, and containers, but deliberately left pointer elements shallow. The
shared-pointer case was missing from the expansion list even though the language
allows `(*holder.node).field` and shared fields are released by aggregate
destruction.

The TIR carried `shared ptr Node` with a fieldless `Node` placeholder. GEP
lowering therefore produced an identified `!llvm.struct<"Node", opaque>` and
either crashed in `extractvalue` or emitted an out-of-range index. The same
source through an owned pointer compiled because owned pointees were already
resolved.

### Fix

`TypeChecker::resolve_aggregate_type` now rebuilds a shared pointer around the
resolved pointee. The existing `active` name set provides the recursion guard for
self-referential shared structures. No ownership rule changed: the shared value
is still reference-counted explicit storage, and its managed fields still use the
Phase 5 rooted shared-allocation path.

Regression: `tests/integration/positive/shared_field_gc_roots` and its `-O0`
variant. Both assert `1`, `73`, `0`: the child is traced before release and
reclaimed after the shared field is released.

## 3. Upgrade 2 — unobservable owned-storage root/barrier elision

### Observation

A stack-placed `owned ptr T` with a managed field was always registered in its
activation frame, even when the function had no collection point. The write into
that storage also used `nrq_gc_root_store`. Both are unnecessary when no
collection can snapshot the activation: the storage is non-escaping and its
lifetime ends before any caller safepoint.

### Implementation

The lowering computes a conservative `has_collection_point` for each function:

- any call that may collect, using the existing transitive `CollectionSummary`;
- any loop backedge, where the runtime safepoint can publish this activation.

Stack-placed raw owned storage records its allocation result and every derived
GEP address. When `has_collection_point` is false:

- no typed root registration is emitted;
- a managed store through one of those addresses is a plain store, not a write
  barrier.

Heap-owned storage, observable stack storage, shared storage, and managed
allocations are unchanged. A call to `gc.collect`, an allocator, an unknown
runtime entry, or a user function that may collect keeps the root and barrier.

### Why the large runtime effect

The baseline's root/barrier calls are side effects around otherwise private stack
blocks. Removing them lets LLVM promote the blocks and their fields normally.
The benchmark therefore measures both the direct calls and the optimization they
blocked, not just a call-count microbenchmark.

## 4. Benchmark

`benchmarks/pre_mmm_phase6/root_elision/main.nrqba`: 32 `Node` blocks per call,
100,000 calls, managed child per block. Windows 11 x64, MSVC 14.51,
`RelWithDebInfo`.

| Measurement | 0.469-beta | 0.473-beta | Change |
|---|---:|---:|---:|
| Runtime median (7 process runs) | 104.13 ms | 16.63 ms | **-84.0%** |
| Compile median (`--emit=llvm`, 5 runs) | 56.9 ms | 48.8 ms | **-14.2%** |
| Compiler peak RSS median (3 runs) | 29.8 MB | 28.7 MB | **-3.7%** |
| Executable size | 94,208 B | 90,112 B | **-4.3%** |

Runtime includes process startup. The workload is synthetic and deliberately
isolates this optimization; the result is not a general application speedup.
Raw samples: `benchmarks/pre_mmm_phase6/results.json`.

## 5. Verification

| Check | Result |
|---|---|
| Clean build (RelWithDebInfo) | PASS |
| Compiler version | `0.473-beta` |
| Unit suite | 228/228 |
| Integration suite | 460/460 |
| Shared field root, `-O2` | `1`, `73`, `0` |
| Shared field root, `-O0` | `1`, `73`, `0` |
| Unobservable root elision, `-O2` | PASS, 73 |
| Unobservable root elision, `-O0` | PASS, 73 |
| Elision IR, `-O2` | no holder root or barrier; one caller root |
| Observable stack root, existing probe | `1`, `73` |
| Optimized vs unoptimized semantic comparison | PASS for the covered probes |

The integration suite now includes `shared_field_gc_roots`,
`shared_field_gc_roots_o0`, `owned_pointer_root_elision`,
`owned_pointer_root_elision_o0`, and `owned_pointer_root_elision_ir`. Existing
`stack_owned_pointer_gc_roots`, `owned_pointer_gc_roots`,
`owned_pointer_gc_reallocation`, `owned_pointer_cleanup_gc_roots`, and
`shared_pointer_gc_roots` continue to pass.

No ASan/UBSan run is available in the MSVC configuration. The known
`GcTest.MinorReclaimsOnlyYoungGarbageAndKeepsOldGarbage` stress flake recorded in
Phase 4 remains separate.

## 6. Preserved semantics and conservative restrictions

- No new cleanup elision was introduced; existing move/cleanup tests still pass.
- No runtime ownership check was removed. The change only removes GC bookkeeping
  that no collection point can observe.
- Calls with unknown effects are collection points. This is conservative and
  keeps optimization from outrunning the summary.
- The elision applies to raw stack-owned storage, not owned arrays, shared
  storage, heap-owned storage, or managed objects.
- The shared-pointer resolution follows the existing active-name recursion guard;
  it does not expand through pointers at layout time beyond the declared pointee.
- Nested shared fields are now compiled and tested. Other aggregate pointer shapes
  remain covered only where the type checker can resolve them.

## 7. Commits

- `sema: resolve shared pointer aggregate fields`
- `test: cover shared field roots at O0 and O2`
- `gc: elide unobservable owned-storage roots and barriers`
- `test: verify owned-storage root elision at O0 and O2`
- benchmark, report, roadmap, version, and progress commits follow.

## 8. Assessment

**Implementation completeness:** Complete for the two Phase 6 upgrades and the
known crash. The boundary is conservative where the collection summary is not
precise.
**Verification confidence:** High for the new shared-field path and the
root/barrier elision, including `-O0`/`-O2` comparisons. The large benchmark gain
is explained by exposing SROA, not by weakening a check.
**Remaining work:** broader effect modelling for deterministic release runtime
calls and a full sanitizer run are follow-ups, not Phase 6 requirements.
