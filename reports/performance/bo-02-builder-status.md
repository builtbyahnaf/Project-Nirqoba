# BO-02 — Builder Path Status Report

**Status:** In progress
**Domain:** Builder Optimization (Pre-Migration Performance Track)
**Date:** 2026-11-07
**Base:** 0.251-beta released, working version 0.257-beta
**Target Architecture:** Windows x64 (MSVC 19.51)
**Measurement status:** no benchmark run was executed for this report. Everything
below is a code or emitted-IR fact. No speedup is claimed anywhere.

---

## 1. Configuration policy this work follows

The two runtime configurations are now separate paths, and the owner's policy is:

- the **development** build may carry heavy diagnostics and be slower than
  production - that cost is accepted;
- the **production** build must not pay for anything only the development build
  observes, and must keep full speed.

Every change below is therefore written so that the production path loses work
while the development path keeps, or gains, its checks.

---

## 2. What changed in this pass

### 2.1 Frame descriptors the compiler built are no longer re-validated

BO-01 recorded an `O(slots x references)` validation walk over a table that the
compiler had generated itself, on every activation, and a memo that could only
recover part of that cost. The lowering's table is a constant private global
(`nrq_gc_roots_*`) whose words are derived from the very slots the function is
about to publish, so the table cannot disagree with the frame - a compile-time
property, not a runtime one.

- New entry point `nrq_gc_root_register_frame_static`. The development
  configuration validates and memoises the table exactly as before; the
  production configuration contains neither the walk nor the memo, because
  `register_frame_roots` takes the decision as a compile-time constant.
- `nrq_gc_root_register_frame` keeps validating every caller table; that is the
  entry point a program using the C ABI must use, and nothing about it changed.
- The lowering emits the static entry for its own tables. Seven integration cases
  that assert the registration call in their emitted IR were updated.
- A unit test registers through the static entry and checks the roots keep their
  referents alive across a collection, resolve through the same slot index as the
  rooted-allocation path, and release at frame end.

### 2.2 Socket runtime unlinked

The socket runtime had been implemented ahead of its planned phase. It left the
runtime source list and the link line, and its unit test left the test binary. The
source, header and test remain in the tree for the phase that owns them.

---

## 3. Already in the tree before this pass

- **Descriptor memo soundness.** The memo requires identical descriptor words, so
  a reused stack address cannot inherit a verdict.
- **Barrier regions.** `begin_fast_barrier`/`end_fast_barrier` use a relaxed store
  on the mutator's own cache line plus an acquire-release fence.
- **Batched frame registration.** Two or more activation roots are published with
  one call, with the descriptor carrying the layouts.
- **One barrier region per batch** instead of one per root.

---

## 4. Remaining work, in order

| Item | Effect | Precondition before it can land |
|---|---|---|
| Open the frame and publish its slots in one call | one call, one barrier region and one mutex check fewer per activation | the emitted open and registration must be adjacent; deferring the open needs its own argument |
| Drop the standing null preamble where the slot is already known null | two stores per iteration in the construction loops | per-slot "known null" tracking, and proof that no collection point sits between the zeroed allocation and the store |
| Intern root layouts by descriptor pointer (80 B → ~40 B per root) | the largest remaining allocation-independent traffic in T02/T03 | layouts must outlive every registration; the slot index rehashes, so this needs a stable arena |
| Fuse `pin_transfer`, `unpin` and `root_store` for a returned value | T02 handoff, the item BO-01 priced highest after frame costs | a new runtime entry plus a lowering rule proving the three calls are one handoff |
| Single-pass object initialization | removes the redundant zero-fill for fully initialized objects | definite-initialization analysis; the zero-fill is language behaviour today |
| Descriptor-pointer object header (96 → 48 B) | largest single win on T02/T03 build | belongs to AO Stage 4; not duplicated here |

---

## 5. Evidence attached to this pass

- Build: succeeds, including the configuration-parity gate (92 invariant checks in
  both configurations).
- Unit: 188/188. Integration: 259/259.
- Configuration equivalence on the corpus after the provenance change: 119 agreed,
  2 skipped, 0 disagreed.
- Emitted IR: the GC corpus now calls `nrq_gc_root_register_frame_static`; the
  prologue of `build_tree` in the T02 workload shows the static registration and
  no descriptor walk call.

---

## 6. What is deliberately not claimed

- No timing improvement, because no benchmark was run (owner authorization is
  required, and the Stage-0 harness repair in BO-01 has not been executed).
- No statement about the T02 collection drift; it remains open for ColO-01 and
  needs a trustworthy baseline first.
