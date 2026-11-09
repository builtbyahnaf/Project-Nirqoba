# BO-04 — Redundant Root Initialization and Loop Frame Reuse (audit)

**Status:** step 1 of 5 — audit; the removal lands in the next step
**Version:** 0.291-beta, production configuration
**Date:** 2026-11-09

## 1. Redundant root initialization: confirmed, and where it comes from

`build_tree` emits **two** null stores into each rooted managed slot.

Raw/optimized IR at HEAD (Stress 02, production):

```
store ptr null, ptr %5        ; frame preamble (flush_gc_root_registrations)
store ptr null, ptr %4        ; frame preamble
...
call i64 @nrq_gc_root_frame_open(ptr %3, i64 2, ptr @nrq_gc_roots_2_…)
call ptr @nrq_gc_allocate_array_typed_rooted(ptr %5, i64 1, i64 24, …)
store ptr null, ptr %4        ; <- alloca-time zero store (Alloca translation)
call void @nrq_gc_root_store(ptr %4, ptr %8)
```

Two emitters write the slot:

1. `flush_gc_root_registrations` (`TirToMlir.cpp:1821`) clears every pending root
   slot in the frame preamble, before the frame is published. This store is
   required: a collected slot must not hold a stale pointer when the frame
   becomes visible.
2. The `Alloca` translation (`TirToMlir.cpp:2396`) emits its own zero store for
   every managed local, and *also* registers that slot as a root. For a rooted
   slot the second store repeats the first.

## 2. Is the second store removable? (proof against the roadmap's conditions)

- **Slot known null at that point:** yes. The slot is a fresh entry-block
  `alloca_at_entry`; the only earlier write to it is the frame-preamble zero
  store, which dominates every later operation in the entry block.
- **No safepoint can observe the difference:** a collection between the preamble
  and the alloca store (an allocation can trigger one) already sees null, and the
  slot holds null until the first published value, so removing the repeat changes
  no observation.
- **Every reachable path initialises it before any root scan:** the preamble runs
  unconditionally at frame open on every activation path; there is no early exit
  before it, and the frame is opened before any user operation in the function.
- **Exceptions, early returns, loops, joins:** none of them reach the slot before
  the preamble; the slot is entry-block allocated and cannot be created on another
  path.
- **LLVM memory attributes do not carry the proof:** this is a structural proof in
  the lowering (dominance plus the absence of writes), not a claim about what a
  runtime call may read. The store is currently kept because LLVM cannot prove
  `frame_open`/`allocate_array_typed_rooted` do not write through the slot
  addresses they are handed.
- **Precondition checked in the lowering:** the elimination applies only when the
  frame preamble is guaranteed to run, i.e. the function has a root frame and the
  slot is registered as a root. Otherwise the alloca-time zero store stays.

No global suppression of root initialisation is involved: unobserved and
unregistered slots keep their deterministic zero store, and the preamble store is
untouched.

## 3. Loop frame reuse: already the behaviour (nothing to hoist)

The lowering opens a frame once per **function activation** in the prologue
(`TirToMlir.cpp:1489-1502`); no path emits frame begin/registration inside a loop
body. IR confirms it: `main` in Stress 02 runs three loop nests (5 × collections,
5 measurement rounds) with one `frame_open` at entry and one `frame_end` at the
return.

Therefore "root-frame reuse across loop iterations" has nothing to remove: the
frame is not re-opened per iteration in the first place. Per the roadmap's
optional-research note, no hoisting or reuse transformation is implemented — the
proof obligations (break/continue/early exit/collection points/threads) would
apply to a transformation that does not exist here.

## 4. Work chosen for this stage

1. Remove the alloca-time zero store for slots that the frame preamble already
   clears, with the precondition above, and record the IR/assembly effect.
2. Cover it with an integration case that counts the zero stores per activation,
   so a regression that reintroduces the duplicate is caught.
3. Leave loop-frame handling and the preamble untouched.
