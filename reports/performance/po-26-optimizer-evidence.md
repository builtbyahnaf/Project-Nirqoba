# PO-26 — Optimizer Transform Evidence (PO-7 acceptance)

**Task:** PO 7 — Advanced IPO, loop, vectorization, and inspection support
**Date:** 2026-11-06
**Status:** Completed for the transforms listed below; one item deferred with a
stated reason
**Compiler:** Nirqoba 0.181-beta (dev), LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo

---

## 1. Summary

The compiler lowers TIR through MLIR and then hands an LLVM module to
`llvm::PassBuilder::buildPerModuleDefaultPipeline` at the selected optimization
level (`compiler/lib/Backend/Optimization.cpp`). Every transform this task names —
unreachable-block elimination, inlining, interprocedural optimization,
loop-invariant code motion, unrolling, loop strength reduction, induction-variable
simplification, SLP vectorization and loop vectorization — is provided by that
pipeline.

The task file states the required response explicitly: *"Do not implement
duplicate versions of these optimizations merely because their names appear in
this roadmap. Instead, investigate whether Nirqoba's generated IR gives LLVM
enough information to perform them effectively."* No hand-written pass was added
for any of them; instead each is mapped to an artifact, and the two places where
the generated IR limits LLVM are recorded as limitations.

## 2. Transform Matrix

| Transform | Owner | Evidence artifact | Verdict |
| --- | --- | --- | --- |
| Unreachable-block elimination | LLVM (SCCP, SimplifyCFG, GlobalDCE) + MLIR cleanup | `tests/integration/positive/opt_unreachable_code` — asserts the folded branch and the helper reachable only through it are both absent from the optimized module | Implemented + tested |
| Function inlining | LLVM inliner (per-module default pipeline) | `tests/integration/positive/opt_inlining` — scalar and GC-registering helpers both inlined, loop preserved so the result is not just full unrolling | Implemented + tested |
| Interprocedural optimization | LLVM (IPSCCP, inliner, function attrs); Attributor behind `--ipo=aggressive` | `compiler/lib/Backend/Optimization.cpp` pipeline construction; `--ipo` modes covered by `optimization_remarks` and PGO fixtures | Implemented; aggressive mode opt-in |
| Loop-invariant code motion | LLVM LICM | `tests/integration/positive/opt_licm` — asserts the hoisting remark for arithmetic computed inside the loop | Implemented + tested |
| Loop unrolling | LLVM loop unroller (cost-model driven) | Observed: a fixed-trip-count loop over a managed array is fully unrolled before vectorization decides; `opt_vectorize_owned` covers the vectorized counterpart | Implemented; heuristics are LLVM's cost model |
| Loop strength reduction | LLVM LSR | Exercised through the vectorized trip-count loop in `opt_vectorize_owned` (`%index.next`, `%n.vec`) | Implemented; covered indirectly |
| Induction-variable simplification | LLVM IndVarSimplify | Same artifact as above (`%index = phi i64 [0, %vector.ph], [%index.next, %vector.body]`) | Implemented; covered indirectly |
| Loop vectorization | LLVM LoopVectorize | `tests/integration/positive/opt_vectorize_owned` — requires `vector.body` and `%n.vec` in the optimized module | Implemented + tested |
| SLP vectorization | LLVM SLPVectorizer (default pipeline) | **Deferred:** no dedicated artifact. Straight-line probes either fold to constants or are unrolled first, so a stable assertion would depend on LLVM's cost model rather than on this compiler. The failure mode is a missed optimization, never a miscompile | Deferred with reason |

## 3. What the Generated IR Does and Does Not Allow

Two limits are properties of the code this compiler emits, not of the pipeline:

1. **Managed loops carry a safepoint poll.** Every back edge of a function that
   registers roots tests the countdown and calls `nrq_gc_safepoint`. A call the
   collector can run through cannot be vectorized, and its conservative memory
   effects block LICM of loads that the loop does not write.
   `tests/integration/positive/opt_vectorize_managed_blocked` records the
   behaviour: the poll is present and no vector body exists.

   Removing the poll would bound how long a mutator can go without observing a
   collection request, which is a liveness property of the collector, not an
   optimization. Weakening the call's memory model would be a correctness risk:
   the safepoint runs marking and sweeping, so claiming it cannot touch
   program-visible memory needs an aliasing proof that the current heap layout
   does not provide.

2. **Owned arrays carry no protocol at all.** Loops over `memory.owned_array`
   vectorize, which is the evidence that the emitted IR is otherwise good enough
   (no address-space mismatches, no opaque pointer arithmetic in the loop, no
   missing trip-count information).

## 4. Test Runner Support

`tests/run_tests.py` now evaluates assertions on the module that leaves the LLVM
pipeline in addition to the pre-optimization module:

| Key | Meaning |
| --- | --- |
| `expect_optimized_ir_contains` | Substring must appear in `--emit=llvm` output |
| `expect_optimized_ir_not_contains` | Substring must not appear |
| `expect_optimized_ir_counts` | Exact substring multiplicity |

`expect_ir_*` continues to run on `--emit=llvm-raw`, which is what lowering
emitted; the two channels are deliberately separate, because "the lowering
produced a vectorizable shape" and "LLVM vectorized it" are different claims.

## 5. Verification

- `./build/msvc-dev/tests/nirqoba_unit_tests.exe` — 156/156.
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` —
  249/249.
- Evidence is IR-level and remark-level, not wall-clock, per the project policy
  for this task.

## 6. Remaining Opportunities

1. **Safepoint-free vectorizable managed loops.** A loop that allocates nothing
   and is provably bounded could run a poll-free inner body with a poll in the
   preheader and latch, preserving the observation bound while giving the
   vectorizer a call-free body. This needs a bound the compiler can prove.
2. **Aliasing evidence for the safepoint call.** A dedicated address space for
   collector metadata, or `noalias` guarantees on payload pointers, would let the
   safepoint be modelled without blocking LICM.
3. **An SLP artifact.** Once a stable straight-line shape exists, add a fixture
   that asserts packed stores or `shufflevector` in the optimized module.
4. **Deliberate always-inline policy** for helpers that only marshal managed
   values, so a caller's root plan is visible to the optimizer in one function.
5. **Remarks as a gate.** `--remarks=missed` output could become an opt-in CI
   report so regressions in vectorization are visible without reviewing IR.
