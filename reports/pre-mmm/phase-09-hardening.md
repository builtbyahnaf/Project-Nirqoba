# Pre-MMM Phase 9 — Ownership Hardening Pass

**Date:** 2026-11-14  
**Version:** `0.515-beta`  
**Status:** Complete  
**Scope:** Three critical Phase 0 findings: owned-only GC isolation, secure
managed-payload rejection, and structural TIR ownership verification.

---

## 1. Why this pass exists

Phase 0 froze the migration architecture but left three prerequisites unresolved:

1. owned-only destruction emitted `nrq_gc_root_unregister` even when the released
   type contained no managed reference;
2. `memory.secure_allocate<T>` accepted managed fields without a rooted secure
   allocation path;
3. `verify_tir_module` checked structure and types but not ownership, release or
   borrowing invariants.

Phase 9 closes those three findings without starting the dedicated migration's
Phase 1. It adds no syntax, no runtime ownership model and no collector redesign.
The existing Sema analyses remain the language-level safety authority.

---

## 2. Finding 1 — owned-only release no longer touches the collector

### Change

Lowering now asks whether the released payload actually contains a managed
reference. Both owned aggregate destruction and owned-array release skip
`nrq_gc_root_unregister` when the answer is no. Managed payloads keep their
existing register/unregister pair.

The predicate is structural over the existing lowered type. It follows arrays,
tuples, aggregates and nested managed fields; a pointer or view is not treated as
managed merely because it can point at managed storage.

### Evidence

Generated LLVM (`--emit=llvm-raw`):

| Program | Release path | `nrq_gc_root_unregister` at `-O0` | at `-O2` |
|---|---|---:|---:|
| `owned_aggregate` | `aggregate.owned_drop` | 0 | 0 |
| `owned_array<int>` | `array.owned_release` | 0 | 0 |
| owned pointer, non-promotable | `memory.deallocate` | 0 | 0 |
| borrowed `array_view<int>` | view release | 0 | 0 |
| stack-promoted owned array | scope exit | 0 | 0 |
| `owned_array<managed<int>>` | `array.owned_release` | 1 | 1 |
| managed pointer aggregate | `aggregate.owned_drop` | 1 | 1 |

Object relocation check (`llvm-nm -u`) on the `-O2` owned aggregate object shows
`nrq_memory_deallocate_checked` and no `nrq_gc_root_unregister`. The managed-array
object still references `nrq_gc_root_unregister`.

The non-promotable pointer fixture sizes its allocation from `args.length`, so the
deallocation cannot be folded into a stack slot; the test therefore observes a real
release path rather than an optimized-away one.

### Effect

Owned-only destruction no longer takes the collector lock or performs registry
lookup. No runtime benchmark was run in this pass, so no speedup is claimed. The
observable structural effect is that the call disappears from IR and from the
object's undefined symbols. At `-O2` the owned-aggregate executable is 62,976 bytes
against the Phase 0 audit's 64,512-byte baseline (a 1,536-byte / 2.4% reduction);
the scalar-only baseline remains 20,480 bytes. This is an artifact-size observation,
not a throughput measurement.

---

## 3. Finding 2 — secure storage rejects managed payloads

### Change

`TypeChecker` rejects `memory.secure_allocate<T>` when `T` contains a managed
reference at any depth. The diagnostic is `NRQ3015`:

```
secure allocations cannot contain a managed field in '<type>':
secure release has no rooted managed-reference contract
```

The rejection is deliberate rather than a silent fallback. A secure allocation is
zeroed and released without walking fields or registering a collector root. The
previous positive test survived only because the managed allocation stayed alive in
a local root slot; that is not a contract the secure payload can rely on.

### Coverage

| Case | Result |
|---|---|
| `secure_allocate<int>` | supported; secure release, zero GC calls |
| secure aggregate with plain fields | supported |
| secure aggregate with direct managed field | rejected |
| secure aggregate with nested managed field | rejected |
| `secure_allocate<managed<Node>>` | rejected |

The previously positive `secure_allocation_managed_payload` fixture is now negative.
The supported plain path remains positive and asserts one
`nrq_memory_secure_release` call with no collector unregister.

Secure managed support is retained as future work only if a rooted secure
allocation path is designed. Until then, `array.allocate<T>` remains the explicit
collector-supported allocation route.

---

## 4. Finding 3 — TIR verifies ownership, release and borrowing

### Representation

`tir::Function` now stores the ownership of every allocated value. Operations keep
their result handles, so a pass cannot silently disagree with the function's value
record; the verifier rejects a handle whose ownership differs from that record.

Ownership remains structural metadata over the existing IR. No move/drop
instructions, no new syntax and no replacement for Sema lifetime analysis were
introduced.

### Checked invariants

`verify_tir_module` now rejects:

- an owned array element that is not an owned pointer;
- an owned array of managed elements, an owned aggregate of owned arrays, or an
  owned pointer to managed storage;
- an owned pointer value with a non-pointer type;
- an owned parameter with a non-pointer, non-owned-array, non-owned-aggregate type;
- an owning function returning borrowed or shared ownership;
- a borrowed return without a source parameter, or with a source that is not a
  borrowed parameter of the same pointee type;
- release of a borrowed, shared or managed value;
- a managed release whose kind does not match the released value's managed kind;
- consumption of a value twice;
- a result whose ownership handle disagrees with the stored value record;
- call arguments that violate the callee's declared parameter ownership:
  borrowed to owned, shared to owned, immutable borrow to mutable parameter;
- owned values passed to borrowed parameters, which remain legal as an implicit
  temporary borrow.

The verifier is structural and flow-insensitive. It does not prove that every path
releases an owner, and it does not replace path-sensitive Sema borrow checking.
Those remain Phase 1 design work.

---

## 5. Regression coverage

New and updated fixtures:

- `owned_aggregate_gc_isolation_o0`, `..._o2`
- `owned_array_gc_isolation_o0`, `..._o2`
- `owned_pointer_gc_isolation_o0`, `..._o2`
- `owned_array_view_gc_isolation_o0`
- `stack_owned_array_gc_isolation_o0`
- `owned_array_gc_roots` (managed element keeps register/unregister)
- `secure_allocation_plain_payload` (supported, GC-free)
- `secure_allocation_managed_payload` (now negative)
- `secure_allocation_nested_managed_payload` (negative)
- `secure_allocation_direct_managed_payload` (negative)
- `vectorized_array_alignment` (seed made runtime-dependent after the opaque call
  was removed, so the alignment checks cannot be constant-folded away)

Unit coverage adds malformed TIR modules for type/ownership mismatches, mutable and
owned parameters, invalid returns and borrow sources, release-family mismatches,
borrowed release, double consumption, call-boundary mismatches, and forged result
ownership.

**Final suites:** 270/270 unit tests and 500/500 integration tests pass on Windows
x64/MSVC, RelWithDebInfo.

---

## 6. Compatibility and limits

- No source syntax changed.
- Valid owned, borrowed, shared, managed, SoA, allocator and secure programs remain
  valid, except programs that relied on managed fields in secure storage.
- Managed secure programs were not actually rooted before this pass; they now fail
  at compile time instead of relying on an incidental local root.
- No runtime benchmark was run. The performance statement above is limited to
  generated IR, undefined symbols and executable size.
- The Release configuration still fails on a pre-existing `gc_test.cpp` C4189
  warning under `/WX`; RelWithDebInfo remains the maintained test target.
- No ASan/UBSan run is available on this MSVC toolchain.

---

## 7. Remaining blockers for the dedicated migration's Phase 1

1. **First-class TIR ownership operations.** Ownership is verified metadata, not a
   move/drop instruction set. Phase 1 still needs explicit transfer and destruction
   operations, plus a pass contract for them.
2. **Path-sensitive destruction proof.** The verifier does not prove release on all
   paths, double release across branches, or cleanup on every early exit.
3. **Borrowing completeness.** Aggregate-field borrows, branch-selected owners and
   mutable-borrow exclusivity across nested calls remain Sema-level and incomplete.
4. **Arena object semantics.** Reset/destroy still refuse while live allocations
   exist; there is no scoped arena, bulk object destruction or escape checking.
5. **Secure managed rooting.** Rejected until a rooted secure allocation path is
   designed; do not re-enable the payload without one.
6. **Exception/panic cleanup.** Fatal exits still terminate without unwinding.
7. **Strings, weak references, FFI, threads and async** remain outside the current
   contract.

Phase 9 removes the three Phase 0 prerequisites. It does not claim that the
ownership representation is complete or that Phase 1 has begun.
