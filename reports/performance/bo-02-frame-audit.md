# BO-02 — Static Frame Registration and Descriptor Cost (audit)

**Status:** step 1 of 8 — audit; no code change yet
**Version:** 0.281-beta, production configuration
**Date:** 2026-11-09

## 1. What already exists (do not re-implement)

| Roadmap item | State at HEAD | Evidence |
| --- | --- | --- |
| Immutable compiler-emitted descriptor | implemented | `frame_root_descriptor()` emits a private constant table; IR shows `@nrq_gc_roots_2_8_8_1_0_8_1_0` |
| Trusted vs validated split | implemented | `nrq_gc_root_register_frame_static` (production does not walk the table) vs `nrq_gc_root_register_frame` (validates every call) |
| Debug validation of the trusted descriptor | implemented | `register_frame_roots(..., validate=nirq::runtime_debug_enabled)`; memoised per thread by address **and contents** |
| Batched single registration call | implemented | lowering emits one `nrq_gc_root_register_frame_static` for frames with ≥ 2 roots |
| Per-slot hash indexes removed from the frame path | partly | `SlotIndex` is open addressing with tombstones fixed (PO-27); a `PointerRootCache` answers the allocation path without a probe |
| Frame begin and static registration as one call | **not implemented** | IR shows two adjacent calls; see §3 |

The descriptor-validation memo cannot be a production fast path: production never
enters the validating function, so there is nothing to optimise there (recorded
already in `bo-02-baseline.md`).

## 2. Work that remains per activation (source + IR, not timed)

For a two-slot frame such as `build_tree`:

| Operation | Count per activation | Where |
| --- | --- | --- |
| runtime calls for the frame protocol | 2 (`root_frame_begin`, `root_register_frame_static`) | IR §3 |
| frame record push/pop | 1 / 1 (`ShadowStack::frames`) | `gc.cpp:3241`, `:3258` |
| `ShadowRoot` pushes (slot + `RootInfo`) | 2, inline offsets, no heap allocation | `register_frame_roots` → `push_root` |
| `SlotIndex` insert / batched release | 2 / ≤ 2 probes (batched by slot) | `add_slot_state`, `remove_frame_slots` |
| pointer-root memo inserts | 2 (`PointerRootCache`) | `add_slot_state` |
| descriptor walk | production: no validation; one pass to copy layouts | `register_frame_roots` |
| per-slot heap allocation | none for ≤ 2 reference offsets (`ReferenceLayout` inline storage) | `gc.cpp:261` |

So the remaining structural cost is not metadata copies: the per-slot
representation is already allocation-free for ordinary layouts and the index is
open-addressed. What remains avoidable is the **split frame protocol**: two
runtime calls, each re-entering `ensure_shadow_stack()` and each taking its own
barrier region, for one activation.

## 3. Emitted protocol at HEAD

`build_tree` (production IR): `root_frame_begin` → slot null stores → slot-array
stores → `root_register_frame_static` → allocation/pins → `root_frame_end`.

The two calls are adjacent only in the sense that nothing observes the frame
between them: `root_frame_begin` only pushes this thread's frame record
(marker + root depth). The collector never reads `stack.frames`; it reads
`stack.roots` under `stack.mutex`, and no root exists between the two calls.

## 4. Chosen work for this stage

1. **Fuse frame creation and static registration** into one runtime entry point
   returning the marker. Ordering to preserve: frame record first, then roots
   published; the combined call is emitted where registration already sits, so
   the slot null stores still precede publication; a failure before publication
   terminates the process, so no partially registered frame can ever be observed.
   Zero- and one-slot frames keep `root_frame_begin` + the direct registration
   call: the batched array would cost more than it saves at that size.
2. Keep `SlotIndex`, `RootInfo`, `pointer_roots` and the dynamic entry point
   exactly as they are: reference diagnostics and the allocation path depend on
   them, and the roadmap requires that they are not removed.

Not attempted, with reasons:
- descriptor-backed root iteration / compact frame ranges: requires rewriting the
  collector's root scan to chase a descriptor per slot; a likely net loss at these
  slot counts and a large correctness surface.
- eliminating the frame vector: the marker is the LIFO token; deriving it from
  root depth would let a stale marker match a sibling frame at the same depth and
  weaken the LIFO check.

## 5. Acceptance for this stage

- One runtime call per activation instead of two for batched static frames.
- Identical semantic output for Stress 02/03 and 192/192 unit tests.
- No change to dynamic/external registration behaviour, LIFO enforcement, debug
  validation, or the public C ABI already documented in `gc.h`.
