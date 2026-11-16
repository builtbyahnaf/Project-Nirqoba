# Phase 5 — Arena and Pool Support

**Starting version:** `0.551-beta`, commit `793cd97`
**Final version:** `0.563-beta`
**Scope:** MMM Phase 5 only. Phases 1–4, the collector, and every existing allocator remain in place.

---

## 1. Outcome

Arena-bound storage is now a checked language lifetime rather than a reserved
ownership spelling:

- `memory.arena(capacity)` and `memory.pool(max_object_size)` create tracked
  handles. The existing raw `memory.arena_allocator` and
  `memory.pool_allocator` constructors keep their old semantics.
- `memory.allocate<T>(bytes, arena_or_pool)` produces an `ArenaBound` value and
  records which tracked lifetime owns it.
- `memory.arena_reset`, `memory.arena_destroy`, `memory.pool_reset` and
  `memory.pool_destroy` invalidate every value and borrow in that lifetime.
- `memory.pool_release(pointer)` returns one slot to its pool.
- Arena/pool identity survives Sema → TIR and is verified there.
- Tracked arena allocation uses `nrq_arena_allocate`, a registry-free bump path.
  Pools keep the registered allocation path because release needs the slot
  record.
- Arena and pool workloads emit no `nrq_gc` reference at `-O0` or `-O2`.

Bulk release does **not** run object destructors. Payloads that require
deterministic cleanup are rejected instead; the limitation is explicit in §7.

---

## 2. Mandatory audit

`reports/memory-model/phase-5-audit.md` records the starting state:

- `ValueOwnership::ArenaBound` already existed and was already rejected as an
  ordinary return category, but no allocation produced it.
- `ArenaAllocator` and `FixedPoolAllocator` already provided bump/free-slot
  allocation, alignment, additional backing chunks and reset.
- `nrq_memory_allocate_from` and the per-pointer registry already existed, and
  `nrq_allocator_reset` correctly refused while allocations were live.
- The missing pieces were arena identity, scoped source rules, bulk
  invalidation, and TIR preservation/verification.

The audit's proposed lowering was conservative: reuse
`nrq_memory_allocate_from` for arenas. Implementation refined that after
measurement: tracked arena allocations now use `nrq_arena_allocate`, avoiding a
registry insertion for storage that can never be released individually. Pools
continue to use `nrq_memory_allocate_from` because `memory.pool_release` needs
the recorded size class. This refinement changes no language rule and is
recorded here rather than left as a stale audit claim.

No parallel allocator was introduced.

---

## 3. Semantics

### 3.1 Tracked handles

```text
let arena = memory.arena(capacity_bytes);
let pool  = memory.pool(max_object_size);
```

Both are opaque `u64` handles at runtime, but Sema gives each binding a unique
arena id and a kind (`1` arena, `2` pool). Raw allocator handles are not
implicitly promoted: `memory.arena_allocator` and `memory.pool_allocator` still
produce untracked handles with the older explicit reset/destroy contract.

### 3.2 Allocation

```text
let p = memory.allocate<T>(bytes, arena);
```

When the second argument names a tracked handle, Sema marks the result
`ArenaBound`, copies the arena id/kind, and does not arm per-object cleanup.
The payload must satisfy the existing explicit-storage cleanup restriction. The
allocation result may be copied as a non-owning alias, used through normal
pointer operations, and borrowed under Phase 3 rules.

A copy of the handle is not a new lifetime. Reset/destroy operate on the
tracked binding; the verifier still requires an actual `memory.arena`/`memory.pool`
call in the function for every arena id it sees.

### 3.3 Reset and destruction

All four bulk operations require the matching live tracked handle and reject a
destroyed handle. Reset and destroy are rejected while any arena-bound value or
borrow with that id is live in a visible scope. A successful reset/destroy marks
every prior arena-bound binding moved, so later use is a compile-time
use-after-free. A successful destroy also marks the handle itself destroyed;
a second destroy is rejected before lowering.

The runtime functions validate handle kind, refuse while an allocation operation
is in flight, erase any registry records owned by the region, release every
backing chunk, and (for destroy) remove the handle. The registry-erasure step is
still required for pools and for mixed code that used the registered path.

### 3.4 Pool release

```text
memory.pool_release(p);
```

Only a value allocated from a tracked pool is accepted. A second release, a
release through an unrelated allocator, and a release while a borrow is live are
rejected. Release marks the allocation moved, so later use is rejected. Lowering
calls `nrq_memory_deallocate_checked`, which returns the slot to its size class;
the pool keeps the backing storage until reset/destroy. No TIR `Move` is emitted:
the value is arena-bound, not individually owned.

### 3.5 Payload cleanup rule

`ArenaBound` was removed from the individual owning/destruction predicates.
Bulk release cannot discover field destructors, so an arena/pool allocation whose
payload requires deterministic cleanup is rejected by the existing explicit
allocation rule. This deliberately rejects `owned` fields, shared tokens, borrows
and similar payloads. Bulk release is therefore sound for every accepted payload
because accepted payloads have nothing to destruct.

---

## 4. Verification

`tir::Value` now carries `arena_id` and `arena_kind`; loads, calls, moves,
projections and address-of results preserve them. `compiler/lib/TIR/Verifier.cpp`
checks:

- every `ArenaBound` value has a non-zero id naming a tracked handle declared in
  the same function;
- an arena-bound allocation's result id matches its handle argument and its
  storage is an ordinary pointer;
- reset/destroy name a tracked handle;
- no `ArenaBound` value is used after a reset/destroy of its lifetime;
- no borrow of that lifetime is live at reset/destroy;
- no arena-bound value feeds `memory.deallocate`, a secure/owned release, or an
  owning parameter;
- no arena-bound value becomes an ordinary return.

The existing CFG-aware `verify_borrow_flow` pass still runs over the whole
function, so branches and loop back-edges cannot hide a borrow conflict.

---

## 5. Escape rejection

Sema rejects arena-bound storage in:

- `return` values;
- owning parameters (only borrowed parameters are accepted);
- borrowed returns whose source is arena-bound;
- struct/aggregate fields and array/tuple initializers;
- assignment to a non-arena owner or to a value from a different arena id;
- ordinary `memory.deallocate` and other per-object releases.

Tests cover each path directly. Borrowed access remains allowed while the arena
is live; reset/destroy then require those borrows to have ended.

---

## 6. Allocation, reset and cleanup evidence

### 6.1 Runtime calls

| Source operation | Lowered call |
|---|---|
| `memory.arena(capacity)` | `nrq_allocator_create(2, capacity)` |
| `memory.pool(max)` | `nrq_allocator_create(3, max)` |
| arena-bound `memory.allocate` | `nrq_arena_allocate(handle, bytes, align)` |
| pool-bound `memory.allocate` | `nrq_memory_allocate_from(handle, bytes, align)` |
| arena/pool reset | `nrq_arena_reset(handle)` |
| arena/pool destroy | `nrq_arena_destroy(handle)` |
| `memory.pool_release` | `nrq_memory_deallocate_checked(pointer)` |

`nrq_arena_allocate` takes the allocator handle lock only to resolve and pin the
allocator, then performs a serialized bump allocation; it inserts no registry
record. Alignment is passed through. Additional backing chunks are allocated by
the existing arena when the requested capacity is exceeded.

### 6.2 Reset correctness

`nrq_arena_reset` and `nrq_arena_destroy` accept both arena and pool kinds (the
source-level spelling already checks the exact kind), reject in-flight
operations, erase registered allocations for the region, reset the allocator,
and remove the handle on destroy. Repeated reset is supported. Reset after
destroy returns `0`; the source compiler rejects it earlier.

### 6.3 GC independence

`tests/integration/mlir/arena_gc_independence`,
`arena_gc_independence_o2` and `pool_gc_independence` compile arena/pool
programs and assert the LLVM output contains **zero** occurrences of `nrq_gc`.
They also assert the expected arena allocation/reset/destroy or pool
allocation/release calls are present. This counts declarations as well as calls.

---

## 7. Limitations

- Arena identity is compile-time metadata plus the runtime handle contract.
  MLIR/LLVM do not receive a per-object arena tag. Hand-written runtime calls can
  bypass the source rules; the TIR verifier is the compiler boundary.
- Bulk release does not run object destructors. Arena/pool payloads needing
  deterministic cleanup are rejected, not supported with a finalizer walk.
- Reset and destroy are checked against lexically visible values and borrows.
  A raw pointer laundered through an external call or untyped integer cannot be
  tracked; the language has no such arena escape today, but FFI is out of scope.
- Arena reset releases backing chunks rather than retaining them for reuse.
  This is the existing allocator behavior and is visible in the benchmark's
  reset-heavy workloads.
- The collector remains fully operational for explicit managed arrays, SoA,
  managed fields and cyclic graphs. No GC code was removed or disabled.
- No Phase 6 weak/shared completion, Phase 7 automatic strategy selection,
  Phase 8 FFI/manual-memory work, runtime borrow tracking, or automatic arena
  inference was added.
- No exception/panic unwinding exists. `system.exit` does not run lexical
  cleanup.
- No ASan/UBSan configuration is available in the MSVC build.

---

## 8. Tests

- Unit: **320 passed**, 0 failed.
- Full runner: **583 passed**, 0 failed, 0 skipped.
- New language cases: **26** under `tests/memory_model/`:
  - lifecycle, growth and repeated reset;
  - borrow, nested, branch and loop scope;
  - reset/destroy with live values or borrows;
  - move, return, call and borrowed-return escapes;
  - field/array escapes and ordinary deallocation;
  - cleanup payload rejection, double destroy and allocation after destroy;
  - pool lifecycle, double release, use-after-release, borrow release and wrong
    release.
- New IR cases: **3** (`arena_gc_independence` at `-O0` and `-O2`, plus
  `pool_gc_independence`).
- Unit coverage added: aligned arena reuse, pool reset, TIR arena identity and
  the arena-bound no-individual-owner classification.
- The Phase 1–4, GC, allocator, secure, stack, shared, destruction and full
  suites pass in the same run.

---

## 9. Benchmarks

`benchmarks/phase5_arena/run_benchmark.py` compiles five workloads at `-O2` and
reports median, minimum and maximum TSC cycles over repeated fresh processes.
The timer wraps only the workload. Checksums prevent dead-code elimination; each
workload writes and reads bytes that it observes.

| Workload | Median cycles | Min–max | Median ratio vs ordinary |
|---|---:|---:|---:|
| small ordinary (1M alloc/free, dynamic width) | 141,824,056 | 136,747,613–150,300,998 | 1.000 |
| small pool (1M alloc/release) | 344,364,634 | 338,669,833–349,508,788 | 2.428 |
| small arena (1M allocations, reset per 1,024) | 128,661,766 | 120,405,143–206,520,551 | 0.907 |
| mixed ordinary (500k alloc/free, five sizes) | 72,317,346 | 70,439,850–86,641,181 | 1.000 |
| mixed arena (500k allocations, reset per 512) | 128,853,940 | 63,554,931–141,181,086 | 1.782 |

Interpretation:

- The pool workload is consistently slower than the ordinary allocator because
  every release goes through the checked registry and the pool's slot metadata.
  This is an honest negative result.
- The small arena median is 9.3% faster than ordinary allocation, with a wide
  range that includes runs slower than the baseline. No stable speedup is claimed.
- The mixed arena is bimodal: some runs are about 12% faster than ordinary and
  others about twice as slow. The median reports the slower mode; the spread is
  retained rather than selecting the favorable samples.
- Mixed sizes and reset frequency make the arena and ordinary workloads measure
  different bulk-lifetime behavior; the comparison is workload-specific, not a
  universal arena performance claim.
- The TSC is constant-rate but scheduler placement and chunk reclamation vary on
  this machine. Fifteen samples were collected; the table repeats the measured
  result rather than smoothing it away.

Exact samples and the compiler path are in
`benchmarks/phase5_arena/results.json`.

---

## 10. Compatibility

- Existing raw allocator constructors, `nrq_allocator_reset`,
  `nrq_allocator_destroy`, `nrq_memory_allocate_from` and
  `nrq_memory_deallocate_checked` keep their behavior.
- Valid Phase 1–4 programs are unchanged. The new rejections apply only to the
  new tracked constructors and to arena-bound values, which did not exist
  before.
- Pool release is a new operation; ordinary `memory.deallocate` of a pool value
  is deliberately rejected.
- No syntax was added to the parser: the tracked constructors are existing
  namespaced builtins with new semantic treatment. Raw names remain distinct.

---

## 11. Phase 5 commits

```
cdf6cd6 docs: audit arena and pool support before Phase 5
c889f77 sema: attach arena identity to tracked handles and bindings
c66688d own: stop treating arena-bound storage as an individual owner
7929bc1 sema: enforce arena lifecycle, escapes and pool release
09f7186 release: bump to 0.555-beta after tracked arena semantics
ac7edcb tir: preserve arena identity through values and transfers
a170f8f tir: verify arena allocation, reset and destruction contracts
48b69e5 release: bump to 0.559-beta after arena TIR verification
480b8aa lower: use the arena fast path for tracked arena allocations
92d4450 runtime: add registry-free arena allocation and bulk invalidation
2bc807d release: bump to 0.563-beta after arena lowering and runtime
c1dbd9f tests: cover arena bulk reset, identity and ownership classification
1f531dd tests: exercise arena allocation, growth and repeated reset
01dc4bd tests: keep arena borrows valid across scopes, branches and loops
06265fa tests: reject bulk release while arena values or borrows are live
bc2dfb6 tests: reject arena escapes through moves, returns and calls
7dcf5b7 tests: reject arena storage in aggregates and ordinary deallocation
2681e65 tests: enforce arena payload and handle lifecycle limits
0be00f7 tests: exercise pool reuse and reject invalid pool lifetimes
e3749c1 tests: reject pool release through a borrow or another allocator
e8c792a tests: prove arena and pool workloads contain no collector references
290d9df tests: reject reset while an arena value is still borrowed
5af00e1 bench: compare arena and pool workloads with ordinary allocation
30b0683 docs: record Phase 5 arena and pool completion
be851fb docs: mark Phase 5 complete in the migration plan
60ca238 docs: add the Phase 5 progress entry
```

The Phase 5 completion-report commit is the 27th commit in this list; a commit
cannot contain its own hash.

---

**Phase 6 was not started.**
