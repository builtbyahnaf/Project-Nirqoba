# BO-00 — Pre-BO Audit and Call-Site Map

**Status:** complete (audit only; no code change under this report)
**Version:** 0.275-beta (`126b4ef`), branch `main`, working tree clean apart from
the untracked local build helper `build269.bat`
**Build:** `build/msvc-dev`, production configuration
(`nirqoba_runtime.lib`, production ABI marker `nrq_runtime_abi_v1_production`
present in emitted IR), MSVC 19.51 x64, LLVM/MLIR 23.1.4
**Date:** 2026-11-09

Everything below is a **SOURCE** or **IR** fact read from this tree. No benchmark
was run for this report and no speedup is claimed.

---

## 1. Verdict on the Pre-BO section

Nothing in the Pre-BO section is a code change: it is an audit whose deliverable
is this report. This file did not exist before; the last full status narrative
(`bo-02-builder-status.md`) is dated 0.257-beta and records implementation, not a
current-HEAD call-site map.

What it establishes:

| Roadmap claim ("already present") | Current HEAD | Evidence |
| --- | --- | --- |
| Static compiler-emitted root descriptors | present | `@nrq_gc_roots_2_8_8_1_0_8_1_0` in Optimized IR of Stress 02 |
| Static registration entry point, production does not validate | present | `runtime/src/gc.cpp:3086` → `validate=nirq::runtime_debug_enabled` |
| `fuse_handoff_releases()` | present | `compiler/lib/Lowering/TirToMlir.cpp:1548` |
| `nrq_gc_root_store_release()` | present | declared `runtime/include/nirqoba/runtime/gc.h:92`, defined `runtime/src/gc.cpp` |
| Fusion actually fires on hot workloads | **no** | no `root_store_release` call in either emitted module (§3.2) |
| Runtime externs carry memory-effect contracts | **no (partly)** | `nrq_gc_root_register_frame_static` has no attribute group at all; `nrq_gc_root_store` is only `nounwind` (§3.3) |

So: the pieces BO-02/BO-03 would otherwise duplicate exist; BO-01 (memory-effect
contracts) is the real open item, and handoff fusion is an audit branch, not an
implementation branch.

## 2. Mandatory source audit (roadmap §1.3 items 1, 2, 5)

- Root-frame protocol: `nrq_gc_root_frame_begin` (:1497) and
  `nrq_gc_root_frame_end`; registration through
  `nrq_gc_root_register_frame_static` (:2084) in `TirToMlir.cpp`.
- Static descriptor shape is emitted as a private constant table and passed as
  the third argument; the slot array is a stack `alloca` (unavoidable: the slot
  addresses must be activation-specific).
- Root store path: `nrq_gc_root_store` / `nrq_gc_root_store_release`
  (`runtime/src/gc.cpp`), `nrq_gc_pin`/`unpin`/`pin_transfer` (:3565, :3663).
- Runtime representation: `runtime/src/slot_index.h`, `RootInfo` in `gc.cpp`,
  thread-local shadow stack, `roots` map, collector scanner. All still present
  and still referenced by reference diagnostics; nothing here is dead after the
  static-descriptor work.
- No uncommitted prototypes exist. The only untracked file is the local build
  helper.

## 3. Fresh IR evidence (roadmap §1.3 items 3–4)

Emitted with the shipping driver, `--emit=llvm-raw` and `--emit=llvm`, `-O3`:
`build/bo00/{02,03}_{raw,opt}.ll`.

### 3.1 Stress Test 2 / 02 `build_tree` — frame shape

Raw IR `build_tree` contains 12 call sites, exactly the historical count:

```
frame_begin, register_frame_static, allocate_array_typed_rooted, root_store,
build_tree, root_store, build_tree, unpin, root_store, unpin, pin_transfer, frame_end
```

Per-internal-node runtime calls (excluding the two recursive calls): **10**;
leaf path executes 6. Two-slot static frame, descriptor
`nrq_gc_roots_2_8_8_1_0_8_1_0`. Optimized IR keeps every one of these calls: the
runtime is a static archive without `/LTCG`, so nothing inlines across the
boundary. `main` also opens a frame and registers an 8-element typed array root
(`nrq_gc_root_register_typed`), so main/build_tree is the nested-frame case and
the dynamic-descriptor case in one module.

### 3.2 Stress Test 2 / 03 `build_cycles`

Three-slot static frame, descriptor
`nrq_gc_roots_3_11_8_1_0_8_1_0_8_1_0`, two `root_store` calls in the loop body.

### 3.3 Where the fusion does *not* fire

`fuse_handoff_releases()` folds `unpin(v)` immediately followed by
`root_store(dst, v)` into one call. The real IR orders these as

```
child1 = build_tree(...); root_store(dst1, child1); child2 = build_tree(...);
unpin(child1); root_store(dst2, child2); unpin(child2)
```

The pending unpin's operand is `child1` while the following store publishes
`child2`, so the adjacency test fails — correctly, because `child1` must stay
pinned across the second recursion. Neither emitted module contains a
`root_store_release` call. This is an audit finding for BO-03, not an
implementation gap to "fix" by weakening pin lifetimes.

## 4. The open item: runtime extern memory effects

`RuntimeAttributes.h` already classifies the non-GC runtime (math, print, fatal,
system queries). The GC protocol entries mostly get `nounwind` and, for some,
`willreturn`/`nocapture` — but **no `memory(...)` attribute**:

- 03 optimized IR: `declare void @nrq_gc_root_register_frame_static(ptr, i64, ptr)`
  — no attribute group at all;
- `@nrq_gc_root_store` → `#3 = mustprogress nounwind willreturn` only;
- `@nrq_gc_root_frame_begin` → `nounwind willreturn` only.

Without a `memory(...)` attribute LLVM must assume the call may read and write
any memory, including the caller's locals, which is exactly the historical
"null store before a root store survives" observation. This is BO-01 and it is
the only one of the roadmap's three sub-goals with no existing implementation.

## 5. Report reconciliation (roadmap §1.3 item 6)

- `reports/bottleneck_discovery/02-builder-bo.md`: historical ns figures and
  "12 call sites / ~10 runtime calls per node". The call-site and per-node counts
  are **confirmed** at HEAD by §3.1; the ns figures remain historical and are not
  reused here.
- `reports/performance/bo-02-builder-status.md`: its three claims (static
  descriptor, production skips validation, fused handoff entry point exists) all
  match current source. It is stale only as a status narrative (0.257-beta base).
- `reports/performance/java-gap.md` and the 0.266-beta material describe the
  defective tree; `docs/VERSION_LOG.md` records that 0.275-beta contains no code
  change since 0.269-beta's collector fix. Current source is authoritative.
- Nothing in those reports claims a memory-effect contract for the GC entries, so
  there is no conflicting claim to resolve for BO-01.

## 6. Baseline inputs for BO-01 acceptance

- Before-state IR/assembly for `build_tree` (`build/bo00/02_opt.ll` and its
  assembly) is the comparison point.
- The acceptance question is narrow: does a provably correct `memory(...)`
  contract remove memory traffic or spills at that hot call site? If not, that
  branch stops and is recorded as such.

## 7. Limitations

- No execution or timing measurement is part of BO-00; no current-HEAD ns number
  is asserted.
- The audit covers the two Stress Test 2 programs named by the roadmap plus a
  nested-frame/dynamic-descriptor case from the same module, not the whole test
  corpus.
