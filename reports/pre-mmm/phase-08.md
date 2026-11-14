# Pre-MMM Phase 8 — Migration readiness and default-policy transition

**Baseline:** `0.499-beta` (`c0ddcfd`)  
**Final label:** `0.511-beta`  
**Status:** Complete.

## 1. Result

The supported language surface now follows one hybrid-memory rule:

- Deterministic ownership and borrowing are the default for every completed type and
  operation.
- A storage boundary that cannot run a value's release path rejects that value at
  compile time.
- No rejected ownership pattern falls back to the collector or to another mechanism.
- The collector remains available for managed/cyclic graphs through explicit
  allocation and explicit GC controls.

The supporting audit is in [`phase-08-audit.md`](phase-08-audit.md); the user-facing
guide is [`docs/migration/phase-8.md`](../../docs/migration/phase-8.md).

## 2. Policy transitions

| # | Implementation upgrade | Transition | Milestone |
|---|---|---|---|
| 1 | Collector arrays | `array.allocate<T>` rejects owned/shared/borrowed fields, owned arrays, SoA and views; managed/trivial payloads remain | `0.501-beta` |
| 2 | Explicit storage | `memory.allocate<T>`, `memory.reallocate<T>` and `memory.secure_allocate<T>` reject deterministic owners | `0.501-beta` |
| 3 | Shared allocations | `memory.shared_allocate<T>` rejects deterministic owners; managed roots remain | `0.505-beta` |
| 4 | Owned arrays | `memory.owned_array<T>` / `_aligned` reject shared and borrowed element fields | `0.505-beta` |
| 5 | SoA | `memory.owned_soa<T>` rejects shared and borrowed columns; managed columns remain traced | `0.508-beta` |
| 6 | Nested generics | `TypeChecker`/parser accept `owned_array<managed<T>>` without a space before `>` | `0.508-beta` |
| 7 | Legacy memory names | Untyped `nrq_memory_allocate`/`nrq_memory_deallocate` removed from source | `0.511-beta` |
| 8 | Legacy GC names | `nrq_gc_init`/`nrq_gc_shutdown`/`nrq_gc_collect` removed from source | `0.511-beta` |

Every rule is implemented in `TypeChecker::requires_deterministic_cleanup` and its
allocation/container callers. The helper walks struct fields recursively, so a nested
owned pointer, owned array, SoA, shared token, borrowed field, array view or managed
payload is classified by its actual contents rather than by its outer spelling.

## 3. Rejected unsupported patterns

| Pattern | Diagnostic | Evidence |
|---|---|---|
| Collector array with shared field | `collector storage has no finalizer for an owned, shared, or borrowed field` | `negative/gc_array_shared_field` |
| Collector array with borrowed field | same | `negative/gc_array_borrowed_field` |
| Explicit allocation with shared field | `deallocation frees the storage without running field destruction` | `negative/explicit_allocation_shared_field` |
| Secure allocation with owned field | `secure release frees the storage without running field destruction` | `negative/secure_allocation_owned_field` |
| Reallocation with shared field | `resizing frees the old storage without running field destruction` | `negative/reallocate_shared_field` |
| Shared allocation with owned field | `the final release frees storage without running field destruction` | `negative/shared_allocation_owned_field` |
| Owned array with shared field | `bulk release does not run reference-counted or borrowed field destruction yet` | `negative/owned_array_shared_field` |
| Owned array with borrowed field | same | `negative/owned_array_borrowed_field` |
| SoA with shared column | `columns cannot hold reference-counted or borrowed fields yet` | `negative/soa_shared_column` |
| SoA with borrowed column | same | `negative/soa_borrowed_column` |
| Legacy GC source name | `call to undeclared function 'nrq_gc_collect'` | `negative/legacy_gc_builtin` |
| Legacy memory source name | `call to undeclared function 'nrq_memory_allocate'` | `negative/legacy_memory_builtin` |

Positive coverage: `positive/gc_array_managed_payload`,
`positive/nested_generic_managed_payload` and
`positive/secure_allocation_managed_payload`. Existing owned-aggregate, shared-field,
borrow, SoA and GC-root suites pass unchanged.

## 4. GC opt-in behavior

- Automatic collection starts disabled at process start.
- `array.allocate<T>` is the explicit collector allocation; it does not enable
  automatic collection.
- `gc.enable()` / `gc.disable()` control automatic triggers; `gc.collect()` runs a
  full cycle regardless of that setting.
- Managed references in owned pointers, owned arrays, SoA, shared allocations, secure
  allocations and struct/tuple aggregates remain typed roots while live.
- Cycles remain supported only through managed references. A cyclic graph cannot be
  expressed with unique owners because it has no static release order.

The runnable example `examples/memory/cyclic_graph.nrqba` produces `22` then `11`;
`examples/memory/hybrid_memory.nrqba` produces `5`, `2`, `7`, `3`.

## 5. Legacy paths

**Removed from the source language:**

- `nrq_memory_allocate`, `nrq_memory_deallocate`
- `nrq_gc_init`, `nrq_gc_shutdown`, `nrq_gc_collect`
- their `TirToMlir` aliases

Replacement paths are `memory.allocate<T>` / `memory.deallocate` and the namespaced
`gc.*` controls. Runtime C entry points remain for embedders and the runtime library;
they are no longer callable as language builtins.

**Retained deliberately:**

- Typed explicit allocation, owned arrays, SoA, shared pointers, secure storage,
  mappings, files, sockets and mutexes. These are supported paths, not legacy.
- `memory.owned_array`, `array.owned_release`, `aggregate.owned_drop` and typed root
  registration. They are the proven replacements, so removing them would delete the
  policy itself.
- Process-lifetime `file.read_text` storage, documented as a deferred limitation.

## 6. Documentation

- `docs/memory/management.md`: actual field contracts, shared aggregate support,
  container restrictions, storage-boundary policy table and nested generics.
- `docs/migration/phase-8.md`: changed defaults, supported/rejected patterns, GC
  usage, compatibility checklist and legacy-name replacement.
- `README.md`: current label, Phase 8 status and migration link.
- `docs/readiness.md`: marked as a historical Phase 6 audit instead of current status.
- `examples/memory/hybrid_memory.nrqba` and `cyclic_graph.nrqba`: runnable evidence.
- `docs/VERSION_LOG.md`: milestone entries and final counts.

## 7. Verification

Final clean build (`scripts/build.ps1 -Configuration RelWithDebInfo -Clean`):

- Build: **PASS**, 177/177 targets.
- Runtime parity: **104** invariant checks present in both configurations.
- Unit: **258/258 PASS**.
- Integration: **489/489 PASS** (474 at `0.499-beta`, +15 focused cases).
- Examples: both compile and run with expected output.
- No sanitizer run: the MSVC toolchain in this environment does not provide one.

Test-count note: the unit suite is one lower than Phase 7 because the two legacy GC
control tests were merged into the namespaced `gc.*` test; integration gained three
positive and twelve negative cases.

## 8. Performance

Representative allocation-policy compile benchmark, seven alternating repeats,
`RelWithDebInfo`, 400 allocation sites with managed-bearing payloads:

| Measurement | `0.499-beta` | `0.511-beta` | Change |
|---|---:|---:|---:|
| Compiler wall | 287.989 ms | 287.472 ms | -0.18% |
| Peak RSS | 53,813,248 B | 53,653,504 B | -0.30% |
| Object bytes | 32,319 | 32,319 | 0% |
| Executable bytes | 146,944 | 146,944 | 0% |

Neutral, within run-to-run spread; no speedup is claimed. The policy is compile-time
only and adds no runtime metadata. Artifacts and harness:
`benchmarks/pre_mmm_phase8/`.

## 9. Commit list

Implementation and milestones:

- `22c6258` compiler: reject deterministic owners in collector-managed arrays
- `e5fdcbe` compiler: reject deterministic owners in explicit allocations
- `215110e` release: bump to 0.501-beta after the storage-boundary rules
- `8ebff9d` compiler: reject deterministic owners in shared allocations
- `44df466` compiler: reject reference-counted and borrowed owned-array payloads
- `a46eb05` compiler: reject reference-counted and borrowed SoA columns
- `a3186fd` parser: split nested generic closers in type contexts
- `52bc0ac` compiler: remove legacy nrq_memory allocation builtins
- `b6e1eb0` compiler: remove legacy nrq_gc language builtins
- `8ea8734` release: bump to 0.505-beta after shared and bulk-array rules
- `ddc5d9e` release: bump to 0.508-beta after SoA and nested-generic rules
- `d518691` release: bump to 0.511-beta after removing legacy language builtins

Tests, documentation, audit and benchmark:

- `a0962c7` docs: document the storage-boundary ownership policy
- `68ebb0f` docs: add the Phase 8 migration guide
- `566350f` docs: add a runnable hybrid memory example
- `39c91de` docs: point the project status at Phase 8
- `f320e78` docs: mark the readiness audit as historical
- `8fd638e` docs: record the Phase 8 supported-surface audit
- `de5f188` bench: measure the allocation-boundary policy checks
- `b12f69b` docs: record the Phase 8 benchmark result
- `f5c608d` test: cover managed payloads in secure storage
- `4aed203` docs: record the final 489-test integration count
- `b24d427` docs: correct the raw pointer field contract
- `4811917` docs: add a cyclic managed-graph example

The final progress/roadmap/report closeout commits follow this file.

## 10. Remaining limitations

These are preserved, not silently absorbed into the Phase 8 claim:

1. Process-lifetime `file.read_text` buffers have no bounded release path.
2. Publication crash recovery remains outside the ownership work.
3. Busy-registry termination cleanup remains deferred.
4. Collector-owned SoA storage is reclaimed asynchronously by sweep, not synchronously
   by `soa.owned_release`.
5. Bulk containers intentionally have no per-element finalizers; arrays of shared or
   owned-field aggregates are an explicit rejected pattern, not a supported container.
6. No sanitizer coverage is available in this MSVC environment.
7. One earlier unit run segfaulted in a GC stress test and passed on rerun; the final
   clean run passed. It is recorded as a pre-existing intermittent failure, not
   attributed to Phase 8.
8. The 4,000-site benchmark variant exhausted LLVM memory in this environment; the
   recorded 400-site workload is bounded and documented.

No push or publication was performed.
