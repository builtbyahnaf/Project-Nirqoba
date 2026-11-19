# Nirqoba — Root-Slot Coalescing Validation (0.621-beta)

**Scope:** Correctness argument and verification evidence for the root-slot coalescing in
`compiler/lib/Lowering/TirToMlir.cpp` (`4d0f6c8`)
**Status:** Complete. Release and Dev agree; no failures.
**Date:** 2026-11-20

---

## 1. The change in one paragraph

`array.allocate` used to mint a synthetic root slot, publish the new object through it, and then
the surrounding declaration stored the same reference into the local's own rooted slot. Every
`var node = array.allocate<T>(1)` therefore opened two shadow-stack slots, cleared both in the
frame preamble, and wrote the reference twice. `coalesced_allocation_root()` now hands the
allocation the local's slot as its publish target when the declaration and its initialiser are
the next two TIR operations. The runtime entry points are unchanged; only the number of slots and
the number of publish stores change.

## 2. Eligibility rules, and why each one is required

The coalescing is refused unless every condition below holds. The negative tests in
`tests/unit/mlir_pipeline_test.cpp` cover the two rules that are easiest to get wrong.

| Rule | Why it is needed |
|---|---|
| The declaration is the operation immediately before the allocation, which initialises it | Anything else means the slot held an older reference first. |
| The slot is a managed-pointer alloca | The publish store writes a GC reference into the slot; a scalar slot has no such meaning. |
| The slot is in `gc_roots_.rooted_slots` (live across a collection point) | The root plan registers exactly those slots in the frame preamble, so the publish target is a registered pointer root on the fast path. A slot outside that set is only registered inside the cold allocation path, which is not where the fast path publishes. |
| The slot is not promoted by stack promotion | A promoted slot is not an alloca, and the frame does not describe it. |
| The declaration block is not a branch target | A back edge can enter the block with a reference already in the slot from an earlier iteration. The allocation's publish store is a plain store, not a write barrier, so it would replace that reference without the barrier's retain step. |
| The activation has a root frame (`only_cold_alloc_roots` is false) | With no frame there is no preamble to register the slot the fast path publishes into. |

## 3. Evidence

**Unit tests (`tests/unit/mlir_pipeline_test.cpp`, 475/475 in Release and Dev):**

- `CoalescesAContainerAllocationIntoTheLocalThatHoldsIt` — `array<1 x ptr>` frame, no
  `array<2 x ptr>`, allocation still goes through `nrq_gc_allocate_array_planned`, and the
  language store still goes through `nrq_gc_root_store` (publish-then-release ordering at a
  handoff is untouched).
- `KeepsASeparateRootSlotWhenTheLocalIsNotFresh` — a local assigned before the allocation keeps
  the call's own slot (`array<2 x ptr>`).
- `KeepsASeparateRootSlotWhenTheDeclarationIsALoopBody` — a declaration inside a loop keeps the
  call's own slot (`array<2 x ptr>`), because the block is a branch target.

**Integration suites:** 626/626 in Release and 626/626 in Dev, including the memory-model
suite. Programs that assert on the emitted root protocol were re-baselined to the new shapes:

| Fixture | Was | Now | Why |
|---|---|---|---|
| `gc_frame_early_return_cleanup` | 3 `root_frame_end`, 2 releases | 2 ends, 2 releases | one activation, one slot: the early-return path still pops exactly one frame |
| `gc_frame_roots` | `@nrq_gc_roots_4_15_…` | `@nrq_gc_roots_3_12_…` | four frame slots become three |
| `gc_fused_frame_open` | `@nrq_gc_roots_4_14_…` | `@nrq_gc_roots_2_8_…` | four slots become two |
| `gc_protocol_elision` | 2 `root_frame_open`, 0 `register_typed` | 0 open, 2 `root_frame_begin` + 2 `register_typed` | a single-root activation takes the begin/register shape instead of the batched descriptor |
| `gc_root_audit_control_flow` | 4 `root_frame_end` | 3 ends, 3 begins, 3 registrations | three activations, one slot each |
| `gc_root_completeness` | `@nrq_gc_roots_5_18_…` | `@nrq_gc_roots_3_12_…` | five slots become three |
| `gc_root_null_init` | 2 `store ptr null, ptr %` | 1 | the frame preamble clears the one rooted slot once |
| `opt_inlining` | no mention of `@build_node` | no `ptr @build_node(` | the call is still inlined; only the outlined `@build_node.cold.1` remainder is allowed to mention the name |

Every one of those programs was also built and executed directly before its expectations were
changed, and each prints exactly the stdout its fixture declares (`7`, `21`, `66`, `6`, `7`,
`35`, `7`, `6`).

**Configuration parity:** `scripts/check_runtime_check_parity.py` reports 150 invariant checks
present in both configurations, and `scripts/compare_configuration_equivalence.py` reports
266 agreed, 4 skipped, 0 failed against both the Release and the Dev compiler.

## 4. What this does not claim

The coalescing is a shape and instruction-count change, not a measured speedup: the interleaved
A/B put test 02 `BUILD_MS` at 21.5 -> 21.5 ms and `COLLECT_MS` at 32.0 -> 31.5 ms, both inside
this host's noise. No performance claim in this round rests on it.
