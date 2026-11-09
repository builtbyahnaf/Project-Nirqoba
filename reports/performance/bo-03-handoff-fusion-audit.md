# BO-03 — Handoff Fusion Audit

**Status:** step 1 of 5 — audit; no code change yet
**Version:** 0.286-beta, production configuration
**Date:** 2026-11-09

Audit only: no second pass and no second runtime entry point were added.

## 1. Is the pass still invoked? (SOURCE)

Yes. `fuse_handoff_releases()` is called at the end of every function translation
(`compiler/lib/Lowering/TirToMlir.cpp:1536`), between the operation loop and
`attach_cold_blocks()` / `flush_gc_root_registrations()`, so it runs before the
frame registration is materialised and before LLVM sees the module.

## 2. Matching conditions and adjacency (SOURCE)

- A call to `nrq_gc_unpin(v)` with exactly one operand becomes the pending
  release (`:1576`) and is *removed* from the block.
- A following `nrq_gc_root_store(dst, v)` whose stored value is that same
  operand is replaced by `nrq_gc_root_store_release(dst, v)` (`:1585`–`:1600`).
- The pending release survives only non-call operations; any other call, any
  branch/label, and any terminator flushes it back at its original semantic
  point (`flush_before`, `:1555` and `:1572`). So the pass cannot cross a
  safepoint, a collection, an allocation or a control-flow join.
- A plain store may sit between the pair because it cannot observe or publish
  the pending managed value.

## 3. Does the current lowering ever emit that sequence? (SOURCE + IR)

**No.** Every managed handoff the lowering emits publishes first and releases
second:

- `root_managed_references_and_release_handoffs` (`:2167`): the direct
  pointer-root path emits `nrq_gc_root_store(slot, value)` at `:2196` and
  `nrq_gc_unpin(value)` at `:2233`; the layout-copy path emits `root_copy`
  followed by unpin per reference at `:2242`.
- The only other unpin site is an explicit `memory.unpin(x)` statement
  (`:3448`), which is emitted at the statement's position — after the store that
  published the value, not before it.

So the pass requires `unpin(v)` *then* `root_store(dst, v)`, and the lowering
produces `root_store(dst, v)` *then* `unpin(v)`. The sequence the pass matches is
never emitted.

IR evidence at this HEAD:

- Stress 01, 02, 03 (`--emit=llvm-raw`): zero `nrq_gc_root_store_release` calls;
  `build_tree` shows the store-then-unpin order (`root_store(%21, %18)` …
  `unpin(%19)`).
- Probe program `memory.unpin(a); obj.field = a;` (built only in `build/bo00`):
  the store is still emitted before the unpin, and no fused call appears.

Consequence: the pass is invoked but cannot match current output. The runtime
entry it would emit is exercised by no benchmark or test in the repository.

## 4. Correctness of the entry itself (SOURCE)

`nrq_gc_root_store_release` (`runtime/src/gc.cpp:3619`) is
`if (slot) nrq_gc_root_store(slot, value); unpin_value(value);`:

- **Root visible before pin release:** yes, the store completes first.
- **Null handling:** a null slot releases only the pin, a null value makes the
  store path no-op, both identical to the split calls.
- **Pin deltas / buffering:** `unpin_value` is used unchanged, so the transfer
  delta buffer and its drain order are identical.
- **LIFO / frames:** the entry touches neither frame records nor markers.
- **Barrier ordering:** see §5 — this is where the existing comment is wrong.

## 5. The barrier claim is wrong (SOURCE)

`runtime/include/nirqoba/runtime/gc.h:88-92` says emitting the store and the
release separately costs "two call boundaries and two barrier regions", implying
the fused entry costs one. It does not:

- `nrq_gc_root_store` opens a fast-barrier region and closes it (`:3172`);
- `unpin_value` opens its own region (`:3636`) and closes it.

The fused entry calls both, so it still enters **two** barrier regions; what it
removes is one **call boundary** (and its argument setup). The comment must state
the call boundary as the benefit and not claim a merged region. The same claim is
repeated in the lowering comment above `fuse_handoff_releases`
(`TirToMlir.cpp:1543-1547`), which also says the pair "costs two call boundaries
and two barrier regions" — true as a description of the split form, but it implies
the fused form is one region.

## 6. Decision

- Correct the comments (step 2). Keep the runtime entry and the pass: removing a
  documented public ABI entry is an owner decision, and deleting a pass that is
  invoked but currently unmatchable is also a behaviour-neutral cleanup that this
  audit is not authorised to make silently.
- Cover both directions as required (steps 3–4): semantics of the fused entry
  directly (the only reachable way to exercise it), and an integration assertion
  that the compiler keeps emitting store-then-unpin and does not fuse.
- No measurement of a "pre-fusion binary" is taken, because no corpus program
  emits the fused form; the roadmap allows skipping measurement in exactly this
  case.
