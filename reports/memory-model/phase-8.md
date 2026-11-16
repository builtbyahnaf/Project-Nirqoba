# Phase 8 completion report — manual memory and the FFI boundary

**Starting version:** `0.575-beta`  
**Starting commit:** `d3f2df8ef209183c07759721b042303d21c043a0`  
**Final version:** `0.583-beta`  
**Final Phase 8 report commit:** the 27th commit after `d3f2df8` (this file)  
**Branch:** `main`  
**Previous phase:** Phase 7 — GC Isolation and Explicit Selection

---

## 1. Result

Phase 8 adds a distinct, verified manual-memory strategy without weakening
ordinary ownership, borrowing, arenas, shared ownership or explicit GC.

The new surface is:

```nrqoba
var value = memory.manual_allocate<int>(4);
*value = 41;
memory.manual_release(value);
```

`memory.manual_allocate<T>(bytes[, allocator])` produces
`ValueOwnership::Manual`. `memory.manual_release` is its only release. The
category survives Sema → TIR → MLIR/LLVM → the runtime ownership registry.

**FFI was not implemented.** The audit found no source-level `extern`, foreign
pointer type, callback type, ABI annotation or unsafe scope. Rather than invent
an unsupported ABI, Phase 8 records FFI ownership transfer, borrowed foreign
pointers, retained foreign pointers, callbacks and foreign GC/shared/arena
interaction as explicitly unsupported. The parser rejection of `extern` is
tested. This is the honest boundary of the current language, not a claim of FFI
safety.

---

## 2. Numbered commit list

`1`–`26` are the implementation, test, benchmark and documentation commits
before this report; `27` is this completion report.

1. `d259165` — audit existing manual memory and foreign boundaries
2. `e9cc31e` — add a distinct manual ownership category
3. `c6aef5c` — parse typed manual allocation calls
4. `13a3f6e` — track manual allocation provenance in semantic analysis
5. `976b112` — represent manual allocation and release contracts in TIR
6. `a1f3bb3` — lower manual allocation through the selected allocator
7. `e61845b` — bump the beta version for the manual ownership category
8. `40dee82` — test manual allocation, explicit release and allocator provenance
9. `11d1294` — test manual GC rooting and branch-complete release
10. `328c939` — test manual release pairing and lifetime rejection
11. `e4fd474` — test manual allocation boundaries and absent FFI surface
12. `78fc711` — test the manual TIR ownership and release contract
13. `d53626d` — benchmark manual allocation, borrows and managed payloads
14. `a7c0716` — record Phase 8 benchmark results at 0.583-beta
15. `8beaf40` — bump the beta version and phase for manual memory
16. `8c2fde6` — document manual allocation and allocator pairing
17. `4586bd1` — document the manual ownership and migration contract
18. `5844407` — add the Phase 8 memory-model overview and manual ownership guide
19. `ef42d74` — document the absent FFI boundary and manual GC rooting
20. `29f7d45` — record Phase 8 in the version log
21. `edf5574` — update the roadmap for Phase 8
22. `195749f` — record Phase 8 completion in the migration plan
23. `2bc9a2b` — add the Phase 8 progress entry
24. `3be86de` — update the target architecture for manual storage
25. `3363c71` — list the manual-memory regression fixtures
26. `82e0e79` — report Phase 8 benchmark methodology and results
27. this report — Phase 8 completion report

No assistant co-author trailer was added. Nothing was pushed, published or
released.

---

## 3. Manual ownership semantics

### 3.1 Category

`ValueOwnership::Manual` is:

- **owning** — a release obligation exists;
- **transferable** — local moves transfer the obligation;
- **destruction-bearing** — the lexical fallback releases it;
- **distinct** — it is not `Owned`, `Raw`, `Shared`, `ArenaBound` or
  `GcManaged`.

`Raw` was deliberately not reused: it is non-owning and non-transferable, so it
cannot carry move/double-release/lifetime checks.

### 3.2 Allocation

`memory.manual_allocate<T>(bytes[, allocator])`:

- uses the selected default/current allocator or the explicit `u64` handle;
- records the allocator, byte count and alignment in the runtime ownership
  registry;
- uses the payload type's natural alignment;
- returns null on allocation failure;
- rejects tracked `memory.arena`/`memory.pool` handles;
- rejects payloads with fields requiring deterministic cleanup;
- permits managed references under the existing typed-root contract.

### 3.3 Release

`memory.manual_release(pointer)`:

- requires a live manual allocation;
- rejects a borrowed pointer, a repeated release and a release while a borrow is
  live;
- marks the allocation moved so later use is rejected;
- preserves fallback cleanup at every supported lexical exit.

No per-object header or global manual tag was added. The existing runtime
ownership record is the source of allocator identity and invalid/double-release
diagnostics.

### 3.4 Moves and borrows

A local move produces `tir.move` and invalidates the source binding. The result
keeps `Manual`. A borrow is non-owning and does not discharge the obligation.
Releasing while a borrow is live is rejected, as is using the owner after release.
Conditional releases on every branch are supported.

### 3.5 Function and module boundary

Manual storage is deliberately function-local. The language has no manual pointer
type that could carry provenance through a signature, and silently degrading to
an ordinary owned parameter would let the callee call `memory.deallocate`.
Therefore:

- passing a manual owner to an owned parameter is rejected;
- returning a manual owner is rejected;
- passing it to a `borrow` parameter is supported;
- release it before the call if ownership must move.

This is a documented limitation, not an unverified claim.

### 3.6 Reallocation

`memory.reallocate` rejects manual storage. `memory.reallocate` preserves the
original allocator on success and leaves the block valid on failure, but it has
no manual-result category today. Supported migration is release + allocate a
replacement.

---

## 4. Allocator integration and provenance

- **Existing infrastructure reused.** Manual allocation goes through the same
  general/pool/arena/stack/slab/buddy/free-list/TLSF/custom allocator registry as
  ordinary explicit allocation.
- **Identity preserved.** The live ownership record associates each pointer with
  its allocator. Release returns it to that allocator; the runtime rejects
  invalid or already-freed pointers.
- **Alignment.** The payload type determines natural alignment. There is no
  separate manual alignment argument in this phase.
- **Failure.** A null result creates no live owner. Ordinary and manual
  allocations share this failure behavior.
- **Allocator lifetime.** Destroy/reset still refuse while an allocation is live
  or in flight. The source-level handle remains a manual root-lifetime contract.
- **Custom allocators.** Registered through the existing
  `NrqCustomAllocatorVTable` ABI. Manual allocation can reach a custom handle
  supplied by native code; callbacks receive the original size and alignment.
  Source-level custom-vtable construction is still unavailable.
- **No global registry duplication.** Manual storage uses the existing sharded
  ownership registry, not a second allocator or a universal object header.

---

## 5. Native resources

No new native-resource framework was added. Existing deterministic owners remain:

| Resource | Producer | Release |
|---|---|---|
| File | `file.open` | `file.close` |
| Socket | `socket.connect` | `socket.close` |
| Mutex | `mutex.create` | `mutex.destroy` |
| Mapping | `memory.map_file` | `memory.unmap_file` |
| Secure buffer | `memory.secure_allocate` | `memory.secure_release` |

These are not manual heap allocations. Their resource-domain cleanup is
unchanged, moves transfer ownership, and an unrelated memory release is rejected.
Phase 8 adds no claim that arbitrary OS/device resources are supported.

---

## 6. FFI contracts and unsupported boundaries

There is **no implemented FFI**. The source surface has no foreign declaration,
pointer type, callback, ABI annotation or unsafe scope. The runtime's C entry
points are internal generated-code calls.

| Boundary | Phase 8 status |
|---|---|
| Borrowed foreign pointer | Unsupported; cannot be expressed |
| Ownership transferred into Nirqoba | Unsupported; no foreign allocation type or release contract |
| Ownership transferred out of Nirqoba | Unsupported; no foreign ABI or release function |
| Foreign allocation with custom deallocator | Unsupported |
| Retained foreign pointer | Unsupported; no representable retained lifetime |
| Callback / reentrant boundary | Unsupported |
| GC handle across FFI | Unsupported; no FFI path |
| Shared token across FFI | Unsupported; no FFI path |
| Arena/pool pointer across FFI | Unsupported; no FFI path |
| Unknown call may collect | Treated conservatively as a collection point in generated code |

The contract vocabulary a future FFI must satisfy is documented in
`docs/memory-model/interop.md`, clearly labelled as unimplemented. No foreign
safety claim is made.

---

## 7. GC and shared-ownership interoperability

### GC

- Plain manual storage emits no `nrq_gc` IR or object dependency.
- A manual payload containing `managed<T>` registers a typed root after
  allocation and unregisters it before release.
- A manual payload requiring deterministic field cleanup is rejected, so root
  cleanup and field destruction cannot silently diverge.
- No foreign pointer is registered as a GC root.
- Phase 7's production/debug isolation and collector behavior are preserved.

### Shared

- Manual and shared storage remain distinct categories.
- Manual release cannot release a shared control block.
- Shared release cannot release manual storage.
- No weak reference, new cycle collector or shared-to-shared cycle support was
  added.

### Arena and pool

- Manual allocation rejects tracked arena/pool handles.
- Arena/pool reset/destroy still reject live values or borrows.
- `memory.pool_release` remains the only per-slot pool release.
- Phase 5 lifetime rules are unchanged.

---

## 8. Compiler and runtime changes

### Compiler

| Layer | Change |
|---|---|
| `Common/Type` | `Manual` category, predicates, stable name |
| Parser | `memory.manual_allocate<T>` typed-call recognition |
| Sema | manual provenance on `Symbol`, allocation/release checks, cleanup mapping, boundary rejection |
| TIR Builder | manual load/move/AOT ownership preservation; manual release callee |
| TIR Verifier | manual allocation contract, manual release contract, ordinary-release rejection |
| Lowering | manual allocation from default/selected/explicit allocator, typed-root handoff, manual release lowering, collection-free classification |

Manual allocation is not stack-promoted. That keeps the release path explicit and
avoids applying an ordinary stack-deallocation shortcut to manual storage.

### Runtime

No new runtime entry point was needed. Manual allocation uses
`nrq_memory_allocate` / `nrq_memory_allocate_from`; manual release lowers to
`nrq_memory_deallocate_checked`. The runtime record provides allocator identity,
invalid-pointer detection and repeated-release detection.

---

## 9. Debug diagnostics

Static checks:

- release through `memory.deallocate` / `memory.secure_release` /
  `memory.shared_release` / resource close;
- manual release of a non-manual allocation;
- double release;
- use after release;
- release while a borrow is live;
- reallocation;
- arena/pool handle;
- deterministic-cleanup payload;
- function argument/return escape;
- malformed TIR category or release pairing.

Runtime checks:

- invalid or already-freed pointer;
- shared storage through the ordinary path;
- stack allocator LIFO violation;
- allocator operation-in-flight races;
- allocator callback failure.

The production runtime pays no new manual-only per-object cost; manual storage
uses the existing ownership record. No expensive debug-only metadata was added
to ordinary objects.

---

## 10. Tests and exact totals

Commands:

```text
./build/msvc-dev/tests/nirqoba_unit_tests.exe
python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe --configuration RelWithDebInfo
python tests/phase7_isolation.py
python scripts/compare_configuration_equivalence.py --compiler build/msvc-dev/bin/nirqoba.exe --filter '*gc*'
```

Results:

- Unit: **329 passed**, 0 failed. Phase 7 baseline was 326; three TIR manual
  contract tests were added.
- Full runner: **624 passed**, 0 failed, 0 skipped. Phase 7 baseline was 606;
  eighteen focused memory-model fixtures were added.
- O0: `manual_lifecycle`, `manual_managed_payload` and `manual_borrow` build and
  run with the expected output.
- Debug runtime (`--debug-enabled`): `manual_lifecycle` 41/42,
  `manual_managed_payload` 11/42, `manual_borrow` 8,8/42.
- Phase 7 isolation: **passed**; twelve ordinary fixtures at `-O0`/`-O2` and the
  debug runtime contain no `nrq_gc` IR or object symbols; the managed control
  contains 8 `nrq_gc` symbols.
- Production/debug configuration equivalence: **67 agreed, 0 skipped, 0
  failed**.
- Phase 1–7 tests were preserved; the full runner includes the arena, shared and
  GC regression cases.

New fixtures cover manual lifecycle, automatic fallback, move, borrow,
allocator handle, managed payload, conditional release, ordinary/secure release
mismatch, double release, use-after-release, live-borrow release, reallocation,
function argument/return escape, arena/pool rejection, deterministic-cleanup
payload rejection and FFI rejection.

---

## 11. Benchmarks

`benchmarks/memory-model/phase-8/` records seven fresh-process samples per
workload and configuration; production and debug checksums are checked for
equality. Production medians:

| Workload | Median cycles/op | Range | Peak RSS |
|---|---:|---:|---:|
| ordinary allocation/release | 142.231 | 134.896–144.292 | 9,859,072 B |
| manual allocation/release | 144.432 | 136.401–165.505 | 9,863,168 B |
| manual from explicit handle | 340.480 | 322.896–344.830 | 9,871,360 B |
| manual fallback cleanup | 142.417 | 136.729–144.386 | 9,846,784 B |
| manual borrow access | 1.704 | 1.535–1.724 | 9,875,456 B |
| manual managed-root payload | 345.422 | 338.557–349.782 | 15,650,816 B |

Manual/ordinary ratio: **1.0155**. This is not a speedup claim: both paths use
the same allocator and registry. Explicit-handle/ordinary ratio: **2.3939**,
reflecting existing handle-resolution cost. Debug/production overhead on the
managed-root workload: **1.0467×**.

The full sample arrays, standard deviations, process times and checksums are in
`results.json`. No FFI, manual reallocation or arbitrary manual alignment
benchmark exists because those paths are unsupported.

---

## 12. Known limitations and deferred work

- Manual storage is function-local; no manual pointer type exists to carry
  provenance through owned parameters/returns.
- Manual reallocation is unsupported.
- Arbitrary manual alignment selection is unsupported; the payload type's
  natural alignment is used.
- Manual storage in aggregates is rejected by the existing field-ownership rule.
- Source-level custom allocator construction is unavailable; a native embedder
  can still provide a handle.
- No FFI surface, foreign ownership transfer, retained foreign pointer, callback,
  foreign allocator contract, or foreign GC/shared/arena interaction.
- No weak references, new cycle collector or automatic GC/CDMM selection.
- No exception/panic unwinding; `system.exit` does not run lexical cleanup.
- Runtime use-after-free detection is limited to the ownership registry; an
  arbitrary raw alias is not dynamically checked.
- Phase 9 optimization/stabilization was not started.
- The Phase 10 CDMM addition in `MMM_PLAN.md` was preserved unchanged.

---

## 13. Working tree and publication state

The working tree was clean after the Phase 8 commits, with phase-generated object
files removed. No commit was pushed. No release, publication, package or deploy
was created. Phase 9 was not started.
