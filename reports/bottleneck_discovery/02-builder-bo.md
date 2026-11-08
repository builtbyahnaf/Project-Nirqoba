# 02 — BO: Object Construction and Root Management

**Domain:** Builder Optimization (per-object construction protocol)
**Verdict:** The builder protocol, not the bump pointer, is the dominant
construction cost. One `build_tree` activation emits **10 runtime calls**, and
opening/publishing/closing a two-slot root frame alone costs **63 ns** per
activation — 33 % of the measured 193 ns/node build cost in test 02. The
previously planned `pin_transfer`/`unpin`/`root_store` fusion (working-tree
prototype) is worth **< 2 %** because the runtime functions it merges are already
2–3 ns each.

---

## 1. The emitted construction sequence [IR/ASM]

Compiled with the current 0.266 compiler, `-O3`, from
`benchmarks/stress_test_2/nirqoba/02_mark_and_sweep.nrqba`.
IR: `build/inv/02_stdout.ll` (lines 91-122). Assembly:
`build/inv/02_masm.txt` (from `02_mark_and_sweep.obj`, `dumpbin /disasm:nobytes`).

`build_tree` is 68 instructions with 12 call sites:

| # | Emitted call | Per | Purpose |
|---|---|---|---|
| 1 | `nrq_gc_root_frame_begin()` | node | open activation root frame |
| 2 | `nrq_gc_root_register_frame_static(slots, 2, descriptor)` | node | publish 2 root slots |
| 3 | `nrq_gc_allocate_array_typed_rooted(&slot, 1, 24, layout, 2)` | node | allocate + assign to root |
| 4 | `nrq_gc_root_store(&slot, node)` | node | root the node itself |
| 5 | `nrq_gc_unpin(left)` | internal | release callee's transfer pin |
| 6 | `nrq_gc_root_store(&node->left, left)` | internal | publish child |
| 7 | `nrq_gc_unpin(right)` | internal | release callee's transfer pin |
| 8 | `nrq_gc_root_store(&node->right, right)` | internal | publish child |
| 9 | `nrq_gc_pin_transfer(node)` | node | hand node to caller, pinned |
| 10 | `nrq_gc_root_frame_end(marker)` | node | close frame, release slots |
| — | 2 × recursive `build_tree` | internal | the actual work |

Leaves execute 6 of these calls; internal nodes 10. Traversal is *not*
affected: `-O3` turns `sum_tree` into a loop of 29 instructions with one
self-call and **zero** runtime calls (`sum_tree` in `02_masm.txt`), so GC root
overhead no longer explains traversal time. This corrects the older
`java-gap.md` framing.

### Generated-code defects visible in the assembly [ASM]

```
sub rsp,50h                     ; 80-byte frame
mov [rsp+30h],0                 ; slot table: 5 stores (2 slots + addresses)
mov [rsp+38h],0
lea r14,[rsp+38h]
mov [rsp+40h],r14
lea r15,[rsp+30h]
mov [rsp+48h],r15
...
mov [rsp+30h],0                 ; <-- dead store: the next call overwrites this slot
mov rcx,r15
mov rdx,rax
call nrq_gc_root_store
```

The second `mov qword ptr [rsp+30h],0` is dead: `root_store` receives the slot
address and the value and overwrites it. LLVM cannot remove it because
`nrq_gc_root_store` is an opaque external call that may read the slot. It is one
store per node (~0.5 ns), small but symptomatic of the cross-TU boundary.

---

## 2. Measured construction cost [MEASURED]

Per-entry-point probe against HEAD, 1 M operations:

| Operation | ns/op | Comment |
|---|---:|---|
| frame begin + frame end, no slots | 16.4 | vector push/pop of `Frame` + TLS |
| root register + unregister, 1 slot | 32.2 | `SlotIndex` insert/erase + `pointer_roots` insert/forget |
| **frame begin + register 2 slots + end** | **63** | what every `build_tree` activation pays |
| frame begin + register 4 slots + end | 97.1 | ≈ 21 ns per additional slot |
| full test-03 pair (2 objects, frame, 4 stores) | 170.6/pair (85.3/object) | matches test-03 build |

Test-02 build: 2,097,151 nodes in 291–757 ms across 8 runs (median 366 ms),
i.e. **139–361 ns/node**; the best runs are ~193 ns/node (406 ms in a quiet
run). Composition of the ~193 ns:

| Component | ns/node | Share |
|---|---:|---:|
| frame begin + register + end | 63 | 33 % |
| allocation (cold page-fault-inclusive) | 54 | 28 % |
| 3 × `root_store` | 4.8 | 2 % |
| `pin_transfer` + 2 × `unpin` | ~10 | 5 % |
| recursion, payload store, slot-table stores, cache misses | remainder | ~32 % |

The frame protocol is the largest single software component of construction and
the only one that is pure protocol.

### Why frame registration costs 63 ns [SOURCE]

`register_frame_roots()` (`gc.cpp:2903-3030`) performs, for every activation:

1. `ensure_shadow_stack()` (TLS check);
2. `begin_fast_barrier()` (relaxed store + fences + phase load);
3. `stack.roots.reserve(size + slot_count)` (branch + potential reallocation);
4. per slot: construct a `RootInfo` (32-byte `ReferenceLayout`, copying offsets
   into inline storage), `push_back(ShadowRoot{slot, RootInfo})`, then
   `add_slot_state()` → one `SlotIndex` hash insert **plus** one
   `pointer_roots` insert;
5. `end_fast_barrier()`.

In **production** the descriptor validation loop is gone
(`nrq_gc_root_register_frame_static` passes
`validate = nirq::runtime_debug_enabled`, `gc.cpp:3039-3042`), which is the BO-02
change and is real. What remains is the per-slot metadata copy and two hash-table
inserts. For a frame whose slots the compiler already described statically, all
of that metadata is known at compile time.

### Transfer pins [SOURCE+MEASURED]

`pin_transfer` and `unpin` no longer touch a global pin table per call: they
buffer signed deltas per mutator (`gc.cpp:3616+`, `kTransferPinBufferCapacity =
32`) and the collector folds them at the next cycle. Measured pair cost 5.1 ns.
This is a real improvement over the behaviour described in the stale
`GC_COST_INVESTIGATION.md` (which claimed TLAB publication per return); that
claim no longer holds for 0.266.

---

## 3. Root management inventory [SOURCE]

| Mechanism | Implementation | Cost |
|---|---|---|
| Root frame open/close | `gc.cpp:3194-3260`; `std::vector<Frame>` push/pop | 16 ns |
| Batched static registration | `gc.cpp:2903-3042` | ~21 ns/slot |
| Slot index | open addressing, `slot_index.h`; `SlotState` holds a `ReferenceLayout` | ~32 ns register+unregister/1 slot |
| Pointer-root memo | second index on the same slot address | part of above |
| Batched frame-end release | `remove_frame_slots()`, ≤ 32 distinct slots per batch | included in close |
| Root store | lock-free when `collection_phase == Idle`; otherwise barrier region | 1.6 ns |
| Root copy (aggregate) | `memcpy` + pointer-word buffering under barrier | not on test path |
| Root scan at cycle | walks every shadow stack under `stack.mutex` | part of `begin_cycle` |

The root scanner at cycle start is O(live roots). For test 02, after the build
returns, only `main`'s frame remains, so it is trivial — which is why
`begin_cycle` is 0.7 ms in test 02 but **11.4 ms in test 03** (it must publish
1 M TLAB objects; see `03-collector-colo.md` §5).

---

## 4. The compiler/runtime boundary is the root cause of the residual protocol

**[SOURCE]** Nirqoba optimises the entire user program as one LLVM module
(`compiler/lib/Backend/Optimization.cpp:196-200`), but the runtime is a
**prebuilt static library** linked by `lld-link` with no `/LTCG`
(`compiler/lib/Driver/CompilationPipeline.cpp:405-441`; `toolchain.json`
`linker: lld-link.exe`). The runtime archive is built by MSVC CMake without
`/GL`. Therefore:

* no inlining of `nrq_gc_root_store`, `nrq_gc_pin_transfer`, or the allocation
  fast path into generated code;
* no memory-effect reasoning beyond the LLVM attributes on the declarations
  (which is why the dead null store survives);
* no elimination of redundant barrier regions across a call boundary;
* every runtime operation pays a call, register spills, and an optimisation
  barrier.

This is a *design* fact with a measurable shadow, and it explains why BO's
call-count reductions have produced smaller gains than the call counts suggest.

---

## 5. Status of the previous BO work

| Item | Status | Evidence |
|---|---|---|
| Compiler-generated frame descriptors | Implemented | `TirToMlir.cpp:2050-2093`, `nrq_gc_roots_*` globals |
| Static registration entry point | Implemented | `TirToMlir.cpp:2082`, `gc.cpp:3039` |
| Production skips descriptor validation | Implemented | `validate = runtime_debug_enabled` |
| Single-probe `add_slot_state` | Implemented | `gc.cpp:2673-2690`; `insert` returns existing state |
| Batched frame-end release | Implemented | `remove_frame_slots`, `kMaxBatchedSlotRemovals = 32` |
| Descriptor memo for validated path | Present, **production-dead** | `thread_validated_descriptors` only runs when `validate` is true |
| Flat open-addressing slot index | Implemented | `slot_index.h` |
| Reduced validation in production | Implemented, but **not measured** | BO-02 explicitly claims no benchmark |
| Fuse `unpin`+`root_store` (J3) | **Uncommitted, unbuilt** | working-tree diff, §6 |

**[PRIOR REPORT]** BO-01 claimed a "thread-local validated descriptor cache" was
the fix for per-activation validation. It exists, but production no longer calls
the validating entry point at all, so the memo is irrelevant to production
performance. BO-01's profiling numbers (50 cycles per `root_store`, 104 cycles
per handoff) also no longer match the current runtime: `root_store` is 1.6 ns
(≈5.6 cycles) and `pin_transfer+unpin` is 5.1 ns (≈18 cycles) because the
barrier/pin-table paths were rewritten afterwards. **BO-01's cost model must not
be reused.**

---

## 6. The uncommitted J3 fusion — honest evaluation

The working tree adds:

```cpp
void nrq_gc_root_store_release(void** slot, void* value) {
  if (slot) nrq_gc_root_store(slot, value);
  unpin_value(value);
}
```

and a lowering pass that replaces adjacent `unpin(x)` + `root_store(slot, x)`
with one call. Assessment:

* It removes **one call boundary** per handoff: ~2–3 ns.
* It does **not** merge barrier regions: `nrq_gc_unpin` does not open one; it
  only adjusts a buffered pin delta. The header comment's claim of "two barrier
  regions" is inaccurate.
* It cannot help collection at all.
* On test 02 it applies twice per internal node → ~4–6 ns out of 193 ns =
  **2–3 % of build**, ~1 % of total runtime.
* It is unbuilt, so even that figure is a projection. It was not applied or
  reverted here.

**Recommendation:** land it only as part of a batch of builder changes, with a
before/after measurement; do not expect it to move a benchmark.

---

## 7. Ranked BO candidates

| Rank | Candidate | Expected impact | Risk | Dependency |
|---|---|---|---|---|
| B1 | Production frame fast path: register a static descriptor pointer and slot array without per-slot `RootInfo`/`SlotIndex` copies (compiler guarantees layout) | frame 63 → ~25–30 ns; 15–20 % of build [PROJECTION] | High (root-scan invariants, diagnostics) | must keep debug path validating; unit tests for nested frames |
| B2 | Inline TLAB bump + barrier fast path in emitted code, runtime slow path | 11–15 ns/object (~6–8 % of build) | High (ABI, compiler) | A2 |
| B3 | Eliminate the dead null store / slot-table preamble where provable | ~1–2 ns/node | Low | lowering liveness proof |
| B4 | `root_store_release` fusion (J3) | 2–3 % of build | Low | land with measurement |
| B5 | Intern root layouts by descriptor pointer (BO-02's 80 B → 40 B per root) | reduces frame registration copy cost | Moderate | stable descriptor arena |
| B6 | Single-pass object initialization (remove zero-fill for fully initialized objects) | small for 24 B payload; larger for wide structs | High (language semantics) | definite-initialization analysis |

B1 is the highest-value builder change, but it is also the one most likely to
break GC correctness if the runtime stops being able to reconstruct a slot's
layout for a diagnostics or mid-cycle registration path. It needs a design that
keeps one authoritative descriptor and a debug-validated path, not a second
source of truth.
