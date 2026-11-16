# Phase 9 completion report — optimization, compatibility, and stabilization

**Starting version:** `0.583-beta`  
**Starting commit:** `9280b37`  
**Final version:** `0.591-beta`  
**Final implementation-bearing commit:** `54eb267` (user commit)  
**Final documentation commit before this report:** `396fbfe`  
**Branch:** `main`  
**Phase 10 CDMM:** not started

---

## 1. Result

Phase 9 audited and stabilized the ownership-first hybrid memory model without
adding a strategy, changing a lifetime rule, or weakening a release, root,
barrier, or concurrency guarantee.

Three measured changes were kept:

1. Natural-aligned default heap blocks no longer carry a private prefix.
2. Cached release no longer signals a condition variable with no possible
   waiter.
3. Lowering removes a dead null/store cleanup pair immediately before
   `tir.return`.

One additional implementation simplification reads the default allocation path's
thread selection and cache switch once instead of twice.

Borrow lowering, arena, pool, shared ownership and GC were reviewed and measured.
No change was forced where the evidence did not justify one. In particular, no
lock was removed from arena/pool allocation, no new shared-control protocol was
introduced, and no GC instrumentation was weakened.

**FFI remains unsupported.** There is still no `extern`, foreign pointer,
callback, unsafe scope, or source-level ABI. No foreign ownership contract is
implemented or claimed.

---

## 2. Starting state

| Check | Result |
|---|---|
| Branch | `main` |
| HEAD | `9280b37` |
| Version | `0.583-beta` |
| Tree | clean |
| Unit | 329/329 |
| Full | 624/624 |
| Phase 7 isolation | passed |
| Phase 8 manual | passed |

The preflight matched the specification. All Phase 0–8 behavior and reports were
preserved.

---

## 3. Implementation changes

### 3.1 Natural-aligned allocation loses its private prefix

**File:** `runtime/src/allocator_windows.cpp`

`heap_alloc_aligned` previously always allocated
`bytes + alignment - 1 + sizeof(void*)`, stored the raw pointer before the
result, and `heap_free_aligned` always read it. On x64 the Windows process heap
already returns `max_align_t`-aligned storage.

Now:

- `alignment <= alignof(std::max_align_t)` allocates the heap block directly;
- over-aligned requests keep the prefix and adjustment;
- `heap_free_pointer(pointer, alignment)` selects the matching free path;
- the ownership registry still records allocator, bytes and alignment;
- invalid and repeated release still fail;
- the thread cache preserves each block's alignment.

`Allocator::deallocate_aligned` defaults to the existing `deallocate`, so pool,
arena, stack, buddy, free-list, TLSF and custom strategies keep their behavior.
Only the general allocator overrides it.

This is not a universal header removal and does not remove the ownership record.

### 3.2 Cached release does not wake an impossible waiter

**File:** `runtime/src/allocator_windows.cpp`

Allocation waits on `shard.changed` only when it finds a record with
`operation_in_flight`. The cache release path only handles records that were
never marked in flight and erases them. It cannot have a waiter. The
`notify_all()` call was removed from that branch. The slow path still notifies
after clearing the in-flight state.

### 3.3 Dead clear removed before return

**Files:** `compiler/include/nirqoba/TIR/IR.h`,
`compiler/lib/Lowering/TirToMlir.cpp`

The builder emits `constant_null` + `store` after discharging an owner so an
outer scope cannot reload it. When the pair is immediately followed by
`tir.return`, no later operation can read the slot. The lowering copy removes the
pair after verified TIR and after `Drop` expansion.

The release call, obligation discharge, reverse-order field destruction, partial
initialization, moves and branch cleanup are unchanged. The pass is deliberately
narrow: it does not remove a clear at a scope end, a branch merge, or any point
where a later operation might read the slot.

### 3.4 Single default-path decision

**File:** `runtime/src/allocator_windows.cpp`

`nrq_memory_allocate` now reads `thread_selection.allocator` and
`default_general_cache_enabled` once. A cache miss previously reread both before
taking the direct default path. The fallback remains the same static-lifetime
general allocator with no operation pin.

---

## 4. Modified components

| Component | Change |
|---|---|
| Runtime general allocator | direct natural-aligned allocation; alignment-aware free; cached-release wakeup removal; single default-path decision |
| TIR representation | dead-clear-before-return helper |
| Lowering | invokes the dead-clear helper on the lowering copy |
| Unit tests | alignment/cache round-trip and dead-clear contract |
| Phase 9 test script | O0/O2/production/debug matrix and LLVM checks |
| Benchmark harness | six modes covering owned, retained, manual, borrow, arena, pool, shared, contention, GC and mixed work |

No ownership category, allocation strategy, runtime ABI entry point, or foreign
ABI was added.

---

## 5. Rationale and generated-code evidence

- Prefix removal is supported by the x64 heap alignment contract and by the
  existing registry's recorded alignment. The unit tests write every byte of
  blocks at alignments 1–4096 and check the returned address.
- Wakeup removal follows from the wait predicate: only in-flight records are
  waited on, and the cache path never creates one.
- Dead-clear removal is visible in generated IR: the release remains and the
  null/store pair before return disappears. A unit test asserts one Drop expands
  to the release and the constant-null count decreases.
- The Phase 9 script compiles ten fixtures at O0/O2 and with production/debug
  runtimes, compares exit/stdout exactly, and checks O0 LLVM for duplicate
  manual releases and `nrq_gc` leakage.

No claim is made that LLVM needed help with a transformation it already performs
correctly. The dead-clear change reduces the IR the compiler itself emits; it is
not a replacement for LLVM optimization.

---

## 6. Before/after benchmarks

Runner: `benchmarks/memory-model/phase-9/`, MSVC `/O2`, fresh process per mode.
Baseline: 9 samples at `0.583-beta`. Final: 15 samples at `0.591-beta`.

| Measurement | Baseline | Final | Change |
|---|---:|---:|---:|
| Owned allocate/release | 58.499 ns | 58.553 ns | +0.1% |
| Retained allocation | 215.929 ns | 182.296 ns | **-15.6%** |
| Retained release | 108.320 ns | 129.552 ns | **+19.6%** |
| Retained allocate + release | 324.249 ns | 311.848 ns | **-3.8%** |
| Manual path | 61.021 ns | 58.608 ns | -4.0% |
| Borrowed read | 0.073 ns | 0.073 ns | unchanged |
| Arena allocate | 48.856 ns | 45.272 ns | -7.3% |
| Arena reset | 194.400 ns | 185.700 ns | -4.5% |
| Pool allocate/release | 152.806 ns | 133.137 ns | -12.9% |
| Shared allocate + 8 retain/release | 644.529 ns | 592.663 ns | -8.1% |
| Shared 4-thread contention | 76.474 ns | 69.710 ns | -8.8% |
| GC allocation | 56.254 ns | 53.979 ns | -4.0% |
| Full collection | 6,393,800 ns | 5,464,300 ns | -14.5% |
| Mixed five-strategy loop | 109.906 ns | 99.784 ns | -9.2% |

Peak RSS:

- retained set: `45,158,400 -> 41,881,600` bytes (**-7.3%**);
- ordinary allocation: `9,494,528 -> 8,986,624` bytes (-5.4%);
- mixed: `10,809,344 -> 10,784,768` bytes (-0.2%).

**Honest regression:** release alone is slower. The prefix-free block is smaller,
and Windows heap free/coalescing cost rose for this 72-byte retained set. The
combined retained allocate+release total and peak memory still improved. A
release-dominated workload should be measured separately.

Arena, pool, shared and GC code was not changed. Their deltas are shared-path,
sample and host effects, not evidence of algorithm improvements. The collection
sample has a standard deviation of about `321,476 ns`.

No comparison to C++, Rust, Java or any other language is claimed.

---

## 7. Verification

Commands:

```text
./build/msvc-dev/tests/nirqoba_unit_tests.exe
python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe --configuration RelWithDebInfo
python tests/phase7_isolation.py
python tests/phase9_optimization.py
python scripts/compare_configuration_equivalence.py --compiler build/msvc-dev/bin/nirqoba.exe --filter '*gc*'
```

Results at `0.591-beta`:

- Unit: **332 passed**, 0 failed. Two allocator tests and one TIR test were
  added; the Phase 8 baseline was 329.
- Full runner: **624 passed**, 0 failed, 0 skipped. No Phase 1–8 case was
  removed or weakened.
- Phase 9 matrix: **10/10 fixtures** identical at O0/O2 with production and
  debug runtimes; O0 LLVM checks passed for release duplication and GC leakage.
- Phase 7 isolation: **passed**; ordinary paths have no `nrq_gc` IR/object
  symbols and the managed control remains non-vacuous.
- Configuration equivalence: **67 agreed, 0 skipped, 0 failed**.
- All five example entry points check successfully.
- Manual lifecycle, managed-payload and borrow fixtures still run; the Phase 8
  release-pairing regressions still reject wrong families and double release.

Not tested:

- foreign calls, because no FFI exists;
- every possible allocator callback failure interleaving;
- a statistically rigorous pause distribution for GC;
- release-dominated workloads beyond the retained-set benchmark.

---

## 8. Compatibility findings

| Area | Result |
|---|---|
| Ordinary/owned | No source change; same ownership and release checks |
| Borrow | No source change; direct access, no hidden allocation/refcount/GC |
| Arena/pool | No source change; lifetime and reset/destroy restrictions preserved |
| Shared | No source change; retain/release, overflow and resurrection protection preserved |
| GC | No source change; roots, barriers, safepoints and isolation preserved |
| Manual | No source change; function-local and explicit pairing preserved |
| Native resources | No source change |
| FFI | Still unsupported; documentation makes no foreign-safety claim |
| Examples | All check successfully; no outdated memory syntax found |

No legacy compatibility mode was introduced. No program lifetime or destruction
semantics were silently changed.

---

## 9. Limitations and unresolved risks

- Retained-set release latency regressed; combined cost and memory improved.
- Arena allocation still pays the global handle lookup/pin and the strategy
  serialization mutex. Removing them needs a generation/epoch lifetime protocol.
- Pool release still performs bucket and registry lookups; a single index was not
  justified by the measurements.
- Shared retain/release remains shard-lock protected. A lock-free fast path needs
  a stable control address or hazard protocol; it was not attempted.
- Manual storage remains function-local and cannot be reallocated.
- FFI remains unsupported.
- Peak RSS is process-wide and sampled at mode end.
- The dead-clear pass handles only the immediate-before-return shape; it is not a
  general dead-store optimizer.
- The 31-commit requirement could not be met: a user commit (`54eb267`) landed
  during implementation and folded the Phase 9 source, tests, benchmarks and
  audit into one commit alongside unrelated stress-test updates. Rewriting that
  commit would violate the instruction to preserve user commits. It was kept
  intact.

---

## 10. Remaining work

- Phase 10 CDMM, explicitly not started.
- A release-dominated benchmark to characterize the observed regression.
- An arena handle-cache protocol with safe destruction coordination, if future
  evidence justifies it.
- A pool pointer-to-bucket index, if release profiling justifies the metadata.
- A shared-control lifetime protocol, if contention measurements justify it.
- Any FFI design, only after an actual source ABI is specified.

---

## 11. Commit list

Because of the user commit, the Phase 9 history after `9280b37` is:

1. `54eb267` — user commit: "bench: rerun stress test 2". It contains the Phase 9
   runtime/compiler changes, tests, benchmark harness/results and audit, together
   with unrelated stress-test updates made by the user. It was preserved exactly.
2. `23ea723` — record Phase 9 version changes at 0.591-beta
3. `0feeb99` — document Phase 9 allocation, cleanup, and alignment behavior
4. `caa016e` — add the Phase 9 compatibility and migration guide
5. `bf8d0fd` — mark Phase 9 complete in the roadmap and migration plan
6. `a942b40` — add the Phase 9 progress entry
7. `396fbfe` — report the Phase 9 benchmark method and results
8. this report — Phase 9 completion report

No assistant co-author trailer was added. Nothing was pushed, published, released
or deployed. Phase 10 was not started.

The user's unstaged `.gitignore` change was preserved and not folded into any
Phase 9 commit. All Phase 9 source, test, benchmark and documentation files are
committed; no generated executable or object artifact is tracked.
