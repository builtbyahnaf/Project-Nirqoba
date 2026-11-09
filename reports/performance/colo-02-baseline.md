# ColO-02 — Baseline and Source Audit (ColO-00)

**Roadmap:** `project_context/ColO_ROADMAP_2.md` §1
**Base commit:** `c9fcce1` (working build; parent `fa799e8`, investigation)
**Branch:** `main`
**Version under test:** 0.269-beta (working build; 0.266-beta is defective)
**Date:** 2026-11-09
**Status:** baseline established; no production behaviour changed by this stage.

---

## 1. Repository state

```
git rev-parse HEAD           c9fcce1601d6ed51593bf880acd93c30915f198d
git branch --show-current    main
git status --short           (clean before this stage; .gitignore was modified,
                              unrelated, left untouched)
```

Toolchain: MSVC 19.51.36231 (`vcvars64.bat` at the path recorded in
`toolchain/toolchain.json`), LLVM/MLIR 23.1.4, CMake/Ninja build directory
`build/msvc-dev`, linker `lld-link` (no `/LTCG`).

The build **must** run inside `vcvars64`; invoking `cmake --build` from a plain
shell fails with `C1083: cannot open include file 'vector'` because `INCLUDE`
is not set. This is environment, not source.

---

## 2. Source audit (current code, not the investigation's copy)

| Item | Current location | State |
|---|---|---|
| `sweep_chunk_locked` | `runtime/src/gc.cpp:1589-1760` | **Per-bit loop confirmed** at 1695-1702: `if (((bits >> bit) & 1) == 0) { ++bit; continue; }` |
| `SweepCursor` | `gc.cpp:1580-1587` | `{page, page_limit, word, bit}`; reset to `{}` at sweep start (`gc.cpp:2268`) |
| `for_each_live_object_locked` idiom | `gc.cpp:687-710` | `countr_zero` + `bits &= bits - 1` |
| Start bitmap creation/mutation | `gc.cpp:236-255` (`set/clear/test_object_start`) | one bit per `alignof(max_align_t) = 8` bytes |
| `object_starts` allocation | `gc.cpp:1925-1937` | `new uint64_t[words]()`, `words = ceil(capacity/8/64)` |
| `object_span_bytes` | `gc.cpp:359-363` | `round_up(96 + payload, 8)` |
| `release_span_locked` | `gc.cpp:1768-1810` | ordered insert, O(≤96) tail shift, bridge merge |
| `take_free_run_locked` | `gc.cpp:1857-1913` | first fit over `span_pages` |
| `finish_cycle_locked` | `gc.cpp:2187-2230` | statistics, policy, phase → Idle |
| minor/major selection | `gc.cpp:2140-2160` | `plan_next_cycle_locked`, `active_collection_is_minor` |
| sweep-phase-2 list | `gc.cpp:1718-1745` | `sweep_list` = `unindexed_objects`, frozen at 2268 |
| slice budgets | `gc.cpp:925` | `kMarkSliceBytes = 64 KiB`, `kSweepSliceBytes = 256 KiB` |

**Conclusion:** the source matches the investigation's reported per-bit loop.
No reapplied patch is needed; the correction has not landed.

### Two facts that constrain the fix

1. **The mask can be snapshotted safely.** Mutators may set new start bits
   between sweep slices, but any such object has
   `birth_epoch >= active_cycle_epoch` and is skipped explicitly
   (`gc.cpp:1710-1713`). Only the sweeper clears start bits, and only for bits
   it has already consumed. Therefore persisting `bits` for the current word
   cannot skip a bit that must be processed, and cannot process a cleared bit.
2. **Phase 1 and phase 2 share the byte budget.** `touched` is incremented in
   both loops; any new word budget must gate phase 1 only (phase 2 has no
   bitmap), and phase 2 must remain reachable after phase 1 completes.

---

## 3. Baseline measurement protocol

* Harness: `benchmarks/bottleneck_discovery/gc_harness` built from the runtime
  sources of the exact revision (`git archive`, all runtime `.cpp` except the
  unlinked socket file), `/O2 /DNDEBUG /std:c++20 /EHsc /MD`,
  `NRQ_RUNTIME_DEBUG_ENABLED=0`.
* Modes: `02` = 2,097,151-node live tree, 41 collections/process; `03` =
  1,000,000-node dead graph, 8 collections/process.
* 8 independent process runs per mode, interleaved by revision where more than
  one revision is compared; min/median/max reported; raw files preserved.
* End-to-end: Stress Test 2 `COLLECT` medians from the same session, one full
  suite run per revision.

## 4. Baseline result

The pre-fix baseline is the committed 0.266 matrix (same collector code; 0.269
differs only in version string and the builder-side fusion, which does not run
in the collector):

| Mode | collections/run | min | median | max | zero-bit iters/col |
|---|---:|---:|---:|---:|---:|
| 02 | 41 | 192.2 | **224.9** | 239.7 | 28,651,521 |
| 03 | 8 | 57.0 | **60.2** | 84.5 | 10,040,814 |

Stage split (3-run medians, patched copy of `gc.cpp`): 02 begin 0.69 / mark
66.59 / sweep 96.57 ms; 03 begin 11.38 / mark 0.01 / sweep 31.77 ms.

Reproduce with:

```
powershell -File benchmarks/bottleneck_discovery/gc_harness/build_and_run.ps1 -Rev <rev>
powershell -File benchmarks/bottleneck_discovery/gc_harness/run_matrix.ps1
python benchmarks/bottleneck_discovery/analyze.py
```

The 0.269 build's runtime is code-identical to `904aee0`'s (only the docs/compiler
fix `43c223f` sits in between, and the harness links only the runtime tree), so the
baseline numbers above are used directly for ColO-01 rather than re-run. The
ColO-01 report records the measured result against them.

---

## 5. Gates carried into ColO-01/02

From `ColO_ROADMAP_2.md` §8, restated against the actual baseline:

| Gate | Target |
|---|---|
| 02 COLLECT median (8 runs) | ≤ 140 ms |
| 03 COLLECT median (8 runs) | ≤ 40 ms |
| 02 sweep (stage probe) | ≤ 35 ms |
| 03 sweep (stage probe) | ≤ 12 ms |
| zero-bit iterations | 0 |
| build / traversal | no regression beyond run spread |
| correctness | full unit + integration suite, both configurations |
