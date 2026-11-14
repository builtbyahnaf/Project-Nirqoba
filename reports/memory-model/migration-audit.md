# Memory Model Migration — Phase 0 Architecture Audit

**Project:** Nirqoba  
**Baseline:** `0.511-beta`, HEAD `d4292aa`  
**Audit date:** 2026-11-15  
**Scope:** Phase 0 of the dedicated Memory Model Migration Plan (Phases 0–9).  
**Status:** Complete; design freeze is in [`target-architecture.md`](target-architecture.md).

---

## 1. Executive assessment

Nirqoba is **not starting from zero**. Pre-MMM Phases 1–8 already implemented a
large part of the target model:

- deterministic owned pointers and arrays;
- move-only ownership transfer through locals, calls and returns;
- aggregate destruction, partial initialization and early-exit cleanup;
- immutable/mutable borrowing, path-sensitive borrow tracking and borrowed returns;
- explicit reference-counted shared pointers;
- a precise, generational, incremental tracing collector with typed roots,
  write barriers and safepoints;
- allocator strategies and a manual allocation escape hatch;
- Phase 8's storage-boundary policy that rejects ownership payloads a container
  cannot release.

The migration is therefore a **representation, isolation and completion** effort,
not a restart.

Two findings block a clean Phase 1 entry:

1. **Owned-only release still depends on the collector.** Every owned aggregate
   drop and owned-array release emits `nrq_gc_root_unregister`, even when the
   released type contains no managed references. The runtime then takes the GC
   heap lock. This violates the plan's ordinary non-GC requirement and Gate C.
2. **TIR carries ownership metadata but does not verify it.** `ValueOwnership` is
   attached to TIR values and parameters, but `verify_tir_module` checks only
   structure and value types. Destruction is represented as string-named runtime
   calls, not first-class ownership operations.

A third finding is a mixed-strategy correctness risk: **secure allocations have no
typed rooted allocation path**. Managed fields in secure storage are currently
reachable only through incidental local roots, not through a secure-storage root
registration contract.

Everything else is either already implemented and tested, deliberately absent, or
a later-phase capability (arenas with object destruction, weak references, FFI,
threads, async/task).

---

## 2. Baseline and working tree

| Item | Value |
|---|---|
| Working label | `0.511-beta` (`CMakeLists.txt`) |
| HEAD | `d4292aa` |
| Unit suite | 258/258 PASS |
| Integration suite | 489/489 PASS |
| Runtime parity | 104 invariant checks |
| Pre-existing dirty files | 78 under `benchmarks/stress_test_2/*` |
| New untracked files before audit | None |

The dirty benchmark artifacts were not read as architectural evidence, modified,
staged or committed. No beta version bump was made; Phase 0 is audit/design only.

---

## 3. Subsystems and files inspected

### Frontend and semantic analysis

- `compiler/lib/Frontend/Lexer.cpp`, `Parser.cpp`, `Token.cpp`
- `compiler/include/nirqoba/Frontend/Ast.h`
- `compiler/include/nirqoba/Common/Type.h`
- `compiler/lib/Sema/TypeChecker.cpp`
- `compiler/include/nirqoba/Sema/TypeChecker.h`
- `compiler/lib/Sema/SymbolTable.cpp`
- `compiler/include/nirqoba/Sema/SymbolTable.h`

### TIR and lowering

- `compiler/include/nirqoba/TIR/IR.h`
- `compiler/lib/TIR/Builder.cpp`, `IR.cpp`, `Verifier.cpp`, `Printing.cpp`
- `compiler/lib/Lowering/TirToMlir.cpp`
- `compiler/lib/Lowering/DataLayout.cpp`
- `compiler/lib/Lowering/PassPipeline.cpp`
- `compiler/lib/Lowering/NrqToLLVMPass.cpp`
- `compiler/lib/Backend/Optimization.cpp`

### Runtime

- `runtime/include/nirqoba/runtime/gc.h`
- `runtime/include/nirqoba/runtime/allocator.h`
- `runtime/src/gc.cpp`
- `runtime/src/allocator_windows.cpp`
- `runtime/src/io_windows.cpp`
- `runtime/src/runtime_buffers.cpp` / `.h`
- `runtime/src/runtime_shutdown.h`
- `runtime/src/fatal_windows.cpp`
- `runtime/src/file_windows.cpp`, `mapping_windows.cpp`, `sync_windows.cpp`,
  `socket_windows.cpp`, `system_windows.cpp`

### Driver, tests, docs, benchmarks

- `compiler/lib/Driver/CompilationPipeline.cpp`, `TemporaryArtifact.cpp`
- `tests/unit/*.cpp` (23 files, 258 tests)
- `tests/integration/{positive,negative,mlir,cli,pgo}` (489 tests)
- `docs/memory/management.md`, `docs/memory/allocators.md`
- `docs/architecture/01-frontend.md`, `02-typed-ir.md`, `04-lowering.md`,
  `06-runtime.md`, `07-models.md`
- `reports/pre-mmm/phase-04.md`, `phase-05.md`, `phase-06.md`,
  `phase-07*.md`, `phase-08*.md`
- `benchmarks/pre_mmm_baseline`, `pre_mmm_phase4`, `pre_mmm_phase6`,
  `pre_mmm_phase7*`, `pre_mmm_phase8`

### Generated-artifact probes

The audit compiled representative programs with `--emit=llvm`, `--emit=obj` and
`--emit=tir`, then inspected declarations, relocations and executable sizes. This
was necessary: source-level syntax and diagnostics do not prove GC isolation.

---

## 4. Current architecture flow

```text
Source
  -> Lexer / Parser
  -> AST: Parameter.borrowed/mutable_borrow/shared/resource_domain,
          Expr ownership + transfers_ownership
  -> Sema: Symbol ownership, allocation IDs, move flags, borrow paths,
           borrow effects, cleanup callee selection, storage-boundary policy
  -> TIR: ValueOwnership on values/parameters/returns,
           cleanup represented as Call("array.owned_release",
           "aggregate.owned_drop", "memory.deallocate", ...)
  -> MLIR/LLVM: allocation lowering, typed root registration, root store,
                safepoint/barrier emission, drop/retain traversal
  -> Runtime: explicit allocator registry, shared refcounts, tracing collector,
              native-resource registries, process string arena
```

Two consequences follow from this shape:

- Most ownership **enforcement** is in Sema, not in TIR.
- Most ownership **execution** is selected in Sema and emitted as calls in
  lowering. TIR has no first-class move/drop/borrow operations, and its verifier
  cannot reject an invalid ownership transformation.

This is adequate for the current tested surface, but it is the central Phase 1
representation gap.

---

## 5. Six-strategy traceability matrix

Status meanings: **Complete** = implemented and directly evidenced; **Partial** =
some implementation exists but the migration requirement is unmet; **Missing** = not
implemented; **Blocked** = depends on an absent subsystem or unresolved decision;
**Not applicable** = the relevant subsystem does not exist in this tree.

### 5.1 Deterministic ownership

| ID | Requirement | Status | Evidence |
|---|---|---|---|
| D1 | Ownership is the default for ordinary scalar and aggregate values | Complete | `ValueOwnership` in `Type.h`; `TypeChecker` binding rules; `owned_aggregate`, `owned_scope_drop` |
| D2 | Unique owned pointer allocation and deterministic release | Complete | `memory.allocate<T>`; `memory.deallocate`; `owned_function_return`, `owned_function_parameter`, `owned_parameter_drop` |
| D3 | Owned arrays, aligned variants and allocator-aware arrays | Complete | `TypeKind::OwnedArray`; `memory.owned_array[_aligned]`; `owned_array`, `owned_array_aligned`, `allocator_owned_array_arg` |
| D4 | Move through locals, fields, parameters and returns | Complete | `Symbol::moved`, `transfers_ownership`, `consume_field_move`; `owned_aggregate_field_move`, `cleanup_transfer_move`, `owned_aggregate_field_reinitialize` |
| D5 | Exactly-once release and invalid/double-free rejection | Complete | runtime ownership registry in `allocator_windows.cpp`; `deferred_double_free`; unit registry tests |
| D6 | Nested aggregate field destruction | Complete | `drop_aggregate_fields` in `TirToMlir.cpp`; `owned_field_nested_destruction`, `owned_aggregate` |
| D7 | Partial initialization (`uninit`) is safe and destroyed correctly | Complete | `Symbol::uninitialized_fields`; `uninit_nested_cleanup`, `uninit_partial_initialization`, `aggregate_soa_field_uninit` |
| D8 | Cleanup on return, break and continue | Complete | `emit_deferred_from`; `cleanup_early_return_order`, `cleanup_stress_loop_paths` |
| D9 | Explicit `defer` overrides inferred cleanup with correct ordering | Complete | `TirBuilder` deferred scopes; `defer_cleanup`, `defer_owner_supersedes_cleanup`, `conditional_defer_fallback` |
| D10 | Resource-domain ownership (file/socket/mutex/mapping/secure) | Complete | `owned file/socket/mutex/mapping/secure` types; `resource_fields_all`, `owned_resource_factories`, `resource_fields_runtime` |
| D11 | Nested generic ownership types | Complete | parser/type resolution; `nested_generic_managed_payload`, `owned_array_gc_roots` |
| D12 | Ordinary owned-only execution does not depend on GC infrastructure | **Partial** | Owned aggregate/array release still emits `nrq_gc_root_unregister`; see §6 |
| D13 | Deterministic destruction of strings | **Missing** | `string` is process-lifetime borrowed storage from `file.read_text`; `runtime_buffers.cpp`; `file_read_text_lifetime` |
| D14 | Exception and panic cleanup | **Missing** | no exception model; `nrq_runtime_fatal` calls `ExitProcess` without unwinding (`fatal_windows.cpp`) |

### 5.2 Borrowing

| ID | Requirement | Status | Evidence |
|---|---|---|---|
| B1 | Immutable borrow parameters | Complete | `borrow T*`; `Symbol::borrowed`; `borrowed_parameter`, `borrow_field_direct` |
| B2 | Mutable borrow with exclusivity | Complete | `borrow mut T*`; `active_borrow_conflicts`; `mutable_borrow`, `mutable_borrow_overlap` |
| B3 | Source-bound borrowed returns | Complete | `FunctionSignature::return_borrow_parameter`; `borrowed_return`, `borrowed_return_selected_parameter` |
| B4 | Field/path-sensitive borrows | Complete | `BorrowPathSet`, `BorrowTarget`; `aggregate_borrow_same_scope`, `borrow_field_nested` |
| B5 | Branch, loop and conditional borrow state | Complete | branch merge/refinement in `SymbolTable`; `branch_borrow_paths_*`, `flow_borrow_*`, `conditional_borrow_*` |
| B6 | Interprocedural effects and recursive propagation | Complete | `FunctionSignature::parameter_effects`; `interprocedural_*`, `recursive_borrow_effect` |
| B7 | Escape, alias and lifetime rejection | Complete | `borrow_escape`, `borrowed_return_expired_source`, `borrowed_return_local_owner`, `aggregate_borrow_inner_escape` |
| B8 | Array views and subviews | Complete | `array_view`/`array_view_mut`; `owned_array_view`, `borrowed_array_view_subview_return`, `mutable_owned_array_view` |
| B9 | Borrows inside aggregates and containers | **Partial** | aggregate borrow fields work; array views cannot be stored in aggregates (`array_view_aggregate_escape`), and bulk containers reject borrowed fields |
| B10 | Borrow-only execution has no GC behavior | **Partial** | borrow tracking itself emits no GC, but releasing the borrowed owned array emits `nrq_gc_root_unregister` |
| B11 | Borrows across threads or FFI | **Not applicable** | no language thread spawn or FFI calling convention in this tree |

### 5.3 Arena and pool allocation

| ID | Requirement | Status | Evidence |
|---|---|---|---|
| A1 | General, pool, arena, stack, slab, buddy, free-list and TLSF constructors | Complete | `runtime/src/allocator_windows.cpp`; `allocator_strategies` |
| A2 | Thread-local, per-allocation and process-default selection | Complete | `memory.use_allocator`, `memory.allocate<T>(bytes, handle)`, `memory.set_default_allocator`; `allocator_strategies`, `memory_default_allocator` |
| A3 | Arena allocation | Complete | `ArenaAllocator`; `allocator_strategies`, `memory_advanced` |
| A4 | Pool/slab/buddy/free-list/TLSF allocation | Complete | allocator classes; `allocator_strategies` |
| A5 | Reset/destroy only when quiescent | Complete | `nrq_allocator_reset`, `nrq_allocator_destroy`; docs and runtime checks |
| A6 | Stack allocator LIFO enforcement | Complete | `StackAllocator`; `nrq_memory_deallocate_checked` LIFO check |
| A7 | Bulk destruction of live objects on arena reset | **Missing** | reset refuses while allocations remain live; it frees storage, not object fields |
| A8 | Language-level arena lifetime/escape validation | **Missing** | allocator handles are runtime values; no scoped arena type or borrow/escape checking |
| A9 | Arena/pool paths independent of GC | **Partial** | allocation itself takes no GC path; owned-array release still calls GC root unregister |

### 5.4 Explicit shared ownership

| ID | Requirement | Status | Evidence |
|---|---|---|---|
| S1 | Explicit shared allocate/retain/release | Complete | `memory.shared_allocate`, `shared_retain`, `shared_release`; `shared_memory` |
| S2 | Count overflow and underflow protection | Complete | `UINT64_MAX` reject and zero/underflow handling in `allocator_windows.cpp`; unit allocator tests |
| S3 | Shared fields in aggregates retain/release | Complete | `aggregate.shared_retain`, `drop_aggregate_fields`; `shared_field_construction`, `shared_field_copy_scope` |
| S4 | Managed references inside shared storage stay rooted | Complete | `nrq_memory_shared_allocate_rooted`; `shared_pointer_gc_roots`, `shared_field_gc_roots` |
| S5 | Final release happens exactly once | Complete | shared-reference reservation and owner record; `shared_memory`, unit shared tests |
| S6 | Reference-count updates are thread-safe | Complete | global allocator mutex around count operations; cross-thread unit coverage in allocator tests (data access itself is not synchronized) |
| S7 | Weak references | **Missing** | no `weak` type, API or runtime support |
| S8 | Cross-thread shared ownership in the language | **Blocked** | runtime refcount is synchronized, but no language thread/spawn subsystem exists |
| S9 | Shared values in bulk containers | **Missing** | Phase 8 deliberately rejects shared payloads in owned arrays, SoA, collector arrays and explicit allocations |
| S10 | Shared ownership across FFI | **Blocked** | no FFI subsystem |

### 5.5 Selective garbage collection

| ID | Requirement | Status | Evidence |
|---|---|---|---|
| G1 | Precise, non-moving tracing | Complete | `runtime/include/nirqoba/runtime/gc.h`; `gc.cpp`; descriptor tables and typed roots |
| G2 | Generational and incremental collection | Complete | young/old generations, incremental marking, sliced sweep in `gc.cpp` |
| G3 | Typed root registration and frame descriptors | Complete | `nrq_gc_root_register_typed`, `nrq_gc_root_register_frame_static`; `gc_root_completeness`, `gc_root_audit_*` |
| G4 | Root-store and write barriers | Complete | `nrq_gc_root_store`, `nrq_gc_root_copy`; `gc_handoff_order`, `gc_root_audit_values` |
| G5 | Safepoints at loops and collection points | Complete | `nrq_gc_safepoint`; `gc_safepoint_bound`, `gc_safepoint_throttle` |
| G6 | Automatic collection starts disabled | Complete | `automatic_collection_enabled{false}` in `gc.cpp`; `gc_root_elision_owned` |
| G7 | Explicit `gc.collect()` | Complete | `nrq_gc_collect`; `gc_api`, `gc_allocation`, `gc_managed_graph` |
| G8 | Explicit collector allocation | Complete | `array.allocate<T>`; `gc_managed_array`, `gc_allocation` |
| G9 | Cyclic graphs | Complete | `gc_managed_graph`, `examples/memory/cyclic_graph.nrqba` |
| G10 | Managed SoA columns | Complete | `nrq_gc_allocate_soa_rooted`, strided descriptors; `soa_reference_*` |
| G11 | Mixed storage: managed fields in explicit/owned/shared storage | **Partial** | explicit/owned/shared paths register typed roots; secure allocation has no rooted path (§7) |
| G12 | Owned-only programs excluded from GC instrumentation | **Partial** | no mutator/root frame/safepoint for a pure owned program, but owned release emits `nrq_gc_root_unregister` |
| G13 | GC metadata and synchronization only for GC values | **Partial** | GC headers are not used for explicit storage; the unconditional unregister call still reaches GC metadata/lock |
| G14 | Stats and controls | Complete | `gc.get_statistics`, threshold, enable/disable, generational controls; `gc_api` |

### 5.6 Manual memory management

| ID | Requirement | Status | Evidence |
|---|---|---|---|
| M1 | Typed explicit allocation/deallocation | Complete | `memory.allocate<T>` and `memory.deallocate`; `memory_allocation`, `memory_typed` |
| M2 | Reallocation preserving allocator/alignment | Complete | `memory.reallocate<T>`; `memory_reallocate`, unit registry tests |
| M3 | Secure allocation/release | **Partial** | `memory.secure_allocate`/`secure_release` work for local payloads; no rooted secure allocation path for managed fields |
| M4 | Memory mapping with explicit unmap | Complete | `memory.map_file`/`unmap_file`; `memory_advanced`, unit mapping tests |
| M5 | Raw pointer and address-of escape hatch | **Partial** | `&` and typed raw pointers exist; raw pointer returns are rejected (`raw_pointer_return`), and raw pointer fields require an ownership qualifier |
| M6 | `unsafe`/manual lifetime scopes | **Missing** | no unsafe scope, raw-pointer field escape or manual lifetime syntax |
| M7 | Custom allocator registration through the C ABI | Complete | `NrqCustomAllocatorVTable`; unit allocator tests |
| M8 | External/FFI function calls | **Missing** | no source-level `extern` or FFI declaration/ABI in the parser or Sema |
| M9 | Foreign ownership transfer and borrowed foreign pointers | **Missing** | no FFI ownership contract; depends on M8 |

### 5.7 Cross-cutting architecture

| ID | Requirement | Status | Evidence |
|---|---|---|---|
| X1 | Ownership categories in the semantic model | Complete | `ValueOwnership`, `Symbol`, `FunctionSignature` |
| X2 | Move, borrow and cleanup validation in Sema | Complete | `TypeChecker` move/borrow/cleanup logic; broad positive/negative suites |
| X3 | Ownership transfer represented explicitly through TIR | **Partial** | `ValueOwnership` is stored, but transfers are implicit in builder control flow; no move operation |
| X4 | TIR verifier rejects invalid ownership transformations | **Missing** | `compiler/lib/TIR/Verifier.cpp` checks structure/types only; ownership is not inspected |
| X5 | Lowering preserves ownership semantics without ad hoc string calls | **Partial** | cleanup uses `Call("array.owned_release")`, `Call("aggregate.owned_drop")`, etc.; no typed drop/move operation |
| X6 | Optimization preserves roots, barriers and cleanup | Complete | `owned_pointer_root_elision*`, `gc_root_at_O0`, `opt_vectorize_managed_blocked` |
| X7 | Root liveness and unobservable-root elision | Complete | `PassPipeline` root plan; `gc_root_elision_owned`, `owned_pointer_root_elision_ir` |
| X8 | Exception/panic cleanup | **Missing** | no exception model; fatal paths call `ExitProcess` |
| X9 | Async/await runtime | **Not applicable** | tokens are reserved for Phase 10; parser rejects them |
| X10 | Thread spawn/scheduling | **Not applicable** | no language thread subsystem; mutex exists as a native handle only |
| X11 | FFI boundary | **Missing** | no FFI implementation |
| X12 | Sanitizer coverage | **Missing** | MSVC configuration has no ASan/UBSan run |
| X13 | Architecture documentation matches implementation | **Partial** | `docs/architecture/02-typed-ir.md`, `06-runtime.md`, `07-models.md` still describe Phase 1/Phase 6-era behavior |
| X14 | Test coverage of the non-GC execution claim | **Partial** | 258/489 pass, but `gc_root_elision_owned` asserts no registration/mutator/safepoint and does not assert no `nrq_gc_root_unregister` |

### 5.8 Completion counts

| Status | Count |
|---|---:|
| Complete | 50 |
| Partial | 13 |
| Missing | 13 |
| Blocked | 2 |
| Not applicable | 3 |
| **Total** | **81** |

The counts are a classification of this matrix, not a percentage of the migration.
Several "Missing" rows are absent subsystems (FFI, threads, async, exception model),
not defects in the current supported surface.

---

## 6. GC-dependency analysis of owned/borrowed execution

### 6.1 Method

The audit compiled representative programs and inspected generated LLVM IR,
relocations and executable size:

| Probe | GC declarations/calls in LLVM |
|---|---|
| scalar arithmetic | none |
| `memory.shared_allocate<int>` / retain / release | none |
| `memory.allocate<int>` inside a struct | `nrq_gc_root_unregister` |
| `memory.owned_array<int>` | `nrq_gc_root_unregister` |
| borrowed `array_view<int>` over an owned array | `nrq_gc_root_unregister` |
| owned aggregate with `owned_array<int>` fields | `nrq_gc_root_unregister` |
| shared/owned storage containing `managed<T>` | typed root registration (expected mixed case) |

The LLVM module for the owned aggregate program contains:

```text
declare void @nrq_gc_root_unregister(ptr)
...
call void @nrq_gc_root_unregister(ptr %1)
call void @nrq_memory_deallocate_checked(ptr %1)
```

The object relocation is direct:

```text
callq ...
  IMAGE_REL_AMD64_REL32 nrq_gc_root_unregister
```

and the linked executable grows from **20,480 bytes** for a scalar-only program to
**64,512 bytes** for the owned-aggregate program. The difference is not attributed
solely to this call, but the undefined GC symbol is a real link-time dependency.

### 6.2 Root cause

`TirToMlir` emits `nrq_gc_root_unregister` unconditionally in three deterministic
release paths:

- `drop_aggregate_fields` for every `owned ptr` and `owned_array` leaf;
- `array.owned_release` for the container data block;
- `soa.owned_release` (SoA is collector-owned, so this path is legitimately GC).

The first two paths run for element/pointee types that contain **no managed
references at all**. There is no root to remove.

`nrq_gc_root_unregister` in `runtime/src/gc.cpp` handles an unknown slot as a no-op
for compatibility, but it still takes the collector's heap lock when the slot is not
in the current thread's shadow stack. Thus the cost is not only a call: an
owned-only release enters GC synchronization.

The existing regression `gc_root_elision_owned` asserts that no
`nrq_gc_root_register_frame_static`, `nrq_gc_root_register_typed`,
`nrq_gc_mutator_enter` or `nrq_gc_safepoint` appears. It does not assert that
`nrq_gc_root_unregister` is absent, so the suite passes while Gate C is violated.

### 6.3 Requirements affected

This finding directly fails:

- MMM §10.1 "owned-only hot path must not update GC root tables, acquire
  GC-specific locks, or invoke unnecessary GC operations";
- Phase 4 tasks 6–9 (remove root tracking/safepoints/mutator/barriers from
  owned-only code);
- Phase 7 task 9 and Gate C.

It does **not** show a collector correctness bug: the unregister is a no-op when no
root exists. It is an architectural isolation defect and a runtime cost.

### 6.4 Other non-GC observations

- `nrq_runtime_init` is empty; an owned-only program does not initialize the
  collector (`io_windows.cpp`).
- A pure scalar program emits no GC declarations at all.
- Shared scalar allocation emits no GC calls.
- Owned-only code does not open a mutator region, register a root frame or emit a
  safepoint.
- `nrq_runtime_shutdown` releases native-resource registries and does not collect.
- The process string arena is intentionally never released before process exit.

---

## 7. Critical findings beyond GC isolation

### 7.1 TIR ownership is not verified

`compiler/include/nirqoba/TIR/IR.h` defines `ValueOwnership`, stores it on
`tir::Value`, parameters and function returns, and `TirBuilder` copies it from the
AST. But `compiler/lib/TIR/Verifier.cpp` only verifies:

- operation presence and terminators;
- value IDs;
- load/store type equality;
- return type equality.

It never checks ownership transfer, moved values, borrow validity, cleanup
obligations or release domains. A malformed TIR module with two drops of one value,
or a borrowed value passed to an owning operation, can pass verification. The
current safety comes from Sema, not from the IR contract.

Phase 1 must make ownership a first-class, verified part of TIR. Without that, a
new lowering pass can silently violate ownership while every existing test passes.

### 7.2 Destruction is encoded as string-named calls

Cleanup appears as generic `Call` operations with names such as:

- `array.owned_release`
- `aggregate.owned_drop`
- `aggregate.shared_retain`
- `memory.deallocate`
- `memory.shared_release`
- `memory.secure_release`
- `file.close` / `socket.close` / `mutex.destroy` / `memory.unmap_file`

This is workable for the current backend, but it means an ownership operation has
no typed result, no release obligation and no verifier rule. A pass cannot ask "is
this value still owned?" or "has this obligation been discharged?" without
reconstructing Sema knowledge.

The installed cleanup for a binding is selected by the file-local helpers
`cleanup_for_type` / `cleanup_for_owner_producer` in `TypeChecker.cpp`; the TIR
builder stores only the callee name and an owner slot. This is the representation Phase 1
must replace or formalize.

### 7.3 Secure storage has no typed root path

`memory.allocate<T>` lowers a managed payload by registering the allocation result
as a typed root (`emit_gc_root_registration` in `TirToMlir.cpp`). `memory.reallocate`
unregisters and re-registers around the move. Shared storage has
`nrq_memory_shared_allocate_rooted` and final-release unregistration.

`memory.secure_allocate` has no equivalent:

- `runtime/include/nirqoba/runtime/allocator.h` exposes no
  `nrq_memory_secure_allocate_rooted`;
- `TirToMlir` lowers secure allocation with `nrq_memory_secure_allocate` only;
- `memory.secure_release` frees the block and does not walk managed fields.

The Phase 8 positive test `secure_allocation_managed_payload` passes because the
managed `Node` allocation result stays in a local root slot for the duration of the
activation. The secure payload itself is not registered. That is not a general
rooting contract, and a future return/escape path could expose it.

**Frozen decision:** either add a rooted secure allocation path with an unregister
on final release, or reject managed fields in secure allocations until that path
exists. Phase 0 recommends the second option for Phase 1 (smaller and safe), with
rooted secure storage deferred to Phase 7.

### 7.4 Arena semantics are allocator-only

The runtime provides arena, pool, stack, slab, buddy, free-list and TLSF
allocators. Selection is explicit and reset/destroy refuse while allocations are
live. This is a useful bulk allocator, but it is not yet a language-level arena
lifetime:

- no scoped arena type;
- no compiler validation that a reference does not escape the arena;
- no bulk **object** destruction; reset frees storage, not fields;
- no arena-borrow relationship in Sema.

MMM Phase 5 requires all of these. The current implementation is a sound allocator,
not the target arena semantics.

### 7.5 No exception/panic cleanup

`nrq_runtime_fatal` writes a message and calls `ExitProcess(70)`. There is no
unwind, no destructor walk and no language exception model. Any owned resource live
at a fatal boundary is reclaimed by process termination, not by its declared
cleanup. Gate A's exception/panic item cannot be claimed until an error model is
designed.

This is a language-level blocker, not a bug in the current normal-exit cleanup.

### 7.6 Strings remain an explicit deterministic exception

`file.read_text` returns a borrowed `string` backed by a process-lifetime arena.
The runtime intentionally does not release the arena at shutdown because copied,
returned, aggregate-stored and shared aliases have no release point. The contract
is documented in `runtime_buffers.h` and tested by `file_read_text_lifetime`.

The MMM target says ordinary values have an identifiable owner. Strings currently
do not. Migration must either introduce an owned string or freeze the borrowed
process-lifetime contract as an explicit exception. It must not silently claim
deterministic string destruction.

### 7.7 Bulk containers reject useful payloads

Phase 8 correctly rejects shared/owned/borrowed fields in containers whose release
path cannot walk them. That is a safe policy, but it leaves no container for
shared-owning or owning-field aggregates. MMM Phase 6 and standard-library work
will need one of:

- per-element finalizers for the relevant container;
- a container of references/indices into an owner arena;
- a documented restriction that such aggregates live only in lexical values.

This is a feature gap, not a silent fallback.

### 7.8 Architecture docs are historical

`docs/architecture/02-typed-ir.md` still lists the Phase 1 operation set, and
`docs/architecture/07-models.md` says deterministic ownership is planned for
Phase 9. `06-runtime.md` still lists `nrq_runtime_init` as a GC initialization
boundary and a `0.266-beta` version. These do not affect code, but they are not a
reliable migration map. The frozen target in
[`target-architecture.md`](target-architecture.md) is authoritative for Phase 1;
the historical architecture pages should be updated during Phase 9 documentation
stabilization, not silently treated as current.

### 7.9 Missing subsystems

No language-level FFI, thread spawn, async/await runtime or tensor/device system
exists. These are out of the current compiler surface:

- `async`/`await` are lexed and rejected as Phase 10;
- there is no `extern`/FFI declaration or call ABI;
- mutexes exist as native resources but there is no language thread primitive;
- no tensor/device types exist.

They are **not Phase 1 blockers**, but they block the corresponding later MMM
phases. The audit does not invent designs for them.

---

## 8. Dependency graph and migration risks

```text
Phase 0 (this audit)
  |
  +--> GC-isolation fix / regression gate  [Phase 4 prerequisite]
  |       |
  |       +--> no unconditional root unregister in owned release
  |       +--> IR assertion: owned-only has no nrq_gc_* at O0/O2
  |
  +--> Phase 1: TIR ownership/destruction representation + verifier
  |       |
  |       +--> typed move/drop/borrow metadata or operations
  |       +--> release-obligation checks
  |       +--> cleanup domain checks
  |
  +--> Phase 2: destruction/move validation on the existing implementation
  |       |
  |       +--> exactly-once, partial init, early exits (already mostly done)
  |       +--> exception/panic policy decision
  |
  +--> Phase 3: borrow/lifetime representation in TIR
  |       |
  |       +--> port Sema borrow paths/effects, do not reimplement semantics
  |
  +--> Phase 4: ordinary non-GC allocation isolation (remove remaining GC deps)
  |
  +--> Phase 5: arena scopes, bulk object destruction, escape validation
  |
  +--> Phase 6: shared completion + weak refs if justified
  |
  +--> Phase 7: GC isolation/mixed-strategy completion (secure rooting, SoA)
  |
  +--> Phase 8: manual/FFI boundary
  |
  +--> Phase 9: optimization, documentation and stabilization
```

### Risks, ranked by severity

| Severity | Risk | Impact | Mitigation |
|---|---|---|---|
| **Critical** | Ownership stays Sema-only | A new pass can violate moves/drops while TIR verifies | Make TIR ownership first-class before new lowering work |
| **Critical** | GC unregister remains in owned release | Gate C fails; hidden lock in ordinary destruction | Gate emission on managed-reference presence; add IR regression first |
| **Critical** | Secure managed payloads remain unrooted | Use-after-free in a future escape path | Reject managed fields in secure storage until rooted path exists |
| **High** | Bulk containers need shared/owned fields | Standard-library pressure leads to unsafe exceptions | Keep reject-by-default; add a deliberate container design later |
| **High** | Arenas treated as complete | Lifetime escapes and missing destructors | Implement scoped arena semantics before exposing as default |
| **High** | String exception forgotten | Users assume deterministic string cleanup | Document as explicit exception; design owned string later |
| **High** | Exception model added late | Cleanup guarantee retrofitted | Decide policy before claiming Gate A |
| **Medium** | FFI/threads/async designed ad hoc | Ownership rules become inconsistent | Freeze boundary rules before implementing those phases |
| **Medium** | Docs drift | Migration decisions based on stale Phase 1 docs | Target architecture is authoritative; refresh docs in Phase 9 |

---

## 9. What Pre-MMM already accomplished

This audit did not find a reason to redo the following:

- **Phase 7:** compiler artifact leases and transactional publication; process string
  arena; file/mapping/mutex shutdown registries; container/aggregate root and drop
  paths. These are native C++ ownership mechanisms, not language-level migration, but
  they remove real resource leaks.
- **Phase 8:** storage-boundary policy. Collector arrays, explicit/reallocated/secure
  storage, shared allocations, owned arrays and SoA columns reject payloads whose
  release paths they cannot run. Legacy untyped memory/GC builtins are gone.
- **Earlier work:** move-only owned pointers/arrays, aggregate drop, partial init,
  early-exit cleanup, path-sensitive borrows, shared refcounts, typed GC roots, root
  liveness, safepoints and barriers.

The compiler already distinguishes owned and borrowed values internally. The gap is
not the semantic model; it is the **IR contract, GC isolation and completion of
later strategies**.

---

## 10. Evidence-backed recommended implementation order

1. **Freeze Phase 0 decisions.** Accept the invariants in
   `target-architecture.md`. No new syntax.
2. **Add the owned-only IR gate and fix the unregister leak.** This is small,
   measurable and prevents Phase 1 from encoding a false isolation contract.
   Gate `nrq_gc_root_unregister` on `collect_managed_reference_offsets` being
   non-empty for the released type, including aggregate leaves and stack-owned
   arrays. Add IR assertions at `-O0` and `-O2` for owned pointer, array, aggregate
   and borrow/release programs.
3. **Resolve secure managed payloads.** For Phase 1, reject managed fields in
   `memory.secure_allocate<T>` unless a rooted secure path is implemented and
   tested. Update the Phase 8 positive test or add a new negative test accordingly.
4. **Phase 1 — ownership representation.** Introduce typed ownership transfer and
   destruction obligations in TIR, preserve them through every pass, and extend the
   verifier. Port the existing Sema rules; do not invent new user-visible semantics.
5. **Phase 2 — destruction and moves.** Keep the existing behavior; add verifier
   coverage for exactly-once, partial init, field moves and early exits. Decide the
   exception/panic policy before claiming complete cleanup.
6. **Phase 3 — borrow/lifetime TIR.** Port `Symbol`/`BorrowPathSet`/effect summaries
   into verified TIR metadata. Keep the existing diagnostics and tests.
7. **Phase 4 — ordinary non-GC allocation.** Remove the remaining GC dependencies
   from owned-only code; verify no GC init, roots, frames, safepoints, barriers,
   allocation, headers or locks.
8. **Phase 5 — arenas.** Add scoped lifetime and bulk object destruction semantics;
   keep the existing allocator strategies.
9. **Phase 6 — shared completion.** Add weak references only if a concrete standard
   library need justifies them; design container integration deliberately.
10. **Phase 7 — GC isolation and mixed strategy.** Add rooted secure storage if
    retained; keep SoA's asynchronous reclamation explicit; verify all interop.
11. **Phase 8 — manual/FFI.** Define foreign ownership and borrowed-pointer rules
    before adding FFI.
12. **Phase 9 — optimization/docs/stabilization.** Refresh architecture docs,
    optimize only after semantics are verified, and measure.

This order deliberately fixes the cheapest architectural lie first: an owned-only
program that calls into the collector is not ready to be the basis of a larger
ownership IR.

---

## 11. Phase 1 entry criteria

Phase 1 implementation should not begin until all of the following are true:

1. **Design freeze accepted.** The invariants in `target-architecture.md` are the
   agreed contract.
2. **Owned-only GC gate exists.** A regression asserts no `nrq_gc_*` call in a
   representative owned/borrowed program at `-O0` and `-O2`, including aggregate
   drop and owned-array release. The unconditional unregister is fixed or the gate
   records it as a known failing prerequisite with an owner.
3. **Secure managed payload decision recorded.** Either:
   - `memory.secure_allocate<T>` rejects any payload containing a managed reference;
     or
   - `nrq_memory_secure_allocate_rooted` and matching release unregistration are
     implemented and tested.
4. **TIR ownership contract specified.** Ownership transfer, destruction
   obligations, borrow metadata and verifier rules are written down before code.
5. **Exception/panic policy frozen.** The project states that fatal exits do not run
   destructors until an error model exists; no phase claims otherwise.
6. **Compatibility baseline frozen.** Existing valid `0.511-beta` programs and the
   258/489 suites remain the regression baseline. No new syntax is introduced in
   Phase 1.
7. **No silent fallback rule restated.** Unsupported ownership remains a
   compile-time error; it is never routed to GC or raw free.
8. **Docs authority established.** New migration docs override historical
   architecture pages until Phase 9 refreshes them.

---

## 12. Verification and audit limits

Performed for this audit:

- Read the source paths listed in §3.
- Ran the full unit and integration suites at `0.511-beta`: 258/258 and 489/489.
- Compiled representative programs with `--emit=llvm`, `--emit=obj` and `--emit=tir`.
- Inspected LLVM declarations/calls, object relocations and executable sizes.
- Compared the generated behavior of scalar, owned pointer, owned array, borrowed
  view, shared scalar, owned aggregate and secure-managed programs.
- Checked the existing phase reports and benchmark claims against source.

Not verified:

- No ASan/UBSan run: the MSVC toolchain in this environment does not provide it.
- No language-level thread, FFI or async test: those subsystems do not exist.
- No exhaustive formal proof of the collector; the audit reviewed the root,
  barrier, safepoint and collection contracts and the existing stress suites, but
  did not model every interleaving.
- Secure managed rooting is evidenced by generated IR and a local test, not by a
  return/escape test, because typed secure values cannot currently escape their
  activation through the supported type surface.

The audit is sufficient to freeze Phase 0. It is not a claim that the migration is
complete.

---

## 13. Phase 9 hardening resolution (post-audit)

Phase 9 at `0.515-beta` resolves the three critical findings and leaves the
design decisions of this audit unchanged.

| Finding | Resolution | Evidence |
|---|---|---|
| Owned-only release calls the collector | Lowering unregisters only when the released payload contains a managed reference. | Owned aggregate, owned array, owned pointer, borrowed view and stack array fixtures emit zero unregisters at `-O0` and `-O2`; managed arrays still emit one. `llvm-nm -u` confirms the owned aggregate object no longer references `nrq_gc_root_unregister`. |
| Secure managed payloads are unrooted | `memory.secure_allocate<T>` rejects managed fields at any depth until a rooted secure allocation path exists. | Direct, aggregate and nested negative fixtures report `NRQ3015`; the plain secure path remains supported and GC-free. |
| TIR ownership is unverified | `verify_tir_module` enforces type/ownership compatibility, return source and ownership, release families, single consumption, parameter contracts and result-record consistency. | 269 unit tests, including malformed modules for each invariant, and 500 integration tests pass. |

The remaining Phase 1 prerequisites are unchanged: first-class move/drop operations,
path-sensitive destruction proofs, arena object semantics, secure managed rooting if
retained, exception cleanup, and the documented string/weak/FFI/thread gaps. Phase 9
removes the three blockers listed in §10; it does not start Phase 1.
