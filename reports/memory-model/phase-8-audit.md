# Phase 8 audit — manual memory and foreign boundaries

**Baseline:** `0.575-beta`, commit `d3f2df8` (`main`)
**Audit date:** 2026-11-16
**Scope:** every existing explicit allocation, deallocation, allocator handle,
native resource owner, raw-pointer escape and foreign-function boundary.

This audit was written before Phase 8 implementation. It separates what the
compiler and runtime actually support from what the migration plan names as a
target. In particular, it does **not** assume that a runtime C ABI is a
source-level FFI, and it does not treat an ordinary owned pointer as a manually
managed pointer.

---

## 1. Method

The following paths were inspected:

- `compiler/include/nirqoba/Common/Type.h` — `ValueOwnership`, pointer kinds.
- `compiler/lib/Common/Type.cpp` — ownership predicates and names.
- `compiler/lib/Sema/TypeChecker.cpp` — allocation/deallocation checks,
  cleanup selection, allocator handles, resource domains, FFI search.
- `compiler/lib/TIR/Builder.cpp` — ownership carried into TIR, obligations and
  deferred destruction.
- `compiler/lib/TIR/Verifier.cpp` — allocation contracts, releases and
  consumption.
- `compiler/lib/Lowering/TirToMlir.cpp` — allocation/reallocation/release
  lowering, GC-root registration, collection-free call list.
- `runtime/include/nirqoba/runtime/allocator.h` — allocator kinds, custom
  allocator ABI, allocation/reallocation/release entry points.
- `runtime/src/allocator_windows.cpp` — allocator registry, ownership records,
  in-flight operations, secure and shared paths.
- `runtime/src/file_windows.cpp`, `socket_windows.cpp`, `sync_windows.cpp`,
  `mapping_windows.cpp` — native resource ownership.
- `runtime/include/nirqoba/runtime/gc.h`, `runtime/src/gc.cpp` — root, mutator
  and barrier contract.
- `reports/memory-model/migration-audit.md` §§5.6, 5.7, 6, 8;
  Phase 4–7 audits and completion reports; `docs/architecture/06-runtime.md`.
- Parser, AST and Sema were searched for `extern`, `ffi`, `foreign`, `unsafe`,
  `callback`, `dll` and source-level ABI declarations.

The starting branch was `main`, HEAD `d3f2df8`, version `0.575-beta`, with a
clean working tree.

---

## 2. Ownership vocabulary at baseline

`ValueOwnership` contains:

| Category | Baseline meaning |
|---|---|
| `Trivial` | no ownership semantics |
| `Owned` | deterministic owner with a destruction obligation |
| `Borrowed` / `MutableBorrowed` | non-owning lifetime-bound access |
| `Shared` | explicit reference-counted owner |
| `GcManaged` | collector-managed reference |
| `Raw` | raw pointer with no compiler-managed lifetime |
| `ArenaBound` | lifetime owned by an arena/pool region |
| `Weak` | reserved; no weak-reference implementation |

There is **no `ManuallyManaged` enumerator**. `Raw` is the closest existing
name, but it explicitly means “no compiler-managed lifetime” and is used for
address-derived pointers. It is not an owning category: `is_owning_ownership`,
`is_transferable_ownership` and `requires_destruction` are all false for `Raw`.
Making manual allocation produce `Raw` would therefore lose move, release and
double-release enforcement. The audit conclusion is that Phase 8 may add a
distinct `Manual` category using the existing ownership vocabulary; it must not
silently coerce manual storage into `Owned`, `Shared`, `ArenaBound` or
`GcManaged`.

---

## 3. Existing explicit-memory inventory

### 3.1 `memory.allocate<T>(bytes[, allocator])`

- **Allocation source:** process default allocator, the current explicit
  allocator when the one-argument form is used, or the allocator handle passed
  as the second argument.
- **Strategy:** the selected `Allocator` implementation (general, pool, arena,
  stack, slab, buddy, free list, TLSF or a registered custom allocator).
- **Alignment:** `alignof(T)` through `type_alignment`; an explicit aligned
  variant does not exist for `memory.allocate`.
- **Ownership:** `Owned` normally; `ArenaBound` when Sema resolves the second
  argument to a tracked `memory.arena`/`memory.pool` handle.
- **Release:** `memory.deallocate` for ordinary storage; `memory.pool_release`
  for a tracked pool slot; arena/pool reset or destroy for tracked arena
  storage. Sema rejects releasing with the wrong domain.
- **Move / borrow:** ownership transfer follows the ordinary `Owned` rules;
  immutable and mutable borrows are tracked by allocation identity.
- **Across functions/modules:** owned pointers can be parameters and returns;
  borrow returns must name a live source parameter.
- **Across FFI:** no source-level FFI exists, so no pointer can cross one.
- **Managed references:** allowed for ordinary storage and registered as typed
  roots; rejected for tracked arena/pool storage because bulk reset/destroy has
  no root-unregistration point (`NRQ3015`).
- **Failure:** allocation returns a null pointer; the runtime records no owner
  for a failed request. Dereference is not automatically checked.
- **Layout/destruction:** payloads requiring deterministic field cleanup are
  rejected because deallocation frees raw storage without running field
  destructors.

### 3.2 `memory.reallocate<T>(pointer, bytes)`

- Preserves the owning allocator recorded for the original pointer.
- Preserves the original alignment; managed-payload rooting is moved from the
  old address to the replacement.
- On success the old address is invalid; on failure the original allocation is
  untouched and remains owned.
- `bytes == 0` releases the block, matching the runtime's `realloc` convention.
- The result is `Owned`; there is no reallocation path that preserves a manual
  or shared category. Shared storage is rejected. Deterministic-cleanup payloads
  are rejected.

### 3.3 `memory.secure_allocate<T>` / `memory.secure_release`

- Uses the ordinary allocator selected at the call site, then zero-fills the
  storage.
- `Owned` with the `secure` resource domain and `secure_release` cleanup.
- Release zeroes the recorded byte count before freeing.
- Managed payloads are not given a rooted secure-allocation path, so a managed
  field in secure storage would not be reliably unregistered. This is a
  documented gap, not a supported mixed path.

### 3.4 `memory.map_file` / `memory.unmap_file`

- A mapping is a resource owner, not an ordinary heap allocation.
- `unmap_file` is the only valid release; `memory.deallocate` is rejected by the
  resource domain check.
- Mapping size and mapped read/write are resource operations, not ownership
  transfers.
- There is no foreign contract allowing a mapping to outlive its unmap.

### 3.5 Allocator handles

- Source-level handles are opaque `u64` values (`memory.arena_allocator`,
  `general_allocator`, `pool_allocator`, `region`, `stack_allocator`,
  `slab_allocator`, `buddy_allocator`, `free_list_allocator`,
  `tlsf_allocator`).
- The runtime keeps handles in a process registry. An allocator may not be
  destroyed or reset while an allocation operation is in flight, while it is
  the default, while it is selected, or while it owns a live allocation.
- Handles are not reference-counted. Sema treats an arena/pool handle as moved
  by destroy; ordinary allocator handles have a manual root-lifetime contract.
- Custom allocators are registered through `NrqCustomAllocatorVTable`. The
  runtime copies the table, serializes callbacks per allocator, and requires
  each callback to pair the original size and alignment.

### 3.6 Native resource owners

| Producer | Release | Notes |
|---|---|---|
| `file.open` | `file.close` | resource domain `file` |
| `socket.connect` | `socket.close` | resource domain `socket` |
| `mutex.create` | `mutex.destroy` | resource domain `mutex` |
| `memory.map_file` | `memory.unmap_file` | resource domain `mapping` |
| `memory.secure_allocate` | `memory.secure_release` | resource domain `secure` |

These are deterministic owners: the resource domain selects cleanup, move
transfers the obligation, and `defer` can override the cleanup point. Their
handles are not memory blocks, and an unrelated memory deallocator is rejected.

### 3.7 Raw pointers and aggregates

- `&value` creates a borrowed pointer/view with a tracked allocation identity.
- Raw pointer fields require an explicit ownership qualifier; unqualified raw
  pointer fields are restricted.
- A raw pointer does not acquire a release obligation and cannot be transferred
  as an owner.
- Manual storage inside aggregates is therefore not represented today: there is
  no manual owner category for a field.

### 3.8 Runtime allocation registry

`allocator_windows.cpp` records each live explicit allocation with:

- allocator identity,
- byte count and alignment,
- shared flag,
- in-flight-operation flag.

`nrq_memory_deallocate_checked` rejects an invalid or already freed pointer and
rejects releasing shared storage through the ordinary path. Reallocation,
secure release and custom callbacks all keep the ownership record in flight so
allocator destruction/reset cannot race the callback. This is the existing
debug and production enforcement mechanism. It is a runtime ownership registry,
not a universal object header.

---

## 4. Trace through Sema → TIR → MLIR → LLVM → runtime

### 4.1 Allocation

1. **Sema** validates arity and argument types, resolves the payload, applies
   the deterministic-cleanup and arena-managed-payload policies, records the
   tracked arena identity, and returns a pointer type.
2. **Ownership assignment** happens in `check_expression`: a pointer-returning
   call becomes `ArenaBound` when an arena identity was recorded, `Shared` for
   shared producers, otherwise `Owned` for an allocation producer.
3. **TIR** stores `call.ownership()` on the result value. There is no
   allocator-provenance field separate from the arena identity; ordinary
   allocator identity is carried only by the argument and the runtime record.
4. **MLIR/LLVM** lowers `memory.allocate` to `nrq_memory_allocate` or
   `nrq_memory_allocate_from`/`nrq_arena_allocate`. It computes alignment from
   the payload type, emits a typed GC root when the payload contains managed
   fields, and may place a constant-size allocation on the stack.
5. **Runtime** selects the allocator, records ownership, and returns null on
   failure.

### 4.2 Release

1. Sema rejects borrowed, shared-wrong-path, wrong-resource and live-borrow
   releases and marks the owner moved after a valid consuming release.
2. TIR represents destruction as `Drop` with a release callee, or as a direct
   release call for deferred paths.
3. The verifier checks that a drop operand is owning and that the release
   matches the allocation family.
4. Lowering emits `nrq_memory_deallocate_checked`, `nrq_memory_secure_release`,
   `nrq_memory_shared_release`, `nrq_memory_unmap_file`, or the resource close
   call; it unregisters a typed root before freeing storage that can contain a
   managed reference.
5. The runtime validates the ownership record and allocator identity before
   calling the allocator.

### 4.3 Reallocation

Sema consumes the source owner, MLIR calls `nrq_memory_reallocate`, and the
runtime keeps the original allocation valid when the allocator cannot provide a
replacement. Root registration moves only after a non-null replacement.

---

## 5. FFI and foreign boundaries

### 5.1 Actual source-level surface

There is **no source-level FFI**:

- no `extern` declaration syntax;
- no foreign-function declaration node in the AST;
- no C ABI type or calling-convention annotation;
- no `unsafe` scope;
- no callback type;
- no foreign-pointer type distinct from a Nirqoba pointer;
- no dynamic-library or symbol-resolution builtin.

The parser rejects `extern` as an unknown declaration. The runtime's `extern "C"`
entry points are an internal implementation boundary used by generated code;
they are not callable from Nirqoba source and carry no source-visible ownership
contract.

### 5.2 Classification

| Boundary | Classification | Consequence |
|---|---|---|
| Borrowed foreign pointer | **Unsupported** | No declaration surface; cannot be expressed |
| Ownership transferred into Nirqoba | **Unsupported** | No foreign allocation type or importing contract |
| Ownership transferred out of Nirqoba | **Unsupported** | No foreign ABI or release-function parameter |
| Foreign function returns allocated memory | **Unsupported** | No foreign return contract |
| Foreign function retains a pointer | **Unsupported** | Cannot be represented or checked |
| Callback into Nirqoba | **Unsupported** | No callback type or runtime callback bridge |
| GC-managed handle across FFI | **Unsupported** | No FFI path; collector roots are internal |
| Shared token across FFI | **Unsupported** | No FFI path; no foreign retain/release contract |
| Arena/pool pointer across FFI | **Unsupported** | No FFI path; Phase 5 escape rules remain authoritative |
| Unknown/external calls triggering collection | **Conservatively treated as collection points** | Lowering's allowlist assumes any unknown callee may collect; this is an internal safety policy, not FFI support |

No FFI safety claim is made. The supported rule is: foreign pointers and foreign
ownership cannot enter or leave a Nirqoba program through the implemented
language surface. Adding an ABI, callback bridge or foreign ownership annotation
would be a new language feature, not a Phase 8 repair.

### 5.3 Consequence for Phase 8

Phase 8 cannot “enforce” a foreign contract that does not exist. It will:

1. preserve the absence of an FFI surface rather than inventing one;
2. add a regression that an `extern` declaration remains a compile-time error;
3. document the contract vocabulary that a future FFI implementation must use,
   without presenting it as implemented;
4. spend implementation effort on the manual ownership gap that is actually
   present in the language.

This is the conservative reading of “do not invent unsupported ABIs”.

---

## 6. Manual-memory findings

| ID | Finding | Severity | Decision |
|---|---|---|---|
| MM1 | No distinct manual ownership category | High | Add `ValueOwnership::Manual` |
| MM2 | `Raw` cannot serve as manual: it is non-owning and non-transferable | High | Keep `Raw` for address-derived pointers |
| MM3 | No source-level operation selects manual allocation | High | Add `memory.manual_allocate<T>` |
| MM4 | No explicit manual release spelling distinct from ordinary deallocation | High | Add `memory.manual_release` |
| MM5 | Cleanup selection is callee-spelling based; a new callee would otherwise fall back to `memory.deallocate` | Medium | Add explicit cleanup mapping |
| MM6 | TIR allocation contracts would reject a manual result | Medium | Add a `ManualPointer` storage contract and release pairing |
| MM7 | Lowering recognizes only `memory.allocate` for allocation/root/reallocation treatment | Medium | Extend allocation, release, root and collection-free paths |
| MM8 | Reallocation is owned-only; manual reallocation has no defined category | Medium | Reject manual reallocation and document release/reallocate as the supported path |
| MM9 | Sema move/double-release/borrow checks are keyed to `owns_allocation`, not a manual flag | Medium | Track manual provenance on `Symbol` and reuse the existing move/lifetime machinery |
| MM10 | Custom allocator identity is runtime-recorded but not a source type | Acceptable | Preserve runtime identity; do not add per-object metadata |
| MM11 | Native resources already have deterministic cleanup and are not memory blocks | No defect | Preserve `file`/`socket`/`mutex`/mapping contracts |
| MM12 | Secure release and ordinary deallocation are already separated | No defect | Preserve; secure storage is not manual heap storage |
| MM13 | Arena/pool bulk lifetime has no per-object release | No defect | Manual allocation must reject tracked arena/pool handles |
| MM14 | No statically provable use-after-free beyond Sema's moved-binding analysis | Known limit | Keep runtime invalid-pointer diagnostics for the unprovable cases |

### 6.1 Chosen manual contract

A manual allocation will:

- be produced only by `memory.manual_allocate<T>(bytes[, allocator])`;
- have ownership category `Manual`, distinct from `Owned`, `Shared`, `Raw`,
  `ArenaBound` and `GcManaged`;
- be releasable only by `memory.manual_release(pointer)`;
- reject `memory.deallocate`, `memory.shared_release`, `memory.secure_release`,
  `memory.pool_release`, `memory.reallocate` and all resource close calls;
- obey the same move, borrow, exactly-once and live-borrow rules as an ordinary
  owner;
- preserve the selected allocator in the runtime ownership record;
- reject tracked arena/pool handles, because their lifetime is bulk-owned by the
  arena/pool and cannot be mixed with a per-binding manual release;
- reject deterministic-cleanup payloads for the same reason ordinary explicit
  storage rejects them;
- permit managed payloads only under the existing typed-root contract;
- retain a compiler-scheduled fallback release for safety, while allowing the
  programmer to make release explicit. The fallback is not a silent conversion
  to `Owned`: the binding remains `Manual` in TIR and the release spelling is
  `memory.manual_release`.

The fallback is deliberate. An unreleased manual allocation is a leak, not a
safety violation, but Nirqoba's ownership-first model already has deterministic
cleanup at every lexical exit. Keeping the fallback lets manual code opt into
explicit lifetime control without making the rest of the language unsafe by
default.

---

## 7. Unsupported and deferred cases

- Foreign declarations, foreign pointers, ownership transfer across an ABI,
  retained foreign pointers and callbacks: **unsupported**.
- Manual reallocation: **deferred**; release and allocate a replacement.
- Manual allocation from a tracked arena or pool: **rejected**; use the arena
  path and `pool_release`/arena reset.
- Manual allocation with deterministic-cleanup fields: **rejected**.
- Manual weak/shared conversions: **not provided**; shared and manual remain
  distinct.
- Automatic allocator selection or CDMM: **not Phase 8**.
- Weak references and new cycle collection: **not Phase 8**.
- Universal object headers or per-object manual tags: **not added**.

---

## 8. Planned verification

- Unit tests for `Manual` classification, ownership predicates and TIR
  contracts.
- Positive lifecycle, move, borrow, allocator-handle, custom-allocator and
  managed-reference fixtures.
- Negative release-mismatch, double-release, use-after-release, borrow-release,
  arena/pool and reallocation fixtures.
- Parser rejection of `extern`.
- O0/O2 TIR/LLVM checks for manual allocation and release.
- Existing Phase 1–7 suites and Phase 7 GC-isolation script.
- Benchmarks comparing equivalent manual and ordinary owned allocation and
  release under the same build.
