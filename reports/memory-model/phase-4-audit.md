# Phase 4 Audit — Ordinary Non-GC Allocation

**Repository:** `D:\Actual Project Files\Project Nirqoba`
**Baseline:** `0.547-beta`, commit `f6c406d`, clean working tree
**Scope:** MMM Phase 4 only. This audit does not implement Phase 5 arena
semantics, Phase 6 shared-ownership completion, Phase 7 GC isolation, or Phase 8
manual/FFI work.

---

## 1. What was inspected

- `project_context/MMM_PLAN.md`, Phase 4 tasks and acceptance gates.
- `reports/memory-model/phase-1.md`, `phase-2.md`, `phase-3.md`.
- `reports/memory-model/target-architecture.md`.
- `reports/memory-model/migration-audit.md`, including the post-audit
  `0.515-beta` resolution of the unconditional owned-release unregister.
- Sema classification in `compiler/lib/Sema/TypeChecker.cpp`:
  `expression_creates_owned_pointer`, `cleanup_for_owner_producer`,
  `cleanup_for_owned_expression`, `memory.allocate`, `memory.owned_array`,
  `array.allocate`, shared/secure allocation and the managed-payload rejection
  rules.
- TIR construction in `compiler/lib/TIR/Builder.cpp` and the ownership records in
  `compiler/include/nirqoba/TIR/IR.h`.
- TIR verification in `compiler/lib/TIR/Verifier.cpp`.
- Lowering in `compiler/lib/Lowering/TirToMlir.cpp`:
  `analyze_stack_allocations`, `analyze_promoted_slots`,
  `function_uses_gc`, `analyze_gc_root_plan`, `release_payload_has_managed_reference`,
  `drop_aggregate_fields`, `array.owned_release`, `soa.owned_release`.
- Runtime allocation and release in `runtime/src/allocator_windows.cpp`:
  `nrq_memory_allocate`, `nrq_memory_deallocate_checked`,
  `nrq_memory_reallocate`, `nrq_memory_shared_*`,
  `nrq_memory_secure_*`, allocator registry and ownership records.
- GC runtime entry points in `runtime/include/nirqoba/runtime/gc.h` and
  `runtime/src/gc.cpp`.
- Runtime startup/shutdown in `runtime/src/io_windows.cpp` and
  `runtime/src/system_windows.cpp`.
- Generated IR for owned pointer, owned array, owned aggregate, borrowed view and
  stack-promoted array fixtures at `-O0` and `-O2`.
- Existing IR isolation fixtures under `tests/integration/positive/`, especially
  `owned_*_gc_isolation_o0`, `owned_*_gc_isolation_o2`,
  `stack_owned_array_gc_isolation_o0`, `owned_pointer_gc_roots`,
  `owned_array_gc_roots`, `gc_root_elision_owned`.

---

## 2. Classification of allocation categories

| Category | Current representation and allocation path | GC dependencies | Required Phase 4 target |
|---|---|---|---|
| Ordinary owned scalar/local | TIR `alloca`/`store`/`load`; LLVM frame or SSA value after slot promotion | None | Preserve stack/inline storage; no allocation or GC. |
| Ordinary owned pointer (`memory.allocate<T>`) | Sema returns a plain pointer with `ValueOwnership::Owned`; TIR `Call`; MLIR `nrq_memory_allocate`; release `memory.deallocate` → `nrq_memory_deallocate_checked` | None for a payload without managed references | Keep on the general allocator; no GC header, root, frame, safepoint, mutator or barrier. |
| Ordinary owned array (`memory.owned_array<T>`, `_aligned`) | Sema `TypeKind::OwnedArray`, ownership `Owned`; TIR call; MLIR general allocator or explicit allocator; release `array.owned_release` | Root register/unregister only when an element contains `managed<T>`; otherwise none | Keep the same path. GC operations remain conditional on a managed element type. |
| Stack/inline owned array | `analyze_stack_allocations` promotes bounded, non-escaping arrays to frame storage; release skips deallocation | None when the element has no managed reference | Preserve promotion and cleanup; do not force heap storage. |
| Ordinary owned aggregate/struct | By-value aggregate in a slot or returned by value; `aggregate.owned_drop` walks fields in reverse declaration order | Only managed-reference leaves are rooted/unregistered | Preserve deterministic recursive destruction; no GC for plain fields. |
| Ordinary owned pointer with managed payload (`memory.allocate<managed<T>>` or aggregate containing it) | General allocator plus typed root registration; release unregisters before deallocation | GC root registration, root frame and mutator region are required | This is a mixed-storage case, not an ordinary owned-only case. Keep the GC dependency. |
| Borrowed pointer (`&x`, `borrow T*`) | TIR `AddressOf`/`Borrow`/lifetime record; LLVM address only | None | Keep allocation-free and GC-free. |
| Borrowed array view (`memory.array_view`, `_mut`) | Two-word descriptor over existing storage; `DynamicIndex`/`GetElementPtr` | None; owner release remains conditional | Keep allocation-free and GC-free. |
| Explicitly GC-managed object (`array.allocate<T>`) | Sema `managed<T>`/`GcManaged`; MLIR typed collector allocation; collector root registration and handoff | GC allocation, headers, roots, root frames, mutator, safepoints, barriers | Unchanged. This is the explicit GC path. |
| SoA container (`memory.owned_soa<T>`) | Collector-owned strided block; `soa.owned_release` unregisters the root and the collector reclaims it | GC allocation, root registration, collector reclamation | Classified as an explicitly GC-backed container despite its historical `owned_soa` spelling. Not an ordinary owned allocation. |
| Shared-owned (`memory.shared_allocate<T>`) | General allocator plus runtime reference-count record; optional typed root path when the payload contains managed references | None for a plain payload; rooted shared storage for managed payloads | Keep explicit; do not add reference counting to ordinary ownership. |
| Secure (`memory.secure_allocate<T>`) | Secure runtime allocation and `memory.secure_release`; managed and destructible payloads are rejected | None | Keep GC-free and reject payloads that would need rooting or field destruction. |
| Reallocated explicit storage (`memory.reallocate<T>`) | General allocator reallocation; old address unregistered and new address re-registered only for managed payloads | Conditional on managed payload | Keep ownership/pairing; no unconditional GC call. |
| Manually managed/raw pointer | Plain pointer with no owned lifetime when passed as `T*`; explicit allocation remains owned | None | Remains the manual escape hatch; no hidden GC fallback. |
| Resource-owned handles (`file`, `socket`, `mutex`, `mapping`) | Runtime handle and domain-specific close/unmap/destroy | None | Unchanged; not memory allocation. |
| Strings from `file.read_text` | Process-lifetime arena, borrowed `string` | None | Documented exception; owned string is later work. |
| Native/process storage | Runtime internal tables and process-lifetime buffers | None for ordinary user values | Outside Phase 4 user-allocation scope. |

### 2.1 Category totals relevant to Phase 4

- **Ordinary owned:** scalar/local, owned pointer, owned array, owned aggregate,
  stack/inline array, reallocated explicit storage.
- **Borrowed/non-owning:** `&x`, borrowed parameters, array views, borrowed
  strings.
- **Explicitly GC-managed:** `array.allocate<T>`, `managed<T>` fields inside
  explicit/shared/owned storage, `memory.owned_soa<T>` (GC-backed container).
- **Shared-owned:** `memory.shared_allocate<T>` and `shared_retain`.
- **Manually managed:** raw pointers and explicitly paired allocation/release.
- **Other special-purpose:** secure storage, native resource handles,
  process-lifetime strings.

---

## 3. Current GC dependency map

### 3.1 Compiler

- `function_uses_gc` classifies a function as GC-touching when a type in its
  signature, result, operand, local slot or argument contains a managed
  reference, or when an explicit allocation has a managed payload.
- `uses_gc_mutator_` is passed only for those functions.
- `needs_gc_root_frame_` requires `uses_gc_mutator_` and an actual root plan.
- Safepoint insertion checks `uses_gc_mutator_`.
- `emit_gc_root_registration` returns immediately when the type has no managed
  reference offsets.
- `release_payload_has_managed_reference` gates owned-pointer and owned-array
  unregistration.
- `drop_aggregate_fields` unregisters only managed leaves; plain owned fields go
  directly to `nrq_memory_deallocate_checked`.
- `soa.owned_release` is intentionally GC-backed.

### 3.2 Runtime

- `nrq_memory_allocate`, `nrq_memory_allocate_array`,
  `nrq_memory_allocate_array_from`, `nrq_memory_deallocate_checked` and
  `nrq_memory_reallocate` use the general/explicit allocator registry. They do
  not call the collector.
- `nrq_runtime_init` is empty. Ordinary startup does not initialize or register a
  mutator.
- `nrq_runtime_shutdown` releases native-resource registries and process strings;
  it does not collect.
- `nrq_gc_*` allocation, roots, frames, mutators, safepoints and barriers remain
  reachable only through explicit collector paths or mixed managed payloads.
- `nrq_memory_shared_allocate_rooted` and final shared release use collector
  roots only when the payload was explicitly rooted.
- `nrq_memory_secure_allocate` has no GC path.

### 3.3 IR evidence at the audit baseline

Representative fixtures were compiled with `--emit=llvm-raw` at `-O0` and `-O2`.
The count is occurrences of the substring `nrq_gc` in the emitted module.

| Fixture | `-O0` GC references | `-O2` GC references | Ordinary deallocation evidence |
|---|---:|---:|---|
| owned pointer (`memory.allocate<Box>`) | 0 | 0 | stack-promoted; no runtime free needed |
| owned array (`memory.owned_array<int>`) | 0 | 0 | stack-promoted; no runtime free needed |
| owned aggregate returned by value | 0 | 0 | `nrq_memory_deallocate_checked` present |
| borrowed view over owned array | 0 | 0 | no allocation; owner path only |
| stack-owned array | 0 | 0 | no heap deallocation |
| explicit managed array | expected non-zero | expected non-zero | collector allocation/root path |

The existing isolation fixtures additionally assert zero `nrq_gc_root_unregister`
and zero root-frame/register/mutator/safepoint calls for owned-only programs, and
one unregister for a managed payload. Mixed cases such as
`owned_pointer_gc_roots` and `owned_array_gc_roots` must retain their roots.

---

## 4. Findings

### 4.1 The ordinary runtime path is already non-GC

`memory.allocate<T>`, `memory.owned_array<T>`, owned aggregates and stack
promotion already use the general allocator and deterministic destruction. The
unconditional `nrq_gc_root_unregister` identified in the original migration
audit was removed during the `0.515-beta` hardening: unregistration is now gated
on `release_payload_has_managed_reference` and on managed leaves in aggregate
drop. Current probes find no GC symbol at all in the owned-only fixtures.

This means Phase 4 does **not** need a second allocator or a new runtime
abstraction. The instruction to reuse existing allocator infrastructure is
satisfied by the general allocator, explicit allocator handles, stack promotion,
and the ownership registry.

### 4.2 Gap: allocation strategy was not verified in TIR

Sema and lowering agree on the allocation path, but TIR verification previously
checked ownership categories generically. A malformed module could describe a
`memory.allocate` result as `gc_managed`, or an `array.allocate` result as
ordinary owned, without a rule naming the allocation strategy. The runtime path
would then be selected by the callee while the ownership contract claimed
something else.

Phase 4 closes this by making allocation strategy part of the verifier contract:

- ordinary allocation calls require `Owned` ownership and plain pointer/owned
  array storage;
- collector allocation requires `GcManaged` and a managed pointer;
- shared allocation requires `Shared` and a pointer.

This does not change generated code. It prevents a future transformation from
silently moving ordinary storage onto the collector path or the reverse.

### 4.3 GC remains for explicit collector objects

`array.allocate<T>` and `memory.owned_soa<T>` remain collector-backed. SoA is a
special container whose release is root unregistration and collector sweep, not
ordinary deallocation. Managed payloads inside explicit, owned or shared storage
also legitimately require typed roots and a mutator region. These are mixed
strategies, not failures of Phase 4.

### 4.4 No GC header is used by ordinary storage

The general allocator records allocation ownership in its registry; it does not
place a collector header in front of the payload. Collector headers and layout
descriptors are created only by `nrq_gc_allocate*`. Ordinary release reaches the
allocator directly.

---

## 5. Target path and implementation plan

1. Keep `memory.allocate`, `memory.owned_array`, `memory.owned_array_aligned`,
   `memory.reallocate`, `memory.secure_allocate` and `memory.shared_allocate`
   on the existing general/secure/shared runtime allocators.
2. Keep stack allocation and the existing single-store/non-escaping promotion
   analysis. Do not promote address-taken, managed-rooted or escaping storage.
3. Keep deterministic cleanup: `array.owned_release`, `aggregate.owned_drop`,
   `memory.deallocate`, resource-domain releases, and exactly-once obligations.
4. Gate every GC operation on actual managed-reference content. Ordinary payloads
   must not register roots, open frames, enter mutators, poll safepoints or call
   unregister.
5. Add TIR allocation-strategy verification so ownership metadata cannot diverge
   from the selected allocation path.
6. Add IR-level regressions for ordinary pointer, array, aggregate, borrowed view,
   stack promotion and the mixed managed case.
7. Preserve `array.allocate`, SoA, managed fields and collector allocation as the
   explicit GC paths.

### 5.1 Files expected to change

- `compiler/lib/TIR/Verifier.cpp` — allocation-strategy contract.
- `tests/unit/ownership_test.cpp` — malformed allocation contracts.
- `tests/integration/positive/` — IR isolation and mixed-path evidence where a
  gap is found.
- Phase 4 documentation, version log and reports.

### 5.2 Existing tests that may need semantic updates

None expected. The allocation contract describes current emitted metadata. If a
fixture fails, the failure identifies a real disagreement between Sema ownership
and the lowered allocation path; the fixture must not be weakened.

---

## 6. Audit conclusion

Ordinary owned allocation is already separated from the collector at the runtime
and lowering levels. The remaining Phase 4 work is to make that separation
explicit and non-regressable in TIR verification, preserve stack/inline and
deterministic cleanup behavior, add IR evidence for the owned-only and mixed
paths, and document the classification above. Phase 4 does not remove or disable
the collector.
