# Phase 9 migration guide — optimization and stabilization

**Version:** `0.591-beta`  
**Previous phase:** Phase 8 — manual memory and the FFI boundary  
**Scope:** existing memory model only; Phase 10 CDMM is not implemented

---

## 1. Is my program affected?

Most programs need no source change. Phase 9 optimized runtime and lowering paths
without adding or removing a memory strategy.

| Existing code | Status |
|---|---|
| `memory.allocate` / `memory.deallocate` | Unchanged surface and lifetime |
| Borrows and mutable borrows | Unchanged; still direct access |
| `memory.arena` / `memory.pool` | Unchanged lifetime/release rules |
| `memory.shared_allocate` / `memory.shared_release` | Unchanged retain/release semantics |
| `array.allocate`, `managed<T>`, `gc.collect` | Unchanged roots/barriers/safepoints |
| `memory.manual_allocate` / `memory.manual_release` | Unchanged manual contract |
| Native file/socket/mutex/mapping/secure owners | Unchanged |
| FFI | Still unsupported; no `extern` surface exists |

If a program relied on an implementation detail that the documentation did not
promise (for example, a particular hidden allocation prefix), it must not. The
only observable ordinary-allocation change is lower peak memory and slightly
different release timing; ownership, alignment and invalid-free diagnostics are
preserved.

---

## 2. What changed

### 2.1 Natural-aligned heap blocks no longer use a prefix

On x64 the Windows process heap returns `max_align_t`-aligned storage. Requests
at or below that alignment now use the heap block directly. Over-aligned
requests still allocate a prefix and adjust the address. The ownership registry
records the requested alignment, so release chooses the matching path.

No universal header was added. The registry still detects invalid and repeated
releases and preserves allocator identity.

### 2.2 Cached releases no longer wake a condition variable

Allocation waits only while an ownership record is marked in flight. The
thread-local cache release path only erases records that were never marked in
flight. Removing the wakeup is therefore invisible to correct programs. The
slow release path still notifies after clearing an in-flight record.

### 2.3 Dead cleanup clears are removed before return

TIR still records one destruction obligation and one `Drop` per owner. Lowering
expands the drop to the runtime release and then removes a null constant/store
pair only when it is immediately before `tir.return`. The release and
reverse-order field destruction are untouched, and the slot has no later user.

### 2.4 No strategy or FFI was added

Phase 9 did not add CDMM, automatic strategy selection, weak references, a new
cycle collector, a foreign ABI, or a universal object header. Arena, pool,
shared and GC implementations were measured but not redesigned because the
evidence did not justify a safer/cheaper alternative within this phase.

---

## 3. Compatibility checks

The Phase 9 matrix builds representative ownership fixtures at `-O0` and `-O2`,
with the production and debug runtimes, and requires identical exit status and
stdout. It also inspects generated LLVM at `-O0` for duplicated releases and
unwanted `nrq_gc` dependencies.

Existing examples still check successfully, including the ordinary, GC-managed
and multi-file programs. The full runner and Phase 7 isolation script remain
green.

---

## 4. Migration actions

- **Ordinary/borrowed/arena/pool/shared/GC programs:** rebuild; no source edit.
- **Manual programs:** no change; the function-local restriction and explicit
  release pairing still apply.
- **Programs using explicit over-alignment:** no change; the over-aligned path
  is retained.
- **Programs expecting foreign interoperability:** still unsupported. Do not
  assume a foreign pointer, callback or ownership transfer exists.
- **Programs measuring release latency:** retained-set release alone may be
  slower even though allocation and total retained-set cost improved. Measure
  the actual workload rather than assuming a universal speedup.

---

## 5. Known limitations

- Retained-set release median regressed in the Phase 9 benchmark; the combined
  allocate+release total and peak RSS improved. This is documented in
  `reports/memory-model/phase-9-benchmarks.md`.
- Arena and pool each still take the handle-lifetime and strategy locks on every
  operation; removing them needs a new lifetime protocol.
- Shared retain/release remains shard-lock protected; no weak references or new
  cycle collector were added.
- Manual storage remains function-local and cannot be reallocated.
- FFI remains unsupported.
- Phase 10 CDMM has not started.
