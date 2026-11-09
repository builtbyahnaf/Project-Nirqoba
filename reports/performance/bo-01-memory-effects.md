# BO-01 — Runtime Memory-Effect Contracts (result)

**Status:** complete — contracts landed; **no instruction-level codegen change**;
optimization branch stopped as the roadmap requires
**Version:** contracts landed under 0.275-beta, verification under 0.281-beta
**Configuration:** production (`NRQ_RUNTIME_DEBUG_ENABLED=0`), MSVC 19.51 x64,
LLVM/MLIR 23.1.4
**Date:** 2026-11-09

## 1. What changed

Six steps, each committed:

1. `reports/performance/bo-01-contracts.md` — every emitted GC entry point
   classified from its implementation (argument dereferences, TLS/global state,
   allocation, collection, pin state, callbacks, failure path).
2. `root_frame_begin`/`root_frame_end` → `memory(readwrite, argmem: none,
   errnomem: none, target_mem: none)`.
3. Root publication and registration (`root_store`, `root_store_release`,
   `root_copy`, `root_register`, `root_register_typed`, `root_register_frame`,
   `root_register_frame_static`, array-root registration) → explicit
   `argmem: readwrite` contract; `root_unregister`/`unpin` → `argmem: none`.
4. Managed allocators → `argmem: none`; `allocate_array_typed_rooted`, `pin`,
   `pin_transfer` → `argmem: readwrite`.
5. Mutator protocol, `collect`, `collect_minor`, `init`, `shutdown`, policy
   switches and statistics queries → `argmem: none`.
6. This verification.

`other` and `inaccessible` memory remain `ModRef` on every entry in the group.
No GC entry point is claimed side-effect free, and none is newly marked
`willreturn`, `readonly` or `writeonly`.

## 2. Method (MEASURED)

Two compilers from the same tree, differing only in
`compiler/include/nirqoba/Lowering/RuntimeAttributes.h`:

- **A** = header at `126b4ef` (before this stage);
- **B** = header at `b85b18f` (after steps 2–5).

For Stress Test 2 / 01, 02, 03: `--emit=llvm -O3` from each compiler, then
`llc -O2 -filetype=asm`. Only the contract table differs between A and B.

## 3. Result

| Module | A asm lines | B asm lines | instruction diff | attribute/unwind diff |
| --- | --- | --- | --- | --- |
| 01 managed allocation | 516 | 489 | **0** | `.seh` directives only |
| 02 mark and sweep | 478 | 453 | **0** | `.seh` directives only |
| 03 cyclic reclamation | 540 | 512 | **0** | `.seh` directives only |

Instruction streams are byte-identical once the `.seh` directives are excluded:
`diff (A minus .seh) (B minus .seh)` is empty for all three modules. Spill and
reload counts are therefore unchanged at every call site, including
`build_tree`, which was the representative hot site the roadmap named.

The declaration attributes do appear as intended. Example (02, optimized IR):

```
declare void @nrq_gc_root_register_frame_static(ptr, i64, ptr) #2
declare ptr  @nrq_gc_allocate_array_typed_rooted(ptr, i64, i64, ptr, i64) #2
declare void @nrq_gc_root_frame_end(i64) #5
declare void @nrq_gc_safepoint() #4
#2 = nounwind memory(readwrite, errnomem: none, target_mem: none)
#4 = nounwind memory(readwrite, argmem: none, errnomem: none, target_mem: none)
#5 = mustprogress nounwind willreturn memory(readwrite, argmem: none, errnomem: none, target_mem: none)
```

## 4. The one observable change, and why it is not a win

With the contracts in place, the existing function-attribute inference can now
finish its analysis of the generated functions and attaches a `nounwind` +
memory-attribute group to `build_tree`, `main` and `sum_tree`. An LLVM `nounwind`
function without `uwtable` does not need SEH unwind directives, so `llc` stops
emitting them for those functions:

- removing just that one attribute group from the B module restores all 11
  `.seh` directives, confirming the cause;
- in the linked production binary the `.pdata` section shrinks from `0x114c` to
  `0x1134` (24 bytes), while every instruction is unchanged.

Instructions are unaffected; the difference is unwind metadata for functions
LLVM has proven cannot throw. It is recorded rather than presented as a gain, and
no attempt was made to force a larger effect by broadening attributes.

## 5. Correctness checks (MEASURED)

- Unit suite after step 5: **192/192 passed**.
- Stress 02 and 03 built and run in the production configuration with the B
  compiler; every semantic line is identical to the pre-change binary
  (`CHECKSUM`, `LIVE_AFTER`, `COLLECTIONS_DELTA`, `CYCLES_CLEARED`,
  `ROOT_VALUE`, per-round `LIVE_AFTER`), timing lines excluded.
- Declaration effects are identical in development and production builds: the
  policy table is shared and configuration only selects the ABI marker symbol,
  which is visible in IR as `nrq_runtime_abi_v1_production`.

## 6. Decision

Per the acceptance rule: no measurable removal of memory traffic or spills
occurred, so this branch **stops here**. The contracts stay because they
document the real ABI and they are correctness-neutral; they are not presented as
a performance improvement, and no workload timing was taken for them.

## 7. Limitations

- Assembly was produced with `llc -O2` on the emitted final IR, not with the
  driver's own object emission; the instruction-level comparison is valid
  because both sides use the same backend invocation.
- Only the two Stress Test 2 programs named by the roadmap plus Stress 01 were
  compared, not the whole corpus.
- No timing was measured, by design: with identical instruction streams there is
  nothing to time.
