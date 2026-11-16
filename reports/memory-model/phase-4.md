# Phase 4 — Ordinary Non-GC Allocation

**Starting version:** `0.547-beta`, commit `f6c406d`
**Final version:** `0.551-beta`
**Scope:** MMM Phase 4 only. GC was not removed or disabled.

---

## 1. Outcome

Ordinary owned allocation is separated from the collector and the separation is
now checked, not merely incidental:

- `memory.allocate`, `memory.reallocate`, `memory.owned_array`,
  `memory.owned_array_aligned` and `memory.secure_allocate` use the ordinary,
  explicit or secure allocator.
- Their release paths are `memory.deallocate_checked`, `array.owned_release`,
  secure release, or the aggregate field walk. No collector call occurs for a
  payload without a managed reference.
- Owned-only programs emit no `nrq_gc` reference at `-O0` or `-O2`.
- `array.allocate`, `memory.owned_soa`, a managed-reference payload and explicit
  collector operations remain fully GC-backed.
- Stack and inline promotion are preserved.
- The TIR verifier now rejects an allocation whose ownership category or storage
  shape disagrees with its strategy, a collector value passed to an ordinary
  release, and a `tir.drop` of a collector-managed value.

No second allocator was introduced. The existing general allocator, explicit
allocator selection, secure allocator, shared control block and frame promotion
were reused as required.

---

## 2. Mandatory audit

`reports/memory-model/phase-4-audit.md` classifies:

| Category | Path | GC dependency |
|---|---|---|
| Scalar/local | frame or SSA | none |
| `memory.allocate<T>` | general allocator | none for plain payload |
| `memory.owned_array<T>` | general/explicit allocator | conditional on managed element |
| Owned aggregate | by-value storage, recursive drop | only managed leaves |
| Stack/inline array | frame storage, escape analysis | none for plain element |
| Borrowed pointer/view | address/descriptor only | none |
| `array.allocate<T>` | collector | required |
| `memory.owned_soa<T>` | collector-owned SoA | required |
| Managed payload in ordinary/shared storage | ordinary storage plus typed roots | required |
| `memory.shared_allocate<T>` | general allocator plus refcount | none for plain payload |
| `memory.secure_allocate<T>` | secure allocation | none; managed payload rejected |
| `memory.reallocate<T>` | general allocator | conditional on managed payload |
| Raw/manual pointer | caller-selected allocation | none |
| Native resources | domain handles | none |
| `file.read_text` string | process-lifetime arena | none (documented exception) |

The audit traced Sema classification (`expression_creates_owned_pointer`,
`cleanup_for_owner_producer`), TIR construction, `TirToMlir` allocation and
release selection, runtime `nrq_memory_*` functions, collector entry points and
startup/shutdown. It found that the unconditional owned-release unregister had
already been removed during the pre-MMM Phase 9 hardening. Phase 4 therefore did
not need to invent a new allocation runtime; it needed to audit, prove and lock
the existing separation.

---

## 3. Allocation path

### 3.1 Ordinary pointer

`memory.allocate<T>(bytes[, allocator])`:

1. Sema returns a plain pointer and marks the call as an owned producer.
2. TIR records `ValueOwnership::Owned` and a destruction obligation when the
   result is bound.
3. MLIR emits `nrq_memory_allocate` or `nrq_memory_allocate_from`.
4. A bounded, single-store, non-escaping allocation is replaced with frame
   storage. Address-taken, escaping and returned allocations remain on the heap.
5. Release is `nrq_memory_deallocate_checked` through the ownership registry.

No GC header is placed in the payload. The registry records the allocator,
byte count and alignment needed for checked release.

### 3.2 Owned array

`memory.owned_array<T>(count)` and `_aligned` allocate one contiguous block from
the selected allocator. A fixed, non-escaping array can be promoted to the frame.
Release extracts the data pointer from the descriptor and deallocates it once.
An element containing a managed reference is the mixed case: the block remains
ordinary storage, but its managed columns are registered as a typed root and
unregistered before deallocation.

### 3.3 Aggregates

`aggregate.owned_drop` walks fields in reverse declaration order. Plain owned
fields go directly to deallocation; shared fields release their control block;
managed leaves unregister their root. There is no unconditional GC call in the
walk.

### 3.4 Reallocation and secure storage

`memory.reallocate<T>` preserves the existing allocator and alignment, releases
the old address and returns the replacement. It rejects payloads needing field
destruction. Managed payloads unregister the old address and register the new one;
plain payloads do neither.

`memory.secure_allocate<T>` uses the secure runtime and `memory.secure_release`.
It rejects managed and destructible payloads because secure storage has no rooted
or nested-finalizer contract.

### 3.5 Shared allocation

A plain `memory.shared_allocate<T>` uses the general allocator plus the runtime
reference count. It does not create a collector object. A managed payload selects
`nrq_memory_shared_allocate_rooted`, and final release unregisters the root before
freeing the control block.

---

## 4. Verifier contract

`compiler/lib/TIR/Verifier.cpp` now names each allocation strategy:

- `memory.allocate`, `memory.reallocate`, `memory.secure_allocate` →
  `owned`, plain pointer;
- `memory.owned_array`, `memory.owned_array_aligned` → `owned`, owned array;
- `array.allocate` → `gc_managed`, managed pointer;
- `memory.shared_allocate`, `memory.shared_retain` → `shared`, pointer.

It also rejects:

- a collector-managed value passed to `memory.deallocate`,
  `memory.secure_release` or `array.owned_release`;
- an owned-array release of anything other than `owned`/`trivial`;
- a `tir.drop` whose operand is not a genuinely owning category.

This changes no generated machine code. It prevents a future pass from selecting
one allocation path while recording ownership for another.

---

## 5. GC dependencies left in place

These are deliberate, not Phase 4 failures:

- `array.allocate<T>`: collector allocation, typed roots, root frame, mutator,
  safepoints and collection.
- `memory.owned_soa<T>`: collector-owned strided storage; release is root
  unregistration and sweep.
- Managed references inside ordinary, reallocated or shared storage: typed root
  registration and conditional unregistration.
- `gc.collect`, statistics, pin/unpin, roots and barriers: available for explicit
  GC use.

`nrq_runtime_init` remains empty. `nrq_runtime_shutdown` releases native
resources and process strings but does not collect. Ordinary startup does not
register a mutator.

---

## 6. Evidence

Generated `llvm-raw` modules were inspected at `-O0` and `-O2`. Zero occurrences
of `nrq_gc` were found for:

| Fixture | Ordinary release |
|---|---|
| owned heap pointer returned from a function | one checked deallocation |
| owned array returned from a function | one checked deallocation |
| borrowed heap view | one checked deallocation of its owner |
| ordinary reallocation | one checked deallocation after growth |
| secure allocation | one `nrq_memory_secure_release` |
| plain shared allocation | shared refcount runtime, no GC |
| owned aggregate | two checked deallocations for its array fields |
| stack-owned array | none: frame storage |
| owned array view over stack storage | none |

The new fixtures assert `"nrq_gc": 0`, which counts declarations as well as
calls, rather than only the specific root functions named by older tests. Existing
owned pointer, array, aggregate, stack-array, view and root-elision expectations
were strengthened the same way. Mixed fixtures such as `owned_pointer_gc_roots`
and `owned_array_gc_roots` retain their expected root registration.

### 6.1 No runtime cost added

Phase 4 adds verifier checks only. The ordinary allocation and release calls are
the same runtime entry points used before this phase. No allocation header,
reference count, root, safepoint, barrier or synchronization was added to the
ordinary path. No benchmark was run because no runtime path changed.

---

## 7. Tests

- Unit: **313 passed**.
- Full runner: **554 passed**, 0 failed, 0 skipped.
- New unit cases:
  - ordinary allocation marked `gc_managed`;
  - collector allocation marked `owned`;
  - GC value through `memory.deallocate`;
  - drop of a GC-managed value;
  - shared allocation marked `owned`.
- New integration cases: six `-O0`/`-O2` pairs for heap pointers, returned arrays,
  borrowed views, reallocation, secure allocation and plain shared allocation.
- Existing GC, allocator, secure, shared-rooted, ownership, destruction and
  borrow suites pass unchanged.

---

## 8. Corrections and compatibility

- The Phase 3 migration note claimed ordinary allocation remained GC-backed. That
  was too broad; the general allocator was already used, and the remaining
  unregister leak had been removed in pre-MMM Phase 9. Both the Phase 3 note and
  the Phase 4 audit record the corrected history.
- No valid program changed meaning. The only new rejections are malformed TIR
  allocation contracts and TIR drops of collector values.
- `array.allocate`-based programs, SoA, managed-field programs and cyclic graphs
  continue through the same collector.

---

## 9. Limitations

- The allocation-strategy contract is a TIR verification rule, not a new runtime
  type tag. Hand-written low-level runtime code can still call the C ABI directly.
- Secure storage rejects managed payloads until a rooted secure release exists.
- SoA remains a collector-backed container.
- Strings from `file.read_text` remain process-lifetime borrowed storage.
- Arena lifetime semantics, weak references, FFI, exception unwinding and
  cross-thread language ownership remain later work.
- No benchmark or memory measurement was run; this phase makes no performance
  claim.

**Phase 5 and later phases were not started.**

---

## 10. Phase 4 commits

```
15ec8d5 docs: classify every allocation path for Phase 4
f24fee6 tir: verify the selected allocation strategy
fe9651a tests: reject mismatched allocation contracts
e3d6812 tests: prove heap pointer allocation is GC-free at O0
691e55d tests: prove heap pointer allocation is GC-free at O2
d0c3a2e tests: prove returned owned arrays are GC-free at O0
036fa29 tests: prove returned owned arrays are GC-free at O2
dd17b69 tests: prove borrowed heap views are GC-free at O0
3ff6e88 tests: prove borrowed heap views are GC-free at O2
cdadf0b tests: prove ordinary reallocation is GC-free at O0
302d3d0 tests: prove ordinary reallocation is GC-free at O2
78bcd96 tests: prove secure allocation is GC-free at O0
6a21453 tests: prove secure allocation is GC-free at O2
9cce1a2 tests: prove plain shared allocation is GC-free at O0
37ffad6 tests: prove plain shared allocation is GC-free at O2
4b505f8 tests: reject a shared allocation marked ordinary
38bd15f tests: reject every GC reference in owned-only IR
06ef3fa docs: describe allocation strategy in TIR
4287096 docs: document the non-GC ordinary allocation path
bb3f3a2 docs: mark the owned-only GC gate complete
f9b0c92 docs: add the Phase 4 migration note
3aa7ca2 docs: correct the Phase 3 allocation claim
5fdd31d docs: mark Phase 4 complete in the roadmap
47735e2 docs: record Phase 4 in the migration plan
0f07417 docs: add the Phase 4 progress entry
49d2f76 release: bump to 0.551-beta after ordinary allocation isolation
98f0f9d docs: record 0.551-beta in the version log
```

The completion report, final count corrections and the release commit follow this
list.
