# Phase 6 — Explicit Shared Ownership

**Starting version:** `0.563-beta`, commit `2306fdf`
**Final version:** `0.571-beta`
**Scope:** MMM Phase 6 only. Phase 7 was not started.

---

## 1. Outcome

Strong shared ownership now has a thread-aware control path and a verified
consumer contract:

- one strong token on `memory.shared_allocate`;
- retain for another token, move without retain;
- exactly-once final release with the token removed before deallocation;
- 64 sharded control maps with atomic strong counts;
- shared release rejected while a borrow of the storage is live;
- TIR rejects malformed retain/count operands and ordinary release of shared
  values;
- plain shared allocation remains GC-free;
- arena reset reuses its capacity-bounded initial chunk.

Weak references and a cycle collector were deliberately not added. The reasons
and the cycle policy are recorded in §4 and §9.

---

## 2. Audit findings

`reports/memory-model/phase-6-audit.md` records the starting state:

- Shared ownership already existed as `ValueOwnership::Shared` and
  `TypeKind::SharedPointer`; there was no `SharedOwned` class.
- The runtime kept a single process-wide count map under the global
  `allocator_mutex`. Retain, release and count were thread-safe by serialization
  but were a global bottleneck.
- Final release reserved the count at zero, removed a typed root if present, and
  deallocated through the ownership registry.
- Aggregate copy already retained shared fields and aggregate move already
  transferred them.
- `ValueOwnership::Weak` existed only as a reserved category. There was no weak
  syntax, runtime API or cycle mechanism.
- No FFI surface exists.

The existing suite covered strong counts, parameter/return transfer, aggregate
fields, GC isolation, and a C++ concurrent retain/release test. It did not cover
independent-object shard progress, a source-level borrow/release conflict, or
arena reset retention.

---

## 3. Implemented semantics

### 3.1 Strong ownership

| Operation | Semantics |
|---|---|
| `memory.shared_allocate<T>(bytes)` | Allocates payload, creates one token |
| `memory.shared_retain(p)` | Creates a new token; no payload copy |
| `let b = a` for a shared local | Moves the token; no retain |
| Aggregate construction from a shared producer | Takes the initial token; no extra retain |
| Aggregate copy | Retains each shared field, including nested fields |
| Aggregate move | Transfers the token; no retain |
| `memory.shared_release(p)` | Consumes one token; final release frees the payload |
| `memory.shared_count(p)` | Returns the current strong count |
| Scope cleanup | Releases once through the generated obligation |

The final release is the only path that deallocates the payload. A release at a
count greater than one decrements and returns. A missing/zero token returns zero.
The runtime does not run collector operations for a plain shared payload.

### 3.2 Move and copy

A shared local is transferable. Moving produces a TIR Move with `Shared`
ownership and lowering aliases the value; no retain is emitted. Aggregate copy
emits `aggregate.shared_retain`, which calls `nrq_memory_shared_retain` once per
shared field. Aggregate destruction emits the matching release in reverse field
order. Partial initialization uses zeroed fields, so an uninitialized shared
field is skipped by the null check.

### 3.3 Borrow interaction

A shared pointer can be dereferenced and borrowed inside a lexical scope. The
borrow does not create a token. A direct `memory.shared_release` while that
borrow is live is now rejected in Sema with:

```text
cannot release a shared owner while a borrow of its storage is live
```

Previously this case was left to TIR borrow-flow verification and produced an
internal `NRQ4007` error mentioning an end-borrow path. The new diagnostic is at
the source release site; valid scoped borrows still pass.

---

## 4. Weak references and cycles

### 4.1 Decision

Weak references are deferred. The original target architecture permits them only
with a concrete standard-library requirement and a testable expiration contract.
Nirqoba has no `weak` type, parser production, library container or existing
program that needs one. Adding a runtime-only weak API would not create a usable
language-level cycle solution, and adding a full weak type would expand Phase 6
beyond its ownership scope.

### 4.2 Cycle policy

A supported shared payload cannot contain a shared field: Sema rejects any
payload whose `requires_deterministic_cleanup` is true, which includes shared
pointers, owned pointers, owned arrays, SoA, array views and borrowed views.
Consequently a shared allocation cannot store an owning edge back to another
shared allocation. Lexical shared fields can point at independently allocated
objects, but the pointed-to payload cannot itself contain a shared field.

Reference cycles among shared objects are therefore outside the supported
payload surface. Cyclic graphs remain the domain of the existing explicit GC
(`array.allocate`, SoA, managed fields). No cycle collector was added and no GC
semantics changed. If future containers allow shared fields inside shared
payloads, they must first add field destruction and then a weak/cycle strategy.

---

## 5. Concurrency design

### 5.1 Runtime layout

```text
SharedControl { std::atomic<uint64_t> strong; }
SharedShard   { std::mutex mutex; unordered_map<void*, unique_ptr<SharedControl>> }
64 shards, selected by std::hash<void*>
```

The shard lock protects the control entry's lifetime. The atomic count is the
public reference count. No global allocator mutex is taken by retain, a
non-final release, or a count query.

### 5.2 Ordering

- Retain: acquire/release CAS; refuses zero and `UINT64_MAX`.
- Non-final release: acquire/release subtraction under the shard lock.
- Final release: 1→0 store with release ordering, control removed from the shard
  before deallocation.
- Count query: acquire load under the shard lock.
- A concurrent retain after the 1→0 transition sees a missing entry and returns
  null. It cannot resurrect the object.
- The allocation ownership record is marked in flight before the final
  deallocation, so allocator reset/destroy cannot race the callback.

Different shared objects in different shards do not serialize on one another.
Access to the pointed-to data is not protected by the reference count.

### 5.3 Overflow and invalid operations

`nrq_memory_shared_retain` returns null at `UINT64_MAX` or for a missing token.
`nrq_memory_shared_release` returns zero for a missing/already-final token.
`nrq_memory_shared_count` returns zero for a missing pointer. The language does
not expose a separate result code for a failed retain; this is the existing ABI
and is documented rather than hidden.

Reaching `UINT64_MAX` is not practical through the language, so no test performs
2^64 retains. The CAS branch is exercised by the implementation contract; tests
cover final-token lookup failure and underflow.

---

## 6. Phase 5 limitation review

| Limitation | Outcome | Detail |
|---|---|---|
| A. Arena bulk release does not run destructors | **Deferred** | A correct path needs typed cleanup metadata, reverse construction order and a bounded registration log. The compiler already rejects payloads whose cleanup bulk release cannot perform. A per-object registry was not added without measured justification. |
| B. Arena reset releases backing chunks | **Partially fixed and verified** | Reset retains the initial capacity-bounded chunk (when its size is at most `capacity_`), frees growth chunks and rewinds the bump pointer. Destroy frees the retained chunk. Unit test verifies address reuse; benchmark reports the delta. |
| C. Arena identity in MLIR/LLVM | **Deferred, no tags** | Sema/TIR identity plus the runtime handle preserve the required contract. Lower IR receives the handle and allocation calls. Per-object tags would add overhead without a correctness or diagnostics need. |
| D. FFI pointer laundering | **Fundamentally unprovable; no FFI surface** | There is no FFI or unsafe scope. No FFI safety claim is made; supported language paths cannot launder an arena/shared token through foreign code. |
| E. Weak references | **Deferred and scoped** | See §4. No source surface or concrete requirement; GC remains the cycle mechanism. |

### 6.1 Arena reset behavior

`ArenaAllocator::reset` now:

1. keeps the first chunk only if `chunks_.front().size <= capacity_`;
2. frees every growth chunk;
3. sets the retained chunk's `used` to zero;
4. leaves the destructor to free the retained chunk.

This is bounded by the arena's declared capacity, prevents stale pointers from
becoming valid (the source compiler still invalidates every prior binding), and
keeps destruction safe.

---

## 7. Compiler and IR evidence

- `compiler/lib/Sema/TypeChecker.cpp` rejects shared release with a live borrow.
- `compiler/lib/TIR/Verifier.cpp` requires `Shared` ownership or a shared-pointer
  type for `memory.shared_retain` and `memory.shared_count`; malformed operands
  are rejected before lowering.
- Existing checks remain: shared allocation strategy, shared parameter matching,
  shared release family, no shared value through `memory.deallocate`, shared
  fields in aggregates, and shared GC-root retention.
- `tests/unit/ownership_test.cpp` now has a malformed retain-operand case plus
  the existing shared move and allocation-strategy cases.
- Lowering is unchanged for ordinary shared operations: allocation calls
  `nrq_memory_shared_allocate` (or the rooted variant), retain calls
  `nrq_memory_shared_retain`, release calls `nrq_memory_shared_release`, count
  calls `nrq_memory_shared_count`.
- Plain shared allocation still emits zero `nrq_gc` references at `-O0`/`-O2`
  (existing fixtures continue to assert this).

---

## 8. Tests and benchmarks

### 8.1 Test totals

- Unit: **323 passed**, 0 failed.
- Full runner: **593 passed**, 0 failed, 0 skipped.
- New Phase 6 language cases: **10**
  - multiple owners and final release;
  - aggregate copy retain;
  - shared borrow without a new token;
  - shared release with a live borrow (negative);
  - shared payload with a shared field rejected (negative);
  - parameter release;
  - aggregate move without retain;
  - nested aggregate copy;
  - loop-scoped release;
  - double release after move (negative).
- Unit additions:
  - 64 independent objects retained/released across worker threads;
  - bounded arena reset reuses the initial chunk;
  - malformed shared retain operand rejected by TIR.
- Existing shared, aggregate, GC-root, GC-isolation, ownership, borrow,
  arena/pool and full suites pass in the same run.

Commands:

```text
./build/msvc-dev/tests/nirqoba_unit_tests.exe
python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe --configuration RelWithDebInfo
python benchmarks/memory-model/phase-6/run_benchmark.py --runs 7
```

### 8.2 Benchmark methodology

Language workloads are compiled at `-O2` and run in fresh processes. Each
workload prints a checksum and TSC cycles around the measured loop. The runner
reports median, minimum and maximum over seven runs. The C++ contention harness
is built with the MSVC x64 developer environment and linked against the
production runtime; it runs five fresh processes.

### 8.3 Language workload results

| Workload | Median cycles | Min–max | Comparison |
|---|---:|---:|---:|
| ordinary allocation (1M) | 139,740,104 | 133,860,144–141,090,405 | baseline |
| shared allocation + release (1M) | 609,315,879 | 590,192,402–642,843,367 | **4.360× ordinary** |
| shared allocation + explicit retain (1M) | 776,471,126 | 741,590,752–788,994,599 | **1.187× shared move** |
| shared allocation + move (1M) | 654,207,332 | 645,581,029–665,484,486 | move avoids retain |
| aggregate shared copy (200k) | 153,576,689 | 149,262,534–154,996,444 | copy retains fields |
| arena reset reuse (1M allocations) | 123,212,845 | 117,253,014–126,650,787 | Phase 5 median was 128,661,766 |

Interpretation:

- Shared allocation is **4.36× slower than ordinary allocation** in this
  workload. The difference is the control entry, shard lock, atomic count and
  root/ownership bookkeeping. This is an honest negative result; shared
  ownership is not claimed to be free.
- An explicit retain adds about **18.7%** over a move for the same allocation
  count.
- Aggregate copies are more expensive than a single token operation because each
  copy retains and each scope exit releases; the workload is reported without a
  claim of superiority.
- Arena reset median is about **4.2% lower** than the Phase 5 median, but the
  ranges overlap. The improvement is consistent with reusing the initial chunk;
  it is not presented as a statistically strong speedup.
- The benchmark does not include destructor execution because supported shared
  payloads reject deterministic-cleanup fields. Final release cost is therefore
  the control lookup, root check and deallocation path.

### 8.4 Contention and memory

Four worker threads, 250,000 retain/release pairs each (1,000,000 total):

| Case | Median | Min–max | Notes |
|---|---:|---:|---|
| uncontended (one thread) | 47,767,200 ns | 43,317,700–48,449,500 | one object |
| independent objects (four threads) | 33,786,400 ns | 30,633,200–38,736,400 | one object per thread |
| contended (four threads, one object) | 75,782,500 ns | 73,012,000–77,401,600 | one shard |

The independent-object aggregate is faster than the single-thread aggregate,
showing that unrelated objects progress without a global lock. It does not scale
fourfold in this environment. Contention on one object costs about 1.59× the
uncontended path; the shard lock still serializes that object by design.

The memory probe allocated 100,000 live 32-byte shared objects and measured a
working-set delta of about **251.49 bytes per object**. This includes the payload,
control allocation, map node, allocator registry record and allocator overhead;
it is an approximate working-set measurement, not a control-block-only size.

---

## 9. Limitations and risks

- Weak references are not implemented. There is no weak type or promotion API.
- Shared-to-shared cycles are not constructible in supported shared payloads
  because shared fields are rejected. If that restriction is lifted, a weak or
  cycle design is required.
- Reference counting does not make pointee access thread-safe.
- No FFI/unsafe surface exists, so cross-FFI laundering is outside the supported
  contract and is not claimed to be analyzable.
- A failed retain returns null; the language has no dedicated overflow error
  value.
- The final release still needs the allocator ownership record and may touch the
  allocator's shard; only the shared control path is sharded.
- Shared payloads reject deterministic-cleanup fields, so no destructor is run
  on final release. This is the same payload restriction Phase 4/5 established.
- The arena retention change keeps only the initial capacity-bounded chunk; a
  request larger than the initial capacity causes the retained chunk to be
  dropped on reset, matching the bounded-memory rule.
- No ASan/UBSan configuration is available in the MSVC build.
- No Phase 7 GC strategy work, no collector redesign and no automatic shared
  promotion was added.

---

## 10. Compatibility

- Ordinary owned and borrowed execution remains independent of GC.
- `memory.shared_allocate`, `shared_retain`, `shared_release` and `shared_count`
  keep their source ABI and pointer representation.
- Existing raw allocator handles, arena/pool behavior, aggregate destruction and
  Phase 1–5 tests remain valid.
- The only new source rejection is a shared release while a borrow is live; the
  old path was already rejected later by TIR verification.
- Manifest/MLIR/LLVM representation of a shared pointer is unchanged: a plain
  pointer plus the runtime calls. No per-object shared tag was added.

---

## 11. Phase 6 commits

```
b12e507 docs: audit explicit shared ownership before Phase 6
f316f92 runtime: shard shared control and retain bounded arena capacity
ef99da6 release: bump to 0.567-beta after shared control and arena reset reuse
97459e8 tir: require shared ownership for retain and count operands
d1b4d4f sema: reject shared release while its storage is borrowed
b5a60a8 release: bump to 0.571-beta after shared contract hardening
543ea74 tests: stress sharded shared controls and retained arena capacity
88095e1 tests: exercise multiple shared owners and final release
dfd30f2 tests: retain a shared field on aggregate copy
a32a3b9 tests: borrow shared storage without acquiring another token
94d316f tests: reject shared release while a borrow is live
9ec5251 tests: reject shared payloads that could close an owning cycle
a06a651 tests: release one shared token inside a callee
e6f002b tests: move an aggregate without retaining its shared field
ea6e741 tests: retain nested shared fields through an aggregate copy
14d6666 tests: release a shared owner at each loop iteration
3d964db tests: reject a second shared release after the owner moved
4c7201a bench: add shared allocation, token and arena reset workloads
fb4a2e5 bench: measure shared control contention and per-object memory
84b1385 bench: record Phase 6 shared-ownership measurements
2d142df docs: mark Phase 6 complete in the migration plan
392db62 docs: add the Phase 6 progress entry
925af33 docs: describe sharded shared ownership in the target architecture
318e0be docs: record 0.567-beta and 0.571-beta in the version log
9cb467e docs: mark Phase 6 complete in the README
9d2eb08 docs: add the Phase 6 migration note
```

The Phase 6 completion-report commit is the 27th commit in this list; a commit
cannot contain its own hash.

---

**Phase 7 was not started.**
