# Nirqoba — Which Way to the Next Test 02 Build Gain

**Scope:** the decision left open by `reports/performance/stress-test-2-0.621-beta.md` §4:
per-node instruction cuts, or avoiding the ~6 ms first-touch cost of the 64 MB tree
**Status:** decision recorded. Nothing in this report is implemented yet.
**Date:** 2026-11-19
**Compiler:** `nirqoba compiler version 0.621-beta (Phase 9)`, `build/msvc-release/bin/nirqoba.exe`
**Artifact under discussion:** `benchmarks/stress_test_2/nirqoba/02_mark_and_sweep.nrqba`,
BUILD median **20.0 ms** for 2,097,151 nodes (target < 22 ms)

---

## 1. The answer

**Per-node instruction and footprint cuts.** The zeroing line is not a separate lever: it is a
consequence of how many bytes the build touches, and the runtime already refuses to write zeros
it does not have to. There is no code change that removes the first-touch cost without touching
fewer bytes, so the only useful direction is the one that shrinks and shortens the per-node path
— which is also where the 8 ms of non-memory time sits.

Concretely, in order of expected gain:

1. **Stop padding every node by 8 bytes.** `TreeNode` is 24 bytes, but the allocation plan
   rounds the slot to 32 because `kPlanAlignment` is 16. That is 16 MiB of the tree's 64 MiB,
   and the measured cold-touch cost falls by ~2 ms if the padding goes.
2. **Cut the remaining per-node protocol instructions** — the per-object accounting stores, the
   four independent `nrq_gc_inline_config` loads per node, and the frame open/close that a
   single-slot activation still performs.

## 2. Why "avoid the zeroing" is not an approach

The ~6 ms in the 0.621-beta report is real, but it is not a `memset` that can be deleted. Three
facts settle it:

- **The storage is already zero and the runtime knows it.** A page's first pass writes no zeros
  at all: `GcPage::zero_cursor` (`runtime/src/gc.cpp:250`) tracks the offset up to which the page
  is known-zero, and every allocation splits its bytes into `prezeroed_bytes` and `filled_bytes`
  (`runtime/src/gc.cpp:3799`, `:4842`, `:5303`, reported by `nrq_gc_get_zeroing_facts`). Reclaimed
  space and recycled page shells set the cursor to `capacity` and pay the fill; fresh committed
  pages do not.
- **What remains is the kernel's demand-zero, charged per first touch.** Measured on this host
  with a standalone probe (needs no project code): a 64 MiB slab of 32-byte slots costs
  **10.87 ms** to touch cold against **5.10 ms** warm (medians of 3 runs; cold − warm = 5.99,
  5.47, 6.01 ms). The delta is the fault-plus-zero work for ~16 k first touches; the 5.10 ms
  that survives is the store traffic itself, which no allocator change removes.
- **The only way to skip faults is memory the process already owns.** Large pages
  (`MEM_LARGE_PAGES`) would cut ~16 383 faults to 32, but Windows requires the *Lock pages in
  memory* privilege, which an ordinary user account does not hold and a benchmark harness cannot
  assume. Pre-touching the heap before the timed region would be a benchmark shortcut, not an
  optimization.

So the "zeroing" bucket is 64 MiB × (fault + zero cost), and the only honest way to move it is
to make the tree smaller. That is the same edit as the footprint half of the per-node work.

## 3. Where the 20 ms actually goes

Decomposition for 2,097,151 nodes, using the probe above as the memory analogue and the emitted
code for the rest:

| Component | Time | Per node | Nature |
|---|---:|---:|---|
| Payload stores + the traversal of 64 MiB | ~5.1 ms | ~2.4 ns | memory traffic, floor for this shape |
| First touch of 64 MiB (fault + OS zero) | ~6.0 ms | ~2.9 ns | proportional to bytes, 16 MiB of it is padding |
| Per-node protocol and instruction issue | ~8.9 ms | ~4.2 ns | the part that is actually code |

The third row is larger than the headline number suggests, and it is the one with slack. What
`@build_tree` does per node today (`--emit=llvm`, 0.621-beta, `build/logs/bt.ll`):

- **Allocation is already inlined.** The fast path is a TLAB bump: load cursor and end, compare,
  advance by the 32-byte stride, write the site/epoch word, bump the two accounting counters, and
  store the object pointer into the frame slot. `nrq_gc_allocate_array_planned` survives only as
  the cold path. There is no runtime call to remove here.
- **The frame is a single slot and its open is inlined** — the shadow-stack push and its release
  store are straight-line code, and the batched descriptor is not used at one root. Good.
- **The publish is conditional:** a plain store when the inline-config bit is clear, otherwise
  `nrq_gc_root_store`. Note the *four* separate acquire loads of `@nrq_gc_inline_config` in one
  node's worth of code (open, publish, close, and once more on each merge), each with its own
  branch.
- **The close is a compound fast path** (`%87`–`%104`): config load, two TLS slot loads, three
  combines, a decision on whether `nrq_gc_pin_transfer` is needed, a frame-nesting compare, and
  the release store that pops the shadow stack.

Per-object accounting is two read-modify-writes per node, derived from the bump itself; a TLAB
refill already knows the object count and byte count it handed out, so those two updates can move
to the refill without any loss of exactness.

## 4. The footprint lever, with its risk stated

`alloc_hint.cpp:21` sets `kPlanAlignment = 16`, and `plan.slot_bytes = align_up(payload_bytes,
alignment)` with `alignment` raised to that floor (`runtime/src/alloc_hint.cpp:66`, `:93`). A
24-byte node therefore occupies 32 bytes, and the emitted bump confirms it (`add i64 %26, 32`).
The objects themselves are only 8-aligned (`kObjectStartAlignment = alignof(std::max_align_t)`),
so the extra 8 bytes buy no invariant the collector relies on.

| Slot stride | Tree footprint | Cold touch (3-run median) | Delta |
|---:|---:|---:|---:|
| 32 B (today) | 64.0 MiB | 10.87 ms | — |
| 24 B | 48.0 MiB | 8.83 ms | **−2.04 ms** |

Probe: 2,097,151 slots × stride, three stores per slot, `VirtualAlloc` + `QueryPerformanceCounter`,
3 runs each (`build/probe/touch_probe.cpp`, a scratch file under the git-ignored `build/`
directory — not project code, and its loop is an analogue of the runtime's touch pattern, not a
measurement of it). The medians move by ~2 ms; individual runs scatter by ±1 ms on this laptop.

This is the largest single number available and it is *not* free: honouring the object's own
alignment instead of the 16-byte plan floor changes a documented plan invariant (any payload that
genuinely needs 16-byte alignment — 128-bit integers, `long double`, SIMD records — must keep
requesting it), and it must be validated against the layout descriptors, the object-start bitmap
grid (`kObjectStartAlignment = 8`, so 24 is legal) and every 16-byte assumption in the emitted
bump. It is a design decision, not a clean-up, which is why it is written down here instead of
being slipped into the 0.621-beta round.

## 5. What this direction explicitly does not do

- No weakening of the sample validation (checksum `2199022206976`, live count `2097151`) and no
  shortening of the tree.
- No pre-touching, no reuse of a heap the harness warmed up outside the timed region, no
  page-size or privilege assumption.
- No accepting a `COLLECT_MS` regression to buy `BUILD_MS`; collection is at 27.0 ms against a
  35 ms target and every candidate above is measured against both.
- No claim that the two levers compose: the ~2 ms footprint gain and the instruction cuts overlap
  in the TLB and store-buffer pressure they relieve, so each one has to be measured on its own.

## 6. Evidence map

| Claim | Source |
|---|---|
| BUILD 20.0 ms median, 15 samples, targets met | `reports/performance/stress-test-2-0.621-beta.md` §1 |
| ~6 ms is first touch, not a `memset` | `build/probe/touch_probe.cpp`, 3 runs, this report §2 |
| The runtime skips zeroing fresh pages | `runtime/src/gc.cpp:250`, `:3799`; `nrq_gc_zeroing_facts` in `runtime/include/nirqoba/runtime/gc.h:428` |
| Allocation and frame open are already inlined | `build/logs/bt.ll` (`--emit=llvm` of 02_mark_and_sweep) |
| Slots are 32 bytes because the plan floor is 16 | `runtime/src/alloc_hint.cpp:21`, `:66`; `add i64 %26, 32` in `bt.ll` |
| Cold-touch cost per stride | same probe, this report §4 |
