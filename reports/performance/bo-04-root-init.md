# BO-04 — Redundant Root Initialization: result and loop frame reuse

**Status:** complete (5 steps)
**Version:** 0.294-beta, production configuration (`NRQ_RUNTIME_DEBUG_ENABLED=0`)
**Labels:** MEASURED / SOURCE / PROJECTION; no HYPOTHESIS is used as evidence.

## 1. What changed

`compiler/lib/Lowering/TirToMlir.cpp`, `Alloca` translation: a rooted managed
slot no longer receives its own zero store at the alloca point, because the frame
preamble already clears it before publishing the frame (audit:
`bo-04-root-init-audit.md`). The store is still emitted when the function has no
root frame, i.e. whenever the preamble is not guaranteed to run. Unregistered
(unobserved) slots keep their deterministic zero store unchanged.

## 2. Effect (MEASURED)

Raw IR, Stress 02, `build_tree` — the duplicate before `nrq_gc_root_store`:

```
- store ptr null, ptr %4, align 8      (removed)
  call void @nrq_gc_root_store(ptr %4, ptr %8)
```

| metric (`build_tree`, llc `-O2`) | before | after |
| --- | --- | --- |
| instructions | 55 | 54 |
| zero store in the body | `movq $0, 48(%rsp)` | — |
| full-module instructions (excluding `.seh`) | 452 | 450 |

`SOURCE`: `build_tree` runs once per node, 2,097,151 activations in the Stress 02
workload, so the removal deletes 2,097,151 eight-byte stores (≈16 MiB of store
traffic) per run. `PROJECTION`: that fraction of a runtime dominated by
allocation and collection is not detectable, and **no runtime speedup is claimed**
— the measurement above is an instruction/traffic count, not a timing.

## 3. Safety argument (recap, SOURCE)

- The frame preamble store is unconditional on every activation path, and it runs
  before the frame is published; the alloca-time store was the only write to that
  slot before it, and the slot is a fresh entry-block alloca, so the preamble store
  dominates it.
- No safepoint observes a difference: the slot is null from the preamble until the
  first published value either way.
- Unobserved and unregistered slots still receive the zero store; nothing relies on
  LLVM memory attributes, so no runtime declaration was weakened.

## 4. Loop frame reuse

Not implemented, because there is nothing to transform: the lowering opens one
root frame per **activation** in the prologue, and no path opens or ends a frame
inside a loop body. `SOURCE`: `TirToMlir.cpp:1489-1502` (prologue frame) with no
other frame-create site; the new `gc_loop_frame_once` test pins it — a loop body
that allocates, collects and holds a managed local opens and ends exactly one
frame. Per the roadmap, hoisting/reuse across iterations was optional research and
requires a proof this stage did not need, since frames are not re-opened per
iteration.

## 5. Verification (MEASURED)

- Unit suite: 194/194 pass.
- Integration suite: 263/263 pass, including two new cases:
  - `positive/gc_root_null_init`: one rooted local plus one allocation-sink root →
    exactly two `store ptr null` in the module (four under the old duplicate);
    prints `7`.
  - `positive/gc_loop_frame_once`: one `frame_open`, one `frame_end`, two preamble
    stores for a loop body with a managed local; prints `10`.
- Stress 02 and 03: built and run in production configuration; every semantic
  `KEY/VALUE` line (excluding `*_MS`) is identical to the BO-00 baselines.

## 6. Limitations

- No timing measurement; the change is justified by removed work and the structural
  proof, not by a measured speedup.
- The test asserts IR store counts, so it can only fail if the duplicate (or a
  missing preamble store) returns; it does not observe runtime root visibility,
  which is covered by the existing fused-open and handoff-order unit tests.
