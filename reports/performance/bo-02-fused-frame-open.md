# BO-02 — Fused Frame Open (result)

**Status:** complete
**Version:** runtime entry and lowering landed under 0.281-beta; tests and evidence
under 0.286-beta
**Configuration:** production (`NRQ_RUNTIME_DEBUG_ENABLED=0`), MSVC 19.51 x64,
LLVM/MLIR 23.1.4
**Date:** 2026-11-09

## 1. Change

`nrq_gc_root_frame_open(slots, slot_count, descriptor)` pushes the frame record
and publishes the frame's static roots in one call. The lowering emits it for
batched static frames and erases the eager `nrq_gc_root_frame_begin` call,
rebinding the marker every already-emitted `nrq_gc_root_frame_end` reads.

Ordering preserved, argued from the implementation rather than assumed:

- The frame record is pushed first, exactly as before; it contains only this
  thread's marker and root depth, and the collector never reads `stack.frames`.
- Roots are published second, after the compiler's slot-null stores and the slot
  address array (the call sits where registration already sat).
- A registration failure takes the runtime fatal path and terminates before the
  caller resumes, so a half-open frame cannot become observable.
- Single-slot and zero-slot frames keep `root_frame_begin` plus the direct
  registration call: the batched array would cost more than the call it removes.

Dynamic/external registration (`nrq_gc_root_register_frame`) and the trusted
static entry point are unchanged; the new entry has the same descriptor
precondition the static one documents.

## 2. Measured effect (MEASURED)

Stress 02 `build_tree` (two-slot static frame), production configuration:

| Quantity | Before | After |
| --- | --- | --- |
| runtime call sites in the function (raw IR) | 12 | **11** |
| runtime calls per internal node (excl. 2 recursive) | 10 | **9** |
| runtime calls on the leaf path | 6 | **5** |
| `build_tree` assembly instructions (`llc -O2`, excl. `.seh`) | 56 | **55** |

The assembly delta is exactly the removed call and its marker move:

```
-  callq  nrq_gc_root_frame_begin
-  movq   %rax, %rsi
...
-  callq  nrq_gc_root_register_frame_static
+  callq  nrq_gc_root_frame_open
+  movq   %rax, %rsi
```

No spills or reloads were added; the rest of the function is identical.

Stress 01 emits two fused opens and keeps three split begins (its single-root and
rootless frames), which is the intended split.

No wall-clock timing was taken. The change removes one runtime call and one
shadow-stack re-entry per batched activation; that is the claim, nothing more.

## 3. Correctness coverage (MEASURED)

| Roadmap coverage item | Where |
| --- | --- |
| Nested and recursive frames | `GcTest.FusedFrameOpenPublishesStaticRootsAndNests`; integration `gc_fused_frame_open` (recursive, 4 roots/activation) |
| Zero, one, two, many slots | zero: fused test's empty frame; one: split-path tests (`gc_root_audit_*`); two: `build_tree`, `StaticDescriptorRegistrationMatchesTheDynamicPath`; many: `FrameRootRegistrationHandlesWideMixedLayouts` (16 slots) |
| Mixed pointer/reference layouts | `FrameRootRegistrationHandlesWideMixedLayouts` |
| Early return | existing `gc_root_audit_control_flow` (multiple returns, frame end per return) |
| Dynamic/external descriptor registration | `FrameRootRegistrationPublishesEverySlot`, `StaticDescriptorRegistrationMatchesTheDynamicPath` |
| Concurrent collection during publication | existing ColO suite and GC tests; the fused entry reuses the same `begin_fast_barrier` region the registration path already used |
| Root-frame LIFO and slot lifetime | `RootFramesUnregisterOnlyTheirOwnNestedRoots` plus the fused nesting test; LIFO violation remains a runtime fatal and is not exercised by a unit test (a fatal ends the process) |
| Debug-mode malformed descriptor rejection | unchanged: the fused entry passes `validate = nirq::runtime_debug_enabled`, exactly like the static entry point |

Suites: unit 193/193; integration 259/260, the single failure being
`memory_advanced`, which already failed before this version (its expectation was
last updated at 0.260, it expects a `0.266-beta` version literal, and it aborts in
the memory-mapping path, unrelated to frame registration).

Semantics of Stress 02 and 03 remain identical to their pre-change binaries on
every non-timing line (`CHECKSUM`, `LIVE_AFTER`, `COLLECTIONS_DELTA`,
`CYCLES_CLEARED`, `ROOT_VALUE`).

## 4. Not done, with reasons

- **Descriptor-backed root iteration / compact frame ranges** (roadmap 3.2):
  would rewrite the collector's root scan to chase a descriptor per slot. At two
  to four slots per frame that is more indirection per scan than the metadata it
  removes, and it is a large correctness surface. Not attempted.
- **Removing the frame vector**: the marker is the LIFO token. Deriving it from
  root depth would let a stale marker match a sibling frame at the same depth and
  weaken the LIFO check.
- **Per-slot metadata copies**: already allocation-free for the ordinary case
  (`ReferenceLayout` keeps two offsets inline) and the slot index is
  open-addressed with the PO-27 tombstone policy; nothing measurable remained
  there.
- **SlotIndex / pointer_roots removal**: explicitly out of bounds per the
  roadmap; the allocation path resolves pointer roots through them.

## 5. Limitations

- Instruction counts come from `llc -O2` on the emitted final IR, not from the
  driver's own object emission; both sides use the same backend invocation.
- No timing was measured, so no speedup percentage is claimed.
- The runtime library is prebuilt and not inlined into generated code, so the
  fused entry's internal savings (one shadow-stack re-entry, one barrier region)
  are argued from the implementation, not measured in isolation.
