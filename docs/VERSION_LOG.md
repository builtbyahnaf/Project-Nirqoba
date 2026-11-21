# Nirqoba Version Log

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

I, Muhammad Tahmid Ahnaf (@builtbyahnaf), am the sole author and owner of Nirqoba and
hold sole authority over its direction, architecture, language design, and every
technical or project decision. No change to the language, compiler, runtime, toolchain,
documentation, or process is authorized without my explicit approval, and every release
recorded below is made under that authority.

> Note: The changes before `0.102-beta` were massive, so they were intentionally left out here.

---

## Version 0.637-beta

### Summary of Changes (since `0.633-beta`)

Phase 9 opens with Phase 10.10: the compiler now reports the memory decisions it
makes on its own, states the facts those decisions rest on to LLVM, and skips the
allocations that cannot be observed. Release label `+0.004` (`0.633-beta` ->
`0.637-beta`) per the three-upgrade rule.

#### CDMM diagnostics

- `--inspect-cdmm` reports every allocation site to stderr: its source location,
the binding that holds it, the layout it allocates, the selected strategy
(`stack`, `pool`, `arena`, `gc`) and the fact that selected it.
- The decision is recorded during TIR translation, where the lifetime analysis and
the choice are both in hand, and reported by the strategy audit pass, which is the
single place an automated decision becomes visible. The carrier attribute is
removed as it is printed, so the diagnostic never reaches the emitted program.
- Rationales use the analysis vocabulary: `bounded to function`, `loop-scoped`,
`promoted to the stack`, and the five escape reasons.

#### LLVM optimization compatibility

- Runtime allocator returns now carry `noalias` and an explicit alignment
(`nrq_memory_allocate`, `nrq_memory_allocate_from`, `nrq_cdmm_arena_allocate`,
`nrq_cdmm_pool_allocate`). A fresh allocation cannot overlap anything the caller
can already name, and its base is a multiple of the payload's alignment.
- The attributes are stated on the allocator declarations, which is where LLVM
keeps a callee's return attributes: every call site inherits them.

#### Dead-allocation elision

- An allocation nothing reads, whose pointer stays in the activation and whose
payload needs no destructor, is removed from the TIR before any strategy claims
it: no allocator call, no arena, no release.
- Elision reuses the evidence selection already computes (bounded lifetime, no
local use, plain payload), and it removes the obligation along with the storage,
so the function no longer claims a destruction it does not perform.

#### Call-site arena promotion

- The lifetime classifier is now interprocedural in one direction: a call whose
returned storage the caller keeps in its own frame is marked *caller-bounded*.
- A callee that returns fresh storage gains a trailing arena handle as soon as one
call in the module bounds the result; the caller creates the arena in its entry
block, passes it, releases the chunk on the way out, and no longer releases the
returned binding.
- The handle is nullable, so the other callers do not have to agree: an escaping
site passes null and the callee allocates that storage from the general allocator
at runtime. One signature serves every call site, and the decision stays local to
the caller.
- The CDMM report names every half: the callee's site reports `arena (returned to
bounded and escaping callers, arena handle checked at runtime)`, a bounded call
site reports `arena (bounded to the caller's frame, filled by <callee>)`, and an
escaping one reports `gc (escaping call site, null arena handle passed to
<callee>)`.

#### Dynamic strategy dispatch

- Promotion is per call site now. A callee that returns fresh storage always gains
  the trailing arena handle when at least one caller bounds the result; every call
  supplies that argument, with null standing for "the storage leaves this frame".
- The callee branches on the handle at the allocation it returns: a real handle
  fills the caller's arena, null falls back to `nrq_memory_allocate`. One
  signature therefore serves both kinds of caller, and an escaping caller no
  longer denies the arena to the bounded ones.
- The classifier reports a callee with both kinds of call site as partially
  promoted, and the CDMM report names the escaping call site and the null handle
  it passes, so both halves of the dispatch are reviewable.

#### Loop-scoped interprocedural arenas

- A promoted call site whose storage is created and consumed inside one loop now
  gets a loop-scoped arena: created on the loop's entry edge, released on its exit
  edges, so a long-running loop's peak is one iteration's work instead of
  everything the frame ever allocated.
- The call-site classifier reuses the allocation classifier's loop nesting
  (`LoopNesting`), so a call site and a local allocation in the same loop describe
  the same span; the narrowest loop holding the call and every use of its result
  wins, and everything else keeps the frame's arena.
- The create carries `nirqoba.region_scope = "loop"` (frame regions say `"frame"`),
  and `--inspect-cdmm` reports `arena (bounded to the loop, filled by <callee>)`.

#### Documentation

- `docs/memory-model/cdmm.md` gains the reporting vocabulary, the elision rule,
the promotion rule and the attributes the optimizer is given.

---

## Version 0.629-beta

### Summary of Changes (since `0.625-beta`)

Owned text became a type the language can write, and the recursive element
destruction contract was written down before the code that will implement it.
Release label `+0.004` (`0.625-beta` -> `0.629-beta`) per the three-upgrade rule.

#### `string_owned`

- The kind is distinct from `string` but shares its representation: it mangles as
  `ptr`, so it has the same ABI and reads, measures and prints like text, while the
  checker can attach a destructor to it and the TIR verifier can accept `Owned`
  ownership for it. A borrowed `string` still cannot carry `Owned`.
- `file.read_text_owned` returns `string_owned` and its cleanup is
  `file.free_text`, so a lexical scope releases the buffer without the programmer
  naming it. `nrq_file_text_live_count` makes the release assertable: three
  thousand reads in a loop leave zero live buffers.
- `string_owned` is spelled in source as a type keyword, so a binding, field,
  parameter or return type can name it.
- Binding an owned string to a borrowed `string` is rejected (`NRQ3015`), which is
  what keeps the two kinds from sharing a release path they do not have.

#### Recursive element destruction contract

- `ownership-representation.md` states the rules the container work will be held
  to: iterate the active element count rather than capacity, drop elements in
  reverse initialization order, and drop every element before releasing the
  container's backing buffer.

#### Known gap

- An owned value passed to an owning parameter is not destroyed by the callee:
  only `let` and assignment declarations carry a cleanup in TIR, and no parameter
  cleanup is emitted. The parameter symbol carries the right ownership, but the
  transfer is not yet observable, so no test claims it.

---

## Version 0.625-beta

### Summary of Changes (since `0.621-beta`)

Region accounting became auditable and the collector gained a pacemaker. Release
label `+0.004` (`0.621-beta` -> `0.625-beta`) per the three-upgrade rule.

#### Region-Utilisation Pacemaker (Phi)

- `nrq_gc_set_region_limit` and `nrq_gc_set_trigger_percent` declare a region
  budget and the fraction of it a program is willing to spend before a cycle
  starts. With no budget (limit `0`, the default) the collector's previous
  allocation-volume trigger decides exactly as before, so a program that never
  calls the new setters sees no behavioural change.
- A cycle that ends with the live heap at or past the budget is planned as a major
  collection: a minor cannot reclaim old garbage, so it could not bring the heap
  back under the bound.
- `nrq_gc_root_frame_open` polls at a safepoint: one relaxed load and a compare,
  with a cycle started only when the accounting says one is due. Allocation entry
  points already poll, so a TLAB refill never polls from inside the heap lock.

#### Region Accounting Audit, in Both Configurations

- Every cycle now ends by deriving each `GcRegion`'s live object, payload byte and
  mark totals from its own pages, then checking the linkage those totals came
  from. A page naming a region outside the pool, a region slot that does not name
  its page, an out-of-range slot, bytes with no live objects, or more marks than
  objects are all fatal.
- The audit was debug-only; a release build now runs it too, because a drift that
  only appears in release is exactly the kind the audit exists to catch. Region
  totals were previously reset and never incremented, so the audit is also what
  makes them exact.

#### The Start-Bitmap Hazard Is Closed by Construction

- `start_bits_for_scan_locked` is the single way a scan reads a page's start
  bitmap. It materialises deferred publication runs first, so no walk can travel a
  page whose live objects are still recorded as runs — the failure mode that turns
  a missed object into a reclaimed one. Reading the bitmap directly for a walk is
  no longer the path of least resistance.

#### Telemetry Honesty

- `GcRegion::evacuation_candidate` was removed: it was assigned `false` in two
  places and never read.
- `survivor_regions`, `evacuation_threshold_percent`, `evacuated_regions`,
  `evacuated_objects`, `evacuated_bytes` and `pinned_region_fallbacks` are
  documented as reserved and always zero, so the published struct no longer
  implies capabilities the runtime does not have.

#### Evacuation (Tau) and Bulk Reclamation (Upsilon) Were Deliberately Not Built

Deferred region-backed allocation already brought sweep time to well under a
millisecond — the phase that motivated the region design was solved without
moving a single object. Copying would have required forwarding that the headerless
(H0) and SoA layouts cannot physically hold, plus reference rewriting across roots,
shadow frames, remembered sets and the object index. Measured upside did not
justify that risk, so the phases are closed rather than pending.

---

## Version 0.621-beta

### Summary of Changes (since `0.617-beta`)

Closed out the `0.617-beta` test-02 build attribution: a fresh container
allocation now publishes straight through the local that holds it, and the
benchmark harness records the host/toolchain metadata the last round was
missing. Release label `+0.004` (`0.617-beta` -> `0.621-beta`) per the
three-upgrade rule.

#### Root-Slot Coalescing for Initialising Allocations

- **`compiler/lib/Lowering/TirToMlir.cpp`:** `array.allocate` minted a synthetic
  root slot and then the declaration stored the result into its own rooted slot,
  so every `var node = array.allocate<T>(1)` opened two shadow-stack slots,
  cleared both, and wrote the reference twice. `coalesced_allocation_root()`
  now reuses the local's slot when the declaration and its initialiser are the
  next two TIR operations, the slot is a managed-pointer alloca that the root
  plan already keeps live across a collection point, the slot is not promoted,
  and its block is not a branch target (so no earlier iteration left an older
  reference there that the plain publish store would not retain). Every other
  allocation keeps its synthetic slot.
- Effect on emitted IR: the stress test 2 `build_tree` frame goes from
  `[2 x ptr]` to `[1 x ptr]`, and single-root activations take the
  `nrq_gc_root_frame_begin` + `nrq_gc_root_register_typed` shape instead of the
  batched `nrq_gc_root_frame_open` descriptor. Multi-root frames keep the
  batched shape with one slot fewer (`@nrq_gc_roots_4_15_8_1_0_8_1_0_8_1_0_16_2_0_8`
  -> `@nrq_gc_roots_3_12_8_1_0_8_1_0_16_2_0_8` in `gc_frame_roots`,
  `4` -> `2` in `gc_fused_frame_open`, `5` -> `3` in `gc_root_completeness`).
- Measured effect: neutral. Interleaved A/B over 10 runs each put test 02 build
  at 21.5 -> 21.5 ms and collection at 32.0 -> 31.5 ms, i.e. inside the noise of
  this host. The change is kept for the one removed publish store/barrier and
  the smaller frame, not as a headline win, and the same A/B rejected the
  `OptimizerLastEP` `HotColdSplittingPass` experiment outright, so that
  experiment was reverted.

#### Benchmark Harness Metadata

- **`benchmarks/stress_test_2/run_benchmark.py`:** `environment_metadata()`
  unpacked `run_command()`'s three-value `(code, output, timed_out)` result as
  two values, so the metadata block was silently dropped from every run. Fixed
  the unpacks, the per-compiler version probe, and the broken `try`/`except`
  around the java probe. Results now record the CPU model, installed RAM, the
  git revision and dirty flag, and the four toolchain versions.

#### Coverage

- **`tests/unit/mlir_pipeline_test.cpp`:** two tests pin the coalesced and
  non-coalesced frame shapes (`array<1 x ptr>` vs `array<2 x ptr>`).
- **Integration expectations** were updated to the root-frame shapes the
  lowering now emits: `gc_frame_early_return_cleanup`, `gc_frame_roots`,
  `gc_fused_frame_open`, `gc_protocol_elision`, `gc_root_audit_control_flow`,
  `gc_root_completeness`, `gc_root_null_init`, and `opt_inlining`, which now
  asserts the absence of a `@build_node` definition or call instead of any
  mention, so the outlined `@build_node.cold.1` clone no longer trips it.

---

## Version 0.617-beta

### Summary of Changes (since `0.613-beta`)

Implemented the two allocation-architecture optimizations identified by the
`0.613-beta` stress-test attribution (**Upgrade Phi** and **Upgrade Upsilon**),
advancing the release label `+0.004` (`0.613-beta` -> `0.617-beta`) per the
three-upgrade rule.

#### Upgrade Phi — Compact Single-Element Array Representation

- **`runtime/src/alloc_hint.cpp` & `runtime/src/gc.cpp`:** Eligible non-SoA
  one-element arrays whose 8-byte-header span (`object_span_bytes(8, payload_bytes)`)
  fits within `kGcPageBytes` (64 KiB) now select `Compact` (H8, 8-byte header)
  instead of `Extended` (H16, 16-byte header). On `Compact` pages,
  `object_element_count()` already returns `1`, while `layout->flags &
  NRQ_OBJECT_LAYOUT_ARRAY` preserves array identity vs fixed records.
- **SoA & large-object safety:** `memory.owned_soa<T>(count)` (including scalar
  SoA blocks lowered through `emit_planned_rooted_array` with `force_extended`)
  and `nrq_alloc_plan_for_layout` for SoA descriptors remain on H16 (`Extended`)
  because `SoaColumnPtr` reads `element_count` at `payload - 16`. Single-element
  arrays exceeding compact page capacity fall back to `Extended` / `Large`.
- For 24-byte `TreeNode` (Test 02) and 16-byte `CycleNode` (Test 03) allocations,
  per-object footprint drops from 40B -> 32B (-20%) and 32B -> 24B (-25%).

#### Upgrade Upsilon — Compiler-Inlined TLAB Allocation

- **`runtime/include/nirqoba/runtime/gc_inline.h` & `runtime/src/gc.cpp`:**
  Defined a shared ABI (`NrqGcInlineConfig`, `NrqGcFastTls`, `NrqGcPendingRange`)
  checked with `static_assert` offsets in `gc.cpp`, along with
  `nrq_gc_layout_id`, `nrq_gc_set_inline_debug_alloc`, and C/C++ inline helpers
  supporting H0, H8, and H16 (`nrq_gc_inline_try_allocate_{h0,h8,h16}`).
  Pending-range end cursors are synced lazily from `tlab_cursor` / `h0_cursor`
  at publication, refill, pending lookup, and mutator-leave boundaries, avoiding
  an extra pointer load and indirect store per inline allocation.
- **`compiler/lib/Lowering/TirToMlir.cpp`, `RuntimeAttributes.h`, `PassPipeline.cpp`:**
  Replaced opaque per-site calls on eligible `array.allocate` and
  `memory.owned_soa` paths with an inline bump-pointer TLAB fast path using
  `llvm.intr.threadlocal.address(@nrq_gc_fast_tls)` and `@nrq_gc_inline_config`.
  The fast path checks site initialization (`nrq_alloc_site_id_* != 0`), collector
  gate (`Idle`, threshold not due, debug allocation trace disabled), accounting
  batch budget, active TLAB kind (`H8` or `H16`), and remaining bump capacity,
  writes the 8B/16B header, zeroes payload via `llvm.intr.memset` only when
  `tlab_payload_zero == 0`, updates per-mutator accounting, and publishes `*root_slot`.
  Refills, exhaustion, active GC cycles, threshold flushes, cold site resolution,
  dynamic oversized requests, and debug event emission branch cleanly to a cold
  slow path calling `nrq_gc_allocate_array_planned` / `nrq_gc_allocate_soa_planned`.
- **`runtime/src/gc_debug.cpp`:** Propagates `NRQ_GC_EVENT_ALLOCATION` toggles
  to `nrq_gc_inline_config.gate` so development event emission stays identical
  between fast and slow paths.

#### Validation & Measurements

- **Isolated allocation microbenchmark (2,097,151 × 24B H8 nodes, Release runtime):**
  - `nrq_gc_allocate_array_planned` call: `64.9–71.3 ms` (`30.9–34.0 ns/obj`)
  - Inline TLAB fast path + slow-path refill: **`20.4–28.4 ms`** (`9.7–13.6 ns/obj`),
    with **2,093,119 / 2,097,151 (99.81%)** fast-path hit rate.
- **Stress Test 2 (`build/msvc-release`, 3-repeat suite medians):**
  - Test 01 (`managed_object_allocation`): `ALLOC_MS` 80.0 ms, `TRAVERSE_MS` 3.0 ms, `COLLECT_MS` 1.0 ms.
  - Test 02 (`mark_and_sweep`, 2,097,151 nodes): suite median `BUILD_MS` 262.0 ms / `COLLECT_MS` 58.0 ms (standalone quiet runs: `BUILD_MS` 183–277 ms, `COLLECT_MS` 40–60 ms; `BUILD_MS` remains dominated by per-activation frame/barrier/pin protocol calls).
  - Test 03 (`cyclic_graph_reclamation`, 1,000,000 nodes): suite median `BUILD_MS` 69.0 ms / `COLLECT_MS` 1.0 ms.
- **Tests:** 464 / 464 C++ unit tests (`GcInlineTlabTest`, `MlirPipelineTest`, `AllocHintTest`, `GcRepresentationTest`, `GcPlannedAllocationTest`, `AllocPlanPropertyTest`), 625 / 625 integration & memory-model cases (including `gc_inline_tlab_alloc`), 140 / 140 runtime invariant parity checks, and 37 / 37 `gc_*` configuration equivalence checks passed.

---

## Version 0.613-beta

### Summary of Changes (since `0.609-beta`)

Performance recovery for stress-test 2, measured against the `0.613-beta` runtime
before the change. The label stays at `0.613-beta`: one completed upgrade is
recorded here, and the versioning rule moves the label after three.

#### Confirmed problems (production configuration, the suite's own harness)

| workload | phase | before | competitor | target |
|---|---|---:|---:|---:|
| 02 mark and sweep | `BUILD_MS` | 223 | Java 14 | < 14 |
| 02 mark and sweep | `COLLECT_MS` | 69 | C++ 33 | < 33 |
| 03 cyclic reclamation | `BUILD_MS` | 68 | Rust 4.7 | < 4.7 |

#### Smoking guns

- **The marking fast path excluded arrays.** `mark_slice_locked` sent an object
  down `enqueue_narrow_children_locked` only when its layout was not an array, so
  a one-element array with two references — the shape both stress workloads build
  their nodes from — took the layout callback and a page-directory probe per edge
  instead of the same-page resolution that path performs. A one-element array has
  the geometry of a record: its payload is the single element and the descriptor's
  offsets are relative to that element.
- **Sweep is not a cost in these workloads.** The phase split shows 0.1 ns/object
  for a fully live heap: whole-live pages are classified from their counts and
  skipped without reading a header. A page-level bulk-promotion path was
  implemented for the major-collection case, measured neutral for that reason, and
  reverted.
- **Marking is memory-latency bound, not instruction bound.** After the fix, a
  live collection of 2 097 151 nodes still spends ~30 ns/object following two
  edges over a 100 MB heap. Prefetching both children at enqueue was implemented
  and measured slower (32.2 against 30.5 ns/object) and reverted.
- **Allocation is ~26–31 ns per object of the ~108 ns/node `BUILD_MS`.** The
  remaining cost is the per-activation protocol the compiler emits — a root frame
  with two registrations, one barrier store into the frame, two field barrier
  stores, two unpins and one pin transfer — not the bump allocator, which the same
  probe measures at 26 ns for this shape in isolation.

#### Implementation changes

- `runtime/src/gc.cpp`: the narrow-shape test in `mark_slice_locked` now accepts a
  single-element array, so the marker walks its two reference offsets directly and
  resolves each child against the parent's page before consulting the directory.
  Larger arrays keep the layout callback, which is the only path that knows the
  element stride.
- `benchmarks/stress_test_2/investigation/build_probe.cpp`: a replica of both
  build phases and of a live collection over the tree, used to attribute the
  phases. Diagnostic only; it writes no result file.
- `scripts/stress2_build_timing.py`: runs the two build phases and reports their
  medians and ranges.

#### Results

Same host, same harness, same configuration, one rebuild apart:

| workload | phase | before | after | ratio |
|---|---|---:|---:|---:|
| 02 mark and sweep | `COLLECT_MS` | 69 | **43** | **1.60×** |
| 02 mark and sweep | `BUILD_MS` | 223 | 231 | 1.04× (noise) |
| 02 mark and sweep | `TRAVERSE_MS` | 5 | 5 | — |
| 03 cyclic reclamation | `BUILD_MS` | 68 | 67 | 0.99× (noise) |

`COLLECT_MS` moves from 2.1× the C++ figure to 1.3× it. **No target was reached:**
`BUILD_MS` in both tests is unchanged by this change, and the collection is still
above 33 ms.

#### Validation

- Unit tests: 457 / 457 in both configurations.
- Integration: 624 / 624 in the development configuration.
- Configuration equivalence: 67 agreed, 0 failed (`*gc*`).
- Stress corpus 02 and 03: validation passed in both the development and the
  production configuration.

#### Construction protocol, attributed

The build phases were attributed by counting cycles inside each runtime entry
point for one full build (temporary instrumentation, removed after the run). The
counts and per-call costs in cycles:

| entry point | test 02 calls | cycles/call | test 03 calls | cycles/call |
|---|---:|---:|---:|---:|
| `nrq_gc_root_frame_open` | 2 097 151 | 87 | 3 500 000 | 107 |
| `nrq_gc_root_frame_end` | 2 097 152 | 60 | 3 500 000 | 69 |
| `nrq_gc_allocate_array_planned` | 2 097 151 | 95 | 7 000 000 | 79 |
| `nrq_gc_root_store` | 4 194 302 | 17 | 10 500 000 | 16 |
| `nrq_gc_unpin` | 2 097 151 | 17 | 0 | — |
| `nrq_gc_pin_transfer` | 2 097 151 | 17 | 0 | — |

Each figure includes ~25 cycles of measurement, so the ordering rather than the
absolute value is what the table is for. Two conclusions follow. The frame pair is
the largest non-allocation cost — 147 cycles per activation against 17 for a write
barrier and 17 for a pin transfer — and it is not a bug: one activation registers
two roots into the thread's slot index and removes them again. And the arithmetic
floor says the published targets cannot be met by removing protocol: at 79–95
cycles per allocation, allocating the 2 097 151 nodes costs ~46 ms before the frame,
the barriers, the pins or the recursion, against a 14 ms target. Reaching it needs
a different allocation code path — an inline bump with an out-of-line slow path,
and a header small enough for one-element arrays — not further trimming of the
calls the compiler emits today.

Three construction candidates were implemented and measured, and all three were
reverted: bulk per-page promotion in a major sweep (neutral, because a fully live
page is already classified from its counts at 0.1 ns/object), child prefetching at
worklist enqueue (32.2 against 30.5 ns/object), and `nrq_gc_root_store_release` in
place of the unpin/store pair (inside the host's noise floor, so not adopted).

#### Remaining gaps

`BUILD_MS` in test 02 (218–231 ms against a 14 ms target) is the per-activation GC
protocol plus 79–95 cycles of allocation per object; closing it needs the root
frame, barrier, pin and transfer calls removed from the emitted sequence for a
recursive builder, not a faster allocator. Test 03's 67 ms for a million nodes is the same
protocol with a void return. Marking a 100 MB live heap at ~30 ns/object is limited
by memory latency; the next lever is object density (40 bytes of node per 24 bytes
of payload), not more instructions removed.

---

## Version 0.609-beta

### Summary of Changes (since `0.605-beta`):

- **AOR Phase 6 completed.** Three upgrades: the shape-indexed layout memo, a
  planned entry point for records, and plan-driven arena and pool construction.
  The first half's releases and measurements are unchanged and still stand.
- **Layout memo:** the four-entry ring thrashed as soon as a program used five
  shapes; eight alternating shapes re-interned on every allocation. It is now a
  sixteen-set, two-way table indexed by shape, with one replacement bit per set,
  costing 260 bytes per thread. The typed eight-shape workload fell from 78.15 to
  52.26 ns/op (1.50×), and typed and planned allocation are now indistinguishable
  at 24 and 40 shapes.
- **`nrq_gc_allocate_planned`:** planned record allocation. The representation
  comes from the plan's two-bit field, the allocator still upgrades a hint the
  payload cannot support, and the site cache is validated against the shape on
  every call.
- **`nrq_cdmm_pool_create_planned` / `nrq_cdmm_arena_allocate_planned`:**
  deterministic regions sized and aligned from a compiler plan, refusing plans
  with headers, references or arrays because a region that does not trace cannot
  honour them.
- **Coverage:** `docs/memory-model/allocation-coverage.md` records which path every
  source construct takes, which entry points use a plan, and which fallbacks are
  intentional. Nirqoba records are owned storage, so no plan applies to them, and
  the typed entry points stay as the documented fallback.
- **Validation:** 434 unit tests (property checks across 14 record sizes and 4
  array shapes, eight-shape memo behaviour, same-size/different-table cache
  rejection, rejected-plan fallback, four threads allocating planned arrays
  concurrently), 624 integration cases, and 67 configuration equivalence
  agreements for `*gc*`.
- **Metadata footprint, measured:** 8 bytes per allocation site (40 sites = 320
  bytes), 6 288-byte object file for the 40-site program, 260-byte thread-local
  memo. No typed-versus-planned code-size delta was isolated.
- **Docs and reports:** `reports/aor/phase-6-coverage.md`,
  `reports/aor/phase-6-performance.md` updated with post-fix numbers and the null
  results, `docs/memory-model/alloc-hints.md` and `cdmm.md` updated.

---

## Version 0.605-beta

### Summary of Changes (since `0.601-beta`):

- **AOR Phase 6 completed.** The Phase 5 pool release path was measured and
  rewritten, and compiler layout knowledge now reaches the allocator through an
  explicit hint, plan and per-site descriptor cache. No safety check introduced in
  Phase 4 or 5 was removed to pay for either change.
- **Pool release (alpha):** a slot's chunk is found through a sorted chunk index
  instead of a linear scan, free lists are per chunk, and reset visits only live
  slots. Individual release fell from ~62 to ~16 ns/op with generations, exact-slot
  validation, double-release refusal and poisoning still enabled; reset of a
  mostly free pool fell from 58.8 µs to 33.0 µs. See
  `reports/aor/phase-6-alpha.md`.
- **New release entry point:** `nrq_cdmm_pool_scope_release_checked` validates a
  generation from an open scope without a registry lookup, and
  `nrq_cdmm_pool_seed_generation` (debug builds only) exposes generation
  wraparound to tests.
- **Allocation hints:** `NrqAllocHint` states what a compiler knows about a site;
  `nrq_alloc_plan` turns it into an `NrqAllocPlan` (representation, header bytes,
  payload, slot size) and `nrq_alloc_plan_flags` packs that into one 32-bit word.
  The policy is the Phase 4 policy: arrays and objects past a page are extended,
  fixed records up to 256 bytes are headerless, everything between is compact.
- **Planned allocation:** `nrq_gc_allocate_array_planned` and
  `nrq_gc_allocate_soa_planned` accept the compiler's plan plus a per-site cache
  word. The runtime validates both on every call and falls back to its own
  decision when either does not hold, so a wrong plan costs a lookup and never a
  wrong representation.
- **Compiler lowering:** `array.allocate<T>` and reference-carrying
  `memory.owned_soa<T>` sites emit one private zeroed `nrq_alloc_site_N` word and
  call the planned entry points with plan `1 | (3 << 8)`. The typed entry points
  keep their signatures.
- **Deterministic layouts:** `nrq_cdmm_pool_create_layout` and
  `nrq_cdmm_arena_allocate_layout` configure a pool or arena from a compiler
  descriptor, and refuse any layout a non-tracing region cannot honour (managed
  references, SoA columns, a custom trace callback, arrays).
- **Measured result:** the site cache makes no measurable difference for a single
  shape, and 1.46× (78.15 → 53.37 ns/op) when eight shapes round-robin through a
  four-entry layout memo. Both results, with spread, are in
  `reports/aor/phase-6-performance.md`.
- **Verification:** 426 unit tests, 624 integration cases, and 67 configuration
  equivalence agreements pass. Reports: `reports/aor/phase-6.md`,
  `reports/aor/phase-6-alpha.md`, `reports/aor/phase-6-performance.md`.
- **Docs:** new `docs/memory-model/alloc-hints.md`; `docs/memory-model/cdmm.md`
  now points at the layout entry points instead of calling them future work.

---

## Version 0.601-beta

### Summary of Changes (since `0.597-beta`):

- **AOR Phase 5 completed.** Arena and pool allocation are first-class CDMM
  strategies with runtime APIs, deterministic destruction, and no object header.
  Neither strategy repeats layout, size or liveness in the object: the arena
  chunk's bump pointer and the pool chunk's bitmap and free list answer for every
  allocation, the same page-dedicated reasoning as the headerless GC
  representation.
- **Arenas:** `nrq_cdmm_arena_create/allocate/reset/destroy`, `max_bytes` cap,
  power-of-two alignment, growth chunks, a generation advanced by every reset, and
  containment checks. The first chunk is committed at creation, so a created
  arena either has its capacity or does not exist.
- **Deterministic destruction:** `nrq_cdmm_arena_allocate_destructible` records a
  destructor inside the arena and runs the records newest-first on reset and
  destroy, so a nested owned resource is released after the object that owns it.
  A failed payload allocation rewinds the arena, leaving no stray record.
- **Pools:** `nrq_cdmm_pool_create/allocate/release/reset/destroy` with one
  liveness bit per slot, a free list threaded through the free slots, exact-slot
  address validation, optional 32-bit generations for stale-release rejection, one
  destructor per pool, and poison-on-release.
- **Bulk paths:** scope structs and bulk allocate/release helpers take one handle
  lookup and one operation pin for a whole batch, which removes both locks from
  the per-object path. Reset and destroy are refused while a scope is open.
- **Introspection:** `nrq_cdmm_strategy_of` and `nrq_cdmm_object_facts` report
  which strategy owns an address, a header size of zero, the pool slot size, the
  generation, and the amortized metadata per object.
- **Coexistence:** the explicit strategies use a separate handle table and a
  separate address-range table, so the collector and the legacy allocator entry
  points cannot reset, destroy or claim each other's storage. Tests cover both
  directions, including a collection while arenas and pools hold live objects.
- **Two runtime defects found and fixed in this phase:** the first chunk was
  committed lazily, so `committed` read zero after creation, and scope release
  reused the serialized entry point and took a mutex per slot. Both are covered.
- Benchmarks: `benchmarks/aor/phase5` measures arena, pool, destructible arena and
  ordinary owned allocation on uniform, mixed-size and mixed-lifetime workloads.
  On an i5-1235U with the development runtime, 200,000 64-byte objects allocate at
  23.99 ns each in an arena against 174.69 ns owned, and a bulk reset costs
  5.07 ns per object against 135.77 ns for individual release (medians of 9 runs).
- Tests: `394` unit tests and `624` full-run cases pass. Phase 7 isolation, the
  Phase 9 production/debug matrix, and `*gc*` configuration equivalence pass.
- Version bumped by `0.004` after the two strategy upgrades (arena, pool), as the
  phase rule requires.

---

## Version 0.597-beta

### Summary of Changes (since `0.593-beta`):

- **AOR Phase 4 completed.** The H32 object header is gone. Managed objects now
  use H0 (headerless, 0 bytes), H8 (compact, 8 bytes), or H16 (extended, 16
  bytes); no standard object carries more than 16 header bytes.
- **A page names the representation.** `PageKind { Headerless, Compact, Extended,
  Large, Fallback }` fixes the header size and payload offset, so no object needs
  a size tag. The object-start bitmap now indexes span starts, and payload is
  `span_start + header_bytes`.
- **H16 layout:** `{element_count, state, layout_id, flags}`. `element_count` is
  first so the compiler's `payload - 16` SoA count read is unchanged. Arrays and
  SoA containers are always extended, including one-element containers.
- **H8 layout:** `{state, layout_id}`. **H0:** no header; the layout is the page's
  `uniform_layout`, state is one byte per object, and the element count is one.
- **`birth_epoch` removed from every object.** A page born bitmap plus a lazy
  `born_epoch` stamp replaces the 8-byte field; stale bits are never read.
- **Layout identity is a 32-bit id table** (14-bit space, thread-local 8-entry
  memo) instead of an 8-byte pointer in each object.
- **Public API:** `nrq_gc_allocate_repr`, `nrq_gc_object_representation`,
  `nrq_gc_object_header_size`, `nrq_gc_get_representation_facts`, and
  `nrq_gc_get_representation_stats`. An impossible explicit request is rejected,
  never silently coerced.
- **Reuse is representation-local.** A free run is only usable by the page's own
  representation; headerless refills reserve a batch of runs so fragmented pages
  do not take the allocator lock per run.
- **Ten bugs fixed**, including recycled pages keeping `use_object_index`, a
  one-element container being described as a record, large pages that took the
  bitmap path with no start bitmap and were never swept, a headerless state table
  that cost as much as the H8 header it removed, and a mixed-shape thread that
  abandoned its slab and allocated a page per object. Regression tests or
  statistics assertions cover each.
- Benchmarks: `benchmarks/aor/phase4` measures allocation and collection for all
  three representations. On an i5-1235U with the development runtime, 200,000
  live 64-byte H0 objects allocate in 5.96 ms and a full live collection takes
  9.58 ms (medians of 9 runs). No H32 binary was rebuilt, so no before/after
  timing is claimed; the header-size comparison is arithmetic and is recorded in
  `reports/aor/phase-4-performance.md`.
- Tests: `360` unit tests and `624` full-run cases pass. Phase 7 isolation, the
  Phase 9 production/debug matrix, and `*gc*` configuration equivalence (67
  agreed) pass.

---

## Version 0.593-beta

### Summary of Changes (since `0.592-beta`):

- **AOR Phase 3 completed.** The intrusive mark link, the per-object TLAB
  publication link, and the stored page pointer were removed. `ObjectHeader` is
  now H32 (24 instance bytes plus the 8-byte shared layout pointer).
- **Marking uses an external worklist.** The 256-entry inline ring is retained;
  overflow goes to a cycle-reused `std::vector` instead of `mark_next`. Capacity
  is kept between cycles, so marking does not allocate per object.
- **TLAB publication uses ranges.** Each mutator records up to 32 contiguous
  `{page, begin, end}` ranges. Publication walks a range by header span, so an
  allocation no longer writes a batch link. A full range table flushes early
  under the collector lock; it never grows.
- **Page ownership is resolved, not stored.** Ordinary objects use a 1024-slot
  direct page-directory probe with a hash fallback; large and fallback objects
  carry their page in their exact `ObjectIndex` entry. Hot sweep/publication
  loops pass the page they already know.
- **SoA count arithmetic unchanged.** `element_count` remains at `payload - 16`;
  compiler and SoA code that loads the count at that offset needed no change.
- Tests: `335` unit tests and `624` full-run cases pass. Phase 7 isolation, the
  Phase 9 production/debug matrix, and `*gc*` configuration equivalence pass.
- No performance claim is made. Phase 3 removes per-object writes and 16 header
  bytes, but adds a page lookup on object-first paths; the net effect has not
  been benchmarked.

---

## Version 0.592-beta

### Summary of Changes (since `0.591-beta`):

- **AOR Phase 1 completed.** Every `ObjectHeader` field was audited against its
  allocator, marker, barrier, sweep, page, array, large-object, debug, and
  recycling consumers. H48 remains the correctness fallback because no field is
  safe to remove from mixed pages yet.
- **AOR Phase 2 page-local metadata implemented.** Pages now carry class,
  slot/layout placeholders, start/mark/remembered bitmaps, and live, live-byte,
  young, born-this-cycle, marked, marked-young, remembered, and indexed-object
  counts. A direct-mapped cache accelerates repeated page lookup without
  replacing the authoritative directory.
- **Remembered-set membership moved off the object header.** The store barrier
  updates a page bitmap and count; the public state mask remains reserved for
  compatibility. Promotion and reclaim clear the page bit, and compaction
  validates the exact object start before recycling.
- **Page recycling and validation hardened.** One reset helper clears all derived
  metadata before reuse; development builds validate count relationships and
  require no mark to survive the cycle end.
- **GC marking and reclamation from the preceding optimization are included:**
  off-heap marks, whole-page dead-page retirement, lazy all-live skip, mark-queue
  prefetch, and inline narrow-layout offsets.
- Tests: `335` unit tests and `624` full-run cases pass. Phase 7 isolation, the
  Phase 9 production/debug matrix, and `67` configuration-equivalence cases pass.
- No performance claim is made for Phase 2 yet. The measured bitmap overhead is
  `3072` bytes per ordinary 64 KiB page (start, mark, remembered; 4.7%).

---

## Version 0.591-beta

### Summary of Changes (since `0.583-beta`):

- **Phase 9 stabilization completed.** The ownership-first hybrid model was
  audited and optimized without adding a strategy, changing a lifetime rule, or
  weakening a release/root/barrier guarantee.
- **Natural-alignment allocation no longer carries a private prefix.** On x64 the
  process heap already satisfies `max_align_t`, so default-aligned blocks are
  allocated and freed directly. Over-aligned requests still use the prefix path.
  Allocator identity, size and alignment remain in the ownership registry.
- **Cached release no longer signals a condition variable.** No waiter can block
  on a cache-path record because that path never marks a record in flight.
  Slow-path release still notifies.
- **Dead cleanup clear removed before return.** A null constant and store that
  immediately precede `tir.return` are dropped after verified TIR and `Drop`
  expansion; destruction calls and discharged obligations are unchanged.
- **Default allocation path reads its thread selection and cache switch once**
  instead of rereading both after a cache miss.
- Tests: `332` unit tests and `624` full-run cases pass. The Phase 9 matrix
  builds ten ownership fixtures at `-O0`, `-O2`, production and debug, compares
  behavior, and checks generated LLVM for duplicated releases and GC leakage.
- Benchmarks: `benchmarks/memory-model/phase-9/` records 15 fresh-process
  samples. Retained allocation improved `215.9 -> 182.3 ns/op` (median), peak RSS
  fell `7.3%`, and the retained allocate+release total improved about `3.8%`.
  Release alone regressed `108.3 -> 129.6 ns/op`; this is reported, not hidden.
- No FFI was added. Phase 10 CDMM was not started.

---

## Version 0.587-beta

### Summary of Changes (since `0.583-beta`):

- Intermediate Phase 9 version after the first two implementation upgrades:
  natural-aligned heap blocks lost their unnecessary private prefix, and cached
  releases stopped waking a condition variable with no waiter.
- Phase label advanced to 9. The final Phase 9 documentation, remaining
  verification and post-optimization benchmark results are recorded under
  `0.591-beta`.

---

## Version 0.583-beta

### Summary of Changes (since `0.575-beta`):

- **Manual storage has a distinct ownership category.** `ValueOwnership::Manual`
  is owning, transferable and requires destruction, but it is not `Owned`,
  `Raw`, `Shared`, `ArenaBound` or `GcManaged`. TIR preserves the category on
  allocation, loads and moves, so lowering and verification cannot silently
  turn manual storage into ordinary owned storage.
- **New explicit surface:** `memory.manual_allocate<T>(bytes[, allocator])`
  and `memory.manual_release(pointer)`. Manual allocation uses the existing
  allocator registry and natural payload alignment; no second heap or universal
  object header was added.
- **Release pairing is enforced.** `memory.deallocate`, `memory.secure_release`,
  `memory.shared_release` and resource close calls reject manual storage;
  `memory.manual_release` rejects ordinary storage. The TIR verifier rejects a
  malformed category or release even when Sema is bypassed.
- **Manual lifetime rules:** moves invalidate the old binding, borrows do not
  transfer ownership, releasing with a live borrow is rejected, double release
  and use-after-release are rejected, and reallocation is refused in favour of
  release-and-reallocate. A lexical fallback release keeps unsupported exits
  safe while allowing explicit release.
- **Scope boundary:** manual storage cannot cross an owned function parameter or
  return, because the language has no manual pointer type to carry provenance.
  Borrow parameters remain available. Tracked arena/pool handles and payloads
  requiring deterministic field cleanup are rejected.
- **Managed payloads and GC:** manual storage containing `managed<T>` receives
  the existing typed-root registration and unregistration; plain manual storage
  emits no `nrq_gc` IR or object dependency. Phase 7 isolation still passes.
- **FFI remains unsupported and is now stated as a boundary.** There is no
  `extern`, foreign pointer type, callback type or source ABI. No C ABI was
  invented; an `extern` declaration remains a compile-time error, and the
  ownership contracts that a future FFI must satisfy are documented as
  unimplemented.
- Tests: 329 unit tests and 624 full-run cases pass, 0 failed, 0 skipped. The
  Phase 1–7 suites are preserved; new cases cover lifecycle, fallback cleanup,
  moves, scoped borrows, allocator handles, managed rooting, conditional
  release, wrong/double release, use-after-release, live-borrow release,
  reallocation, function escape, arena rejection, deterministic-cleanup payloads
  and FFI rejection.
- Benchmarks: `benchmarks/memory-model/phase-8/` records 7 fresh-process samples
  for ordinary and manual allocation, explicit allocator handles, fallback
  cleanup, borrowed access and a managed payload. Production medians:
  ordinary `142.2` cycles/op, manual `144.4`, manual with an explicit handle
  `340.5`, fallback `142.4`, borrowed access `1.70` cycles/read, managed-root
  manual storage `345.4` cycles/op. Manual/ordinary ratio is `1.0155`; no
  speedup is claimed. Debug overhead on the managed root workload is `1.0467×`.
- **Known limitations:** manual storage is function-local; manual reallocation
  and arbitrary alignment selection are not provided; no FFI bridge, foreign
  ownership transfer, retained foreign pointer or callback contract exists;
  manual allocator handles still have a manual root-lifetime contract. Phase 9
  was not started, and the Phase 10 CDMM proposal is untouched.

---

## Version 0.575-beta

### Summary of Changes (since `0.571-beta`):

- **GC allocation is explicitly selected.** `array.allocate<T>` remains the
  collector allocation, `managed<T>` its reference type, and
  `memory.owned_soa<T>` an explicitly GC-backed container with a documented
  legacy name. `memory.allocate<T>`, `memory.shared_allocate<T>`, arenas and
  pools remain ordinary/shared/bulk storage and do not become collector
  allocations. No automatic strategy selection or CDMM was added.
- **The stale-root arena/pool path is rejected.** A tracked arena or pool
  allocation whose payload contains a managed reference now fails in Sema with
  a diagnostic explaining that bulk reset/destroy cannot unregister GC roots.
  The TIR verifier enforces the same contract for malformed or hand-built TIR.
- **Shared managed payloads propagate a GC effect.** A function that calls
  `memory.shared_allocate<T>` where `T` contains a managed field now opens the
  required mutator region instead of being classified as GC-independent.
- **Root, frame, safepoint, barrier and mutator behavior is verified.** Focused
  fixtures cover owned storage holding managed fields across a collecting
  helper, shared root copies and final cleanup, nested/normal roots, transitive
  helpers, required loop polls, barrier preservation at `-O2`, and repeated
  unreachable cycle reclamation.
- **Isolation is checked by declaration and dependency, not only timing.**
  `tests/phase7_isolation.py` compiles twelve ordinary fixtures at `-O0` and
  `-O2`, rejects every `nrq_gc` reference in emitted LLVM and every `nrq_gc`
  symbol in the object, runs each executable, and repeats the check for the
  development runtime. A managed control must contain GC symbols.
- Tests: 326 unit tests and 606 full-run cases pass, 0 failed, 0 skipped;
  the GC-related production/debug configuration corpus agrees on 67 programs,
  0 skipped, 0 failed.
  Existing Phase 1–6 cases are preserved; the new cases cover the arena/pool
  rejection, malformed arena TIR, shared managed-effect classification, root
  lifetime across helpers and shared copies, O0/O2 arena/pool/helper isolation,
  O2 barrier preservation, debug-runtime isolation and repeated cycles.
- Benchmarks: `benchmarks/memory-model/phase-7/` records 7 fresh-process
  samples. Managed allocation median is about 54.5 ns/object, root
  register/unregister about 64.8 ns, root frame about 28.1 ns, safepoint about
  0.38 ns, barrier about 1.78 ns, a 500k-object full collection about 6.22 ms,
  a 200k-object cycle build about 74.2 ns/object with a 2.18 ms collection,
  and four-thread allocation about 110.7 ns/object. Ordinary isolation
  medians are about 58.0 ns per owned allocation, 0.072 ns per borrowed read,
  45.6 ns per arena allocation, 141.9 ns per pool allocation/release and 244.0
  ns per plain-shared allocation/release, with every GC counter unchanged.
  No universal speedup is claimed.
- **Known limitations:** tracked arena/pool payloads cannot hold managed
  references; manual allocator handles have a manual root contract; indirect
  calls are conservative collection points; FFI ownership, weak references and
  a new cycle collector remain deferred. No Phase 8 work was started.

## Version 0.571-beta

### Summary of Changes (since `0.563-beta`):

- **Shared control is sharded and atomic.** Each shared pointer has a control
  with an atomic strong count in one of 64 pointer-selected shards. Retain,
  non-final release and count no longer take the global allocator mutex. Final
  release removes the control before deallocation so a concurrent retain cannot
  resurrect the object.
- **Release and borrow interaction is checked.** `memory.shared_release` is
  rejected while a borrow of the storage is live. TIR requires a shared token or
  shared-pointer type for `memory.shared_retain` and `memory.shared_count`.
- **Arena reset reuses bounded capacity.** Reset retains only the initial
  capacity-bounded chunk and frees growth chunks; destroy frees the retained
  chunk. Existing arena invalidation is unchanged.
- **Strong ownership semantics are unchanged.** Copy retains, move does not,
  aggregate fields retain/release in order, and the final release frees once.
- **Weak references are deliberately deferred.** The reserved `Weak` category
  remains unused. Supported shared payloads reject shared fields, so a supported
  shared-to-shared owning cycle cannot be constructed; cyclic graphs remain a
  GC concern.
- Tests: 323 unit tests and 593 full-run cases pass. New cases cover multiple
  owners, copy retain, move without retain, parameter release, nested/loop scope,
  double release, shared borrow/release conflict and shared-cycle payload
  rejection.
- Benchmarks: shared allocation is 4.16x ordinary allocation; an explicit
  retain is 1.233x a move; four-thread independent objects progress without the
  global lock; one-object contention is 1.58x uncontended; batch final release
  costs about 209 ns per object. Arena reset median is about 2.6% below the Phase
  5 median with overlapping ranges. No universal speedup is claimed.
- No Phase 7 GC work, cycle collector, weak syntax, automatic shared promotion
  or new GC was added.

## Version 0.567-beta (intermediate checkpoint)

Two Phase 6 upgrades: the sharded shared-control runtime with final-release
safety, and bounded arena reset capacity reuse. The complete Phase 6 summary is
recorded under `0.571-beta`.

## Version 0.563-beta

### Summary of Changes (since `0.551-beta`):

- **Arena and pool lifetimes are tracked.** `memory.arena(capacity)` and
  `memory.pool(max_object_size)` create typed handles; allocation from one
  produces an `ArenaBound` value with a unique arena id. Raw
  `memory.arena_allocator`/`memory.pool_allocator` handles keep their previous
  semantics.
- **Bulk release is explicit and safe.** `memory.arena_reset`/`destroy` and
  `memory.pool_reset`/`destroy` reject live values or borrows, then invalidate
  every prior binding. The runtime checks handle kind and in-flight operations,
  erases region records and releases backing chunks. `memory.pool_release`
  returns one slot and rejects double release, live borrows and wrong allocators.
- **Arena escapes are compile-time errors.** Returns, owning parameters,
  borrowed returns, aggregate fields, array literals, cross-arena assignment and
  ordinary deallocation are rejected. TIR preserves arena identity through
  loads, calls, moves, projections and address-of, and verifies the reset/destroy
  contract.
- **Arena allocation bypasses the registry.** Tracked arena allocations lower to
  `nrq_arena_allocate`; pools retain `nrq_memory_allocate_from` for slot
  release. Neither path emits GC allocation, roots, barriers or safepoints.
- **Bulk release does not run destructors.** Payloads requiring deterministic
  cleanup are rejected instead of silently leaking; this is the documented
  limitation.
- Tests: 320 unit tests and 583 full-run cases pass. New cases cover lifecycle,
  growth, repeated reset, borrow scopes, branches, loops, escapes, invalid bulk
  release and pool lifetimes; three IR fixtures assert no `nrq_gc` reference at
  `-O0`/`-O2`.
- Benchmarks: the pool path is consistently slower than ordinary allocation;
  arena results are workload-specific and noisy. Full ranges are reported in
  `reports/memory-model/phase-5.md`; no universal speedup is claimed.
- No Phase 6 weak/shared completion, automatic arena selection, FFI, runtime
  borrow tracking or new GC was added.

## Version 0.559-beta (intermediate checkpoint)

Two Phase 5 upgrades: arena id/kind propagation through TIR values and verifier
enforcement of allocation, reset/destroy, release and owning-escape contracts.
The complete Phase 5 summary is recorded under `0.563-beta`.

## Version 0.555-beta (intermediate checkpoint)

Two Phase 5 upgrades: tracked arena/pool constructors with unique identities in
Sema, and enforced lifecycle, escape and pool-release rules. The complete Phase 5
summary is recorded under `0.563-beta`.

## Version 0.551-beta

### Summary of Changes (since `0.547-beta`):

- **Ordinary allocation is verified as non-GC.** The Phase 4 audit classifies
  every allocation path; ordinary pointer, array, aggregate, stack, reallocated,
  secure and plain shared storage use the existing non-collector allocators.
- **Allocation strategy is a TIR contract.** Ordinary allocation requires
  `owned` plain-pointer or owned-array storage, collector allocation requires
  `gc_managed`, and shared allocation requires `shared`. Ordinary releases and
  `tir.drop` reject collector-managed values.
- **Owned-only IR is empty of `nrq_gc` references.** New and existing fixtures
  cover heap pointers, returned arrays, borrowed views, reallocation, secure and
  shared allocation at `-O0` and `-O2`. Managed payloads, explicit collector
  arrays and SoA remain rooted and operational.
- No second allocator, syntax change or runtime borrow/GC flag was introduced.
- Tests: 313 unit tests and 554 full-run cases pass.

## Version 0.547-beta

### Summary of Changes (since `0.543-beta`):

- **Borrow contracts are checked at calls and returns.** Mutable return
  mismatches, aggregate borrow escapes through calls, duplicate lexical lifetime
  records and stale records without a borrow operation are rejected.
- **Diagnostics point at the conflict.** Source borrow conflicts carry a note at
  the live borrow that caused the rejection.
- **Loop merge correction.** A newly discovered allocation id on one path is no
  longer mistaken for a different owner, so borrowing a scalar inside a loop does
  not make it unusable after the loop.
- **No runtime cost.** MLIR evidence asserts that an address-of/read program
  contains no borrow, allocation or GC call.
- Tests: 308 unit tests and 542 full-run cases pass; the memory-model borrow
  suite now has 40 cases.

## Version 0.543-beta

### Summary of Changes (since `0.539-beta`):

- **Borrow state is verified over the CFG.** A new TIR pass builds basic
  blocks, computes active borrows at branch merges and loop back-edges, and
  rejects conflicting or invalidated borrows that a straight-line scan missed.
- **Borrow liveness follows aliases.** Address-of results, copies, loads from
  bindings and borrowed call results form an alias set; a restriction lasts only
  while one alias is live, so overwritten and expired borrows stop conflicting.
- **Writes and returns respect live borrows.** A store overlapping a live borrow
  is rejected while disjoint field writes stay valid, and a return may not leave
  a local owner still borrowed.
- **Call permission is the declared contract.** Passing `&x` to an immutable
  borrow parameter now creates a read-only borrow, allowing compatible immutable
  borrows that were previously over-rejected.
- **Borrow contracts are checked at returns and calls.** Mutable return
  mismatches, aggregate borrow escapes, duplicate lexical lifetime records and
  stale records without a borrow operation are rejected.
- Tests: 305 unit tests pass; the full runner has 530 cases, including eleven
  new positive and negative Phase 3 memory-model cases.

## Version 0.539-beta

### Summary of Changes (since `0.535-beta`):

- **Overwriting an owner destroys the old value first.** When an automatic
  cleanup is still armed, an assignment to that slot emits the pending
  destruction before storing the replacement. This covers conditional releases
  and conditional moves, where the slot is null only on one path.
- **Defer captures its argument.** A deferred release keeps the value that was
  registered, so rebinding the owner and deferring the replacement remains a
  supported pattern with two independent releases.
- **Explicit releases and defers resolve their obligations** rather than marking
  them transferred to a nonexistent new owner.
- Tests: 299 unit tests pass; the memory-model suite adds overwrite and
  conditional-release runtime coverage.

## Version 0.535-beta

### Summary of Changes (since `0.531-beta`):

- **Drops perform destruction.** Automatic cleanup is emitted as one `tir.drop`
  carrying its release operation; lowering expands it into the runtime call.
  Verification rejects a missing release and a duplicate immediate release.
- **Re-owned slots get new obligations.** A slot that was moved out or already
  discharged and later receives an owner registers a fresh obligation instead of
  reusing a resolved one.
- **Obligations must resolve.** Every obligation must be discharged or transferred;
  a dangling obligation is verified as a leak.
- Tests: 295 unit tests pass with the new ownership cases.

## Version 0.531-beta

### Summary of Changes (since `0.527-beta`):

- **Transfer reasons are checked against their consumers.** A `return` move must
  feed a `Return`, an `argument` move must feed a call, and a transfer reason can
  no longer describe an unrelated consumer.
- **Destruction obligations must attach to storage.** An obligation owner must be
  produced by `alloca`, `array_alloca`, `gep`, `address_of` or `soa_column_ptr`,
  and reserved `Weak`/`ArenaBound` categories cannot be ordinary return ownership.
- **Transferred aggregate fields keep their ownership.** Extracting an owned field
  records the field's category on the extracted value so its move verifies, while a
  plain read remains trivial.
- **Tests:** 294 unit tests and 502 integration tests pass; the new runtime
  regression covers a return-to-local-to-release transfer chain with a live borrow.

## Version 0.527-beta

### Summary of Changes (since `0.523-beta`):

- **Borrow paths and drop/release pairing.** A borrow records the selected
  storage path (`p.x`, `values[]`), must originate from `address_of`, and every
  `tir.drop` must be immediately followed by the release call it names.
- **TIR output exposes the representation.** Function signatures show ownership
  categories, and the dump lists lifetime relations and destruction obligations
  with their state, alongside the move/borrow/end_borrow/drop operations.
- **Ownership diagnostics are actionable.** Use-after-move, double-end and
  double-drop errors carry a note at the operation that created the invalid
  state.
- **Tests:** 289 unit tests and the integration suite cover the new rules,
  including positive representations and corrupted-TIR negative cases.
- **Documentation:** `docs/memory/ownership-representation.md` and the CLI
  diagnostic table describe the contract and its non-goals.

## Version 0.523-beta

### Summary of Changes (since `0.519-beta`):

- **TIR verifier enforces the ownership contract.** The Phase 9 verifier now
  rejects use after move, double moves, borrow mutability/origin/lifetime
  mismatches, double-ended borrows, unknown or twice-discharged obligations, a
  drop without its release call, and a release whose family or resource domain
  does not match the obligation. `NRQ4006`-`NRQ4008` name the broken rule.
- **Ownership metadata reaches MLIR.** Functions publish
  `nirqoba.lifetime_relations` and `nirqoba.destruction_obligations` alongside the
  existing parameter and return ownership attributes.
- **Stack promotion is preserved.** Moves propagate descriptor identity through
  the escape scan, so promoted stack allocations are never released through the
  heap deallocator and escaping allocations remain on the heap.

## Version 0.519-beta

### Summary of Changes (since `0.515-beta`):

- **Explicit ownership categories.** `ValueOwnership` now has one shared
  vocabulary and classification helpers (`ArenaBound` and `Weak` are reserved
  representation categories, not produced by the builder). TIR values,
  parameters and returns keep their category.
- **First-class transfer, borrow and drop operations.** Ownership transfers emit
  `tir.move` with a reason, `&` emits `tir.borrow` with owner and mutability plus
  a lifetime record, scope exit emits `tir.end_borrow`, and lexical cleanup emits
  `tir.drop` naming the discharge obligation.
- **No source or runtime change.** The operations are semantic markers; lowering
  aliases a move and ignores the other markers, so generated code is unchanged.

## Version 0.515-beta

### Summary of Changes (since `0.511-beta`):

- **Owned-only release is collector-free.** Owned aggregate destruction, owned
  array release, owned pointer deallocation, borrowed views and stack-promoted
  arrays no longer emit `nrq_gc_root_unregister` when the payload contains no
  managed reference. Managed payloads retain their register/unregister pair. The
  call disappears at `-O0` and `-O2`, and from the owned-aggregate object's
  undefined symbols.
- **Secure storage rejects managed payloads.** `memory.secure_allocate<T>` now
  rejects a managed field at any depth with `NRQ3015`, because secure release
  zeroes bytes without a rooted registration. Plain secure payloads remain
  supported and emit no collector calls. This is a deliberate compile-time
  incompatibility for previously accepted, but unrooted, secure managed programs.
- **TIR ownership is verified.** `verify_tir_module` now enforces ownership/type
  compatibility, valid release families, single consumption, return and borrow
  sources, callee parameter contracts and result-record consistency. Ownership is
  still structural metadata; no move/drop syntax or new language syntax was added.
- **Tests:** 270 unit tests and 500 integration tests pass. New fixtures cover
  owned aggregate/array/pointer/view/stack GC isolation at `-O0` and `-O2`,
  managed-root retention, direct and nested secure managed rejection, and malformed
  TIR ownership states.
- **Performance:** no runtime benchmark was run. At `-O2` the owned-aggregate
  executable is 62,976 bytes against the Phase 0 audit's 64,512-byte baseline;
  this is an artifact observation, not a throughput claim.
- **Still deferred:** first-class TIR move/drop operations, path-sensitive
  destruction proofs, arena object semantics, secure managed rooting, exception
  cleanup, owned strings, weak references, FFI and thread ownership.

## Version 0.511-beta

### Summary of Changes (since `0.508-beta`):

- **Legacy GC language builtins removed.** `nrq_gc_init`, `nrq_gc_shutdown` and
  `nrq_gc_collect` are no longer declared in source. Use `gc.enable()`,
  `gc.disable()` and `gc.collect()`; process startup and shutdown own the
  runtime lifecycle. The C entry points remain for embedders.
- **Legacy memory builtins removed.** Untyped `nrq_memory_allocate` and
  `nrq_memory_deallocate` are undeclared. Use `memory.allocate<T>` and
  `memory.deallocate`, which participate in the typed ownership and cleanup
  contract.
- **Tests:** 258 unit tests and 489 integration tests pass after the removals.
  The unit count drops by one because the two GC control tests were merged into
  one namespaced test; the integration suite gained a secure-storage managed
  payload case.
- **Phase 8 implementation and documentation are complete;** the final phase
  report follows this label.
- **Still deferred:** bounded release of process-lifetime `file.read_text`
  buffers; crash-safe publication; busy-registry termination cleanup;
  synchronous reclamation of collector-owned SoA storage.

## Version 0.508-beta

### Summary of Changes (since `0.505-beta`):

- **SoA columns reject reference-counted and borrowed fields.** A SoA release
  unregisters the container root but does not walk column fields. Shared tokens,
  direct borrowed fields and borrows nested in a column now fail construction;
  plain and managed-reference columns remain supported.
- **Nested generic closers work in type contexts.** `owned_array<managed<T>>`
  and the equivalent nested managed, SoA and view types no longer require a
  space before the final `>`. Shift expressions are unchanged.
- **Tests:** 259 unit tests and 486 integration tests pass at this milestone.
- **Still deferred:** bounded release of process-lifetime `file.read_text`
  buffers; crash-safe publication; busy-registry termination cleanup;
  synchronous reclamation of collector-owned SoA storage.

## Version 0.505-beta

### Summary of Changes (since `0.501-beta`):

- **Shared allocations reject deterministic owners.** A shared payload whose
  fields include an owned resource, a shared token, a borrow or an array view
  cannot be released field-by-field at the final decrement, so it is rejected.
  Managed-only payloads retain their traced roots.
- **Owned arrays reject reference-counted and borrowed elements.** Bulk
  `array.owned_release` frees the element block; it does not walk element
  fields. Elements with shared tokens, borrowed fields or array views are now
  rejected instead of silently dropping their cleanup.
- **Tests:** 259 unit tests and 483 integration tests pass at this milestone.
- **Still deferred:** bounded release of process-lifetime `file.read_text`
  buffers; crash-safe publication; busy-registry termination cleanup;
  synchronous reclamation of collector-owned SoA storage.

## Version 0.501-beta

### Summary of Changes (since `0.499-beta`):

*Pre-MMM Phase 8; `0.457-beta` remains the released `n-1` checkpoint.*

- **Collector arrays reject deterministic owners.** `array.allocate<T>` now
  rejects a payload containing an owned pointer, owned array, SoA, shared token,
  borrowed field or array view. Collector storage has no finalizer, so those
  contracts cannot be honoured; managed references remain supported and traced.
- **Explicit storage rejects deterministic owners.** `memory.allocate<T>`,
  `memory.reallocate<T>` and `memory.secure_allocate<T>` reject the same
  payloads because their release paths free raw bytes without field
  destruction. Managed-only payloads remain available.
- **Tests:** 259 unit tests and 480 integration tests pass, including the new
  positive managed-payload case and the rejected shared/borrowed/owned cases.
- **Still deferred:** bounded release of process-lifetime `file.read_text`
  buffers; crash-safe publication; busy-registry termination cleanup;
  synchronous reclamation of collector-owned SoA storage.

## Version 0.499-beta

### Summary of Changes (since `0.495-beta`):

*Pre-MMM Phase 7 Part 4; `0.457-beta` remains the released `n-1` checkpoint.*

- **SoA fields participate in aggregate rooting.** A `soa<T>` created as a
  struct or tuple initializer now forces an entry root frame. Previously the
  root slot existed but was never registered, so the rooted allocation aborted
  with "rooted GC allocation requires a registered pointer root slot".
- **Aggregate destruction releases SoA fields.** `aggregate.owned_drop` now
  unregisters the container root for every SoA leaf, including fields reached
  through arrays, tuples and nested structs. It does not free the collector
  object; the sweep reclaims it after the root is gone.
- **Aggregate traversal is allocation-free in the compiler.**
  `aggregate.owned_drop` and `aggregate.shared_retain` use direct recursive
  helpers instead of a per-site `std::function`. The `SmallVector<int64_t, 8>`
  path from Part 3 is retained for the field path.
- **Container audit completed.** Owned arrays own explicit storage and register
  typed roots only when an element needs tracing; array views are borrowed
  stack descriptors with no heap lifetime; SoA is a single collector object
  whose reference columns use strided `(offset, stride)` tracing; shared
  storage keeps its reference count and inline root metadata. No speculative
  redesign was made.
- **Benchmark:** 5,000 aggregate drop sites, 15 alternating builds:
  7,454.763 -> 7,507.598 ms (**+0.71%** wall) and 417,304,576 ->
  417,480,704 B (+0.04% peak RSS). The difference is neutral; no speedup is
  claimed for the traversal refactor. The SoA correctness cases cannot run on
  the baseline because it aborts on them.
- **Validation:** 259 unit tests and 474 integration tests pass. No ASan/UBSan
  run is available in the MSVC configuration.
- **Still deferred:** publication crash journal; bounded release of
  process-lifetime `file.read_text` buffers; safe cleanup of busy registries
  during termination; collector-owned SoA storage is reclaimed by the sweep,
  not by `soa.owned_release` itself.

## Version 0.495-beta

### Summary of Changes (since `0.491-beta`):

*Pre-MMM Phase 7 Part 3 close-out; `0.457-beta` remains the released `n-1` checkpoint.*

- **Termination cleanup is best-effort.** Normal return waits for the file,
  mapping and mutex registry locks and retires every entry. `system.exit` uses
  try-lock and terminates without waiting for a blocked I/O thread; a busy
  registry is reclaimed by the operating system.
- **Validation:** 258 unit tests and 466 integration tests pass. No ASan/UBSan
  run is available in the MSVC configuration; the pre-existing Phase 4 GC stress
  flake remains separate.
- **Release note:** this checkpoint contains one implementation upgrade. The
  label was bumped with the Part 3 validation close-out rather than waiting for
  a second upgrade; the history records that fact rather than inventing one.

## Version 0.491-beta

### Summary of Changes (since `0.487-beta`):

*Pre-MMM Phase 7 Part 3 checkpoint.*

- **Aggregate drop/retain paths use inline field paths.** The recursive walk
  uses `llvm::SmallVector<int64_t, 8>` for the path instead of a heap vector.
  Part 4 later replaced the recursive `std::function` wrapper as well.
- **Mutex handles use a generation-checked slot table.** A token carries a slot
  index and generation; destroying a handle clears the slot and a later reuse
  increments the generation, so a stale token cannot name a new mutex.
- **Benchmark:** 100,000 mutex round trips 47.396 -> 42.319 ms (**-10.71%**).
  The aggregate traversal change was measured separately in Part 4; the Part 3
  record does not claim it as a runtime speedup.
- **Validation:** 258 unit tests and 466 integration tests pass.

> **Phase 7 intermediate-label note.** `0.487-beta`, `0.484-beta` and
> `0.480-beta` are retrospective milestone entries: the work was implemented
> and shipped, but no release commit was made at those labels. They are kept as
> separate upgrade records instead of being compressed into `0.483-beta` or
> `0.495-beta`. Older `(released)` entries are actual published checkpoints.

## Version 0.487-beta (retrospective milestone)

### Summary of Changes (since `0.484-beta`):

*Pre-MMM Phase 7 Part 3 work. No release commit was made at this label; the
changes shipped together in `0.491-beta`.*

- **`system.exit` runs deterministic cleanup.** The termination path retires
  uncontended file, mapping and mutex registries before exiting; blocked
  resources remain the documented fallback.
- **Shared aggregate roots are inline.** `SharedRootMetadata` stores up to eight
  managed-reference offsets inline, with the vector fallback only for deeper
  layouts. Aggregate root registration therefore avoids one vector allocation
  for the common shape.
- **Validation:** covered by the `0.491-beta` and `0.495-beta` suites; no
  separate build was published at this label.

## Version 0.484-beta (retrospective milestone)

### Summary of Changes (since `0.483-beta`):

*Pre-MMM Phase 7 Part 3 work. No release commit was made at this label; the
changes shipped together in `0.491-beta`.*

- **Artifact publication is a rollback transaction.** Object and executable
  keep hard-link backups; a failed second rename restores both finals, so a
  handled failure cannot leave a new object beside an old executable. A crash
  between renames remains an explicit limitation.
- **Read strings are process-lifetime values.** `file.read_text` buffers are
  immutable and valid through deferred cleanup and static destructors; the OS
  reclaims the arena at termination. This removes the unsound entry-return
  invalidation rule, at the cost of process-lifetime memory growth.
- **Validation:** covered by the `0.491-beta` suite; no separate build was
  published at this label.

## Version 0.483-beta

### Summary of Changes (since `0.480-beta`):

*Pre-MMM Phase 7 runtime-buffer and native-resource migration; `0.457-beta`
remains the released `n-1` checkpoint.*

- **Shutdown is emitted for every `main` return.** Early returns and the
  argument wrapper's callee are covered. Shutdown stopped calling
  `FlushFileBuffers` on stdout: output is unbuffered, and flushing an anonymous
  pipe blocks until the parent drains it.
- **Live native resources are released at shutdown.** File, mapping and mutex
  registries are cleared and their handles closed; stale tokens no longer
  resolve after the registry is cleared.
- **File handles use a slot table.** A token carries a slot index and
  generation, removing one map-node allocation per open and rejecting stale
  tokens after slot reuse.
- **Benchmark:** repeated 4 KiB reads (20,000): wall -1.35%, peak RSS -2.22%.
  File open/close (100,000): wall -2.04%, peak RSS +0.21% (one page, noise).
- **Validation:** 252 unit tests and 464 integration tests pass. No ASan/UBSan
  run is available in the MSVC configuration; the pre-existing Phase 4 GC stress
  flake remains separate.

## Version 0.480-beta (retrospective milestone)

### Summary of Changes (since `0.477-beta`):

*Pre-MMM Phase 7 runtime-buffer work. No release commit was made at this label;
the work shipped in `0.483-beta`.*

- **The argument bridge no longer allocates.** The CRT argv pointers are already
  process-lifetime, so the runtime refreshes one static `NrqStringArray`
  instead of allocating a heap descriptor per program.
- **`file.read_text` has one process-lifetime arena owner.** Escaping strings
  are allocated from 1 MiB blocks; an oversized allocation gets its own block,
  and release is idempotent. This removed the first 64 KiB layout after it
  measured +3.4% wall and +3.4% RSS.
- **Validation:** covered by the `0.483-beta` suite; no separate build was
  published at this label.

## Version 0.477-beta

### Summary of Changes (since `0.473-beta`):

*Pre-MMM Phase 7 first incremental subsystem migration; `0.457-beta` remains the
released `n-1` checkpoint.*

- **Compiler-generated artifacts are deterministically owned.** `build` stages
  the object and executable at unique sibling paths and publishes each only
  after its producing stage succeeds. A failed build cleans its staging files
  and leaves the previous successful artifacts untouched. Publication failure is
  `NRQ5010`.
- **`run` no longer leaks build products.** The staged executable and object are
  removed after every program exit. `--keep-artifacts` restores the old behavior
  by publishing both at their normal paths.
- **`nirqoba new` publishes atomically.** The complete scaffold is written to a
  sibling `TemporaryDirectory` and renamed into place; a failed write leaves no
  partial project, and a non-empty target is refused.
- **Language/runtime behavior is unchanged.** No runtime source, allocator,
  collector, lowering rule, or optimizer changed. The `hello` executable is
  byte-identical to `0.473-beta` (`6d4d6ff0...e8d7a9`, 9,728 B).
- **Benchmark (41 interleaved samples):** build 99.298 -> 102.779 ms (+3.51%),
  run 157.214 -> 155.831 ms (-0.88%), `new` 33.933 -> 36.325 ms (+7.05%),
  compiler peak RSS +0.24%, `run` artifacts 2 files / 10,300 B -> 0 files / 0 B.
  The build/scaffold publication cost is small in absolute terms and is reported
  as a regression, not hidden.
- **Validation:** 237 unit tests and 462 integration tests pass. No ASan/UBSan
  run is available in the MSVC configuration; the pre-existing Phase 4 GC stress
  flake remains separate.

## Version 0.473-beta

### Summary of Changes (since `0.469-beta`):

*Pre-MMM Phase 6 compiler-lowering and optimization checkpoint; `0.457-beta` remains the released `n-1` checkpoint.*

- **Shared pointer aggregate fields resolve their pointee layout.** The Phase 5
  crash and out-of-bounds GEP for `shared ptr T` fields when `T` contains managed
  fields are fixed. The active-name cycle guard still prevents recursive
  expansion.
- **Unobservable stack-owned roots and write barriers are elided.** A stack-owned
  block with managed fields publishes no typed root and uses a plain store when
  the activation has no collection point. Calls that may collect and loop backedges
  retain the root and barrier; heap, shared, and observable stack storage are
  unchanged.
- **Ownership and alias attributes remain intact.** `noalias`, `readonly`,
  `nocapture`, ownership spelling, cleanup ordering, and exactly-once destruction
  pass the existing IR and integration cases through `-O0` and `-O2`.
- **Isolated benchmark (`root_elision`):** runtime 104.13 ms -> 16.63 ms
  (-84.0%), compile 56.9 ms -> 48.8 ms (-14.2%), compiler peak RSS 29.8 MB ->
  28.7 MB (-3.7%), executable 94,208 B -> 90,112 B (-4.3%). The runtime gain is
  from exposing SROA after removing unobservable bookkeeping, not from weakening
  a check.
- **Validation:** 228 unit tests and 460 integration tests pass, including the
  new shared-field O0/O2 cases and root-elision runtime/IR cases. No ASan/UBSan
  run is available in the MSVC configuration; the Phase 4 GC stress flake remains
  separate.

## Version 0.469-beta

### Summary of Changes (since `0.465-beta`):

*Pre-MMM Phase 5 GC/ownership-boundary checkpoint; `0.457-beta` remains the released `n-1` checkpoint.*

- **Explicitly owned pointer storage can now contain managed fields safely.**
  Heap and stack-placed `memory.allocate<T>` publish typed GC roots for the
  managed offsets in `T`; reallocation moves the root only on success, and
  deallocation removes it before the block is freed.
- **Stack-owned roots end with the activation frame.** A function whose only
  managed reference is inside stack-placed owned storage is marked GC-using, and
  the root is published inside its frame instead of leaking past return.
- **Shared storage keeps its managed fields rooted until the final release.**
  Non-final shared releases retain the root; the last release unregisters it
  before freeing, and restores it if the allocator rejects the free.
- **Explicit allocator-handle release no longer takes a redundant operation
  pin.** Release, reallocation, and final shared release use the live ownership
  record; destroy/reset already refuse while that record is live or in flight.
  Allocation keeps its pin because no record exists before the allocator returns.
- **Measured explicit-handle contention (1/2/4 workers):** 2,330/5,005/16,068 us
  at `0.465-beta` versus 1,840/3,650/8,622 us, improvements of 21.0%, 27.1%, and
  46.3%. The explicit path remains slower than the default path.
- **Validation:** 228 unit tests and 455 integration tests pass. Managed fields
  remain visible across collection in heap, stack, reallocated, cleanup-exit, and
  shared probes. A pre-existing compiler crash for aggregate fields declared as
  `shared ptr T`/`owned ptr T` when `T` contains managed fields remains a known
  limitation.

## Version 0.465-beta

### Summary of Changes (since `0.461-beta`):

*Owned-allocator Phase 4 checkpoint; `0.457-beta` remains the released `n-1` checkpoint.*

- **The default allocation path no longer takes the global allocator mutex or an
  operation pin.** The process-default general allocator cannot be destroyed or
  reset, so ordinary allocations and releases lock only their ownership shard.
- **The ownership registry is sharded.** Eight pointer-selected shards replace
  one process-wide map, so unrelated threads no longer serialize on one mutex.
- **Ownership records are stored inline.** Each shard uses a linear-probing table
  with tombstone reuse and compaction; a live pointer no longer causes a separate
  hash-node allocation and release.
- **Reallocation moves the record between shards.** The replacement keeps the
  allocator, alignment, size, and shared state without a metadata node.
- **Profiling remains exact under concurrency.** An atomic enable flag and a
  dedicated profile mutex protect a consistent capture.
- **Measured result.** Against `0.461-beta`, allocation-heavy improved 27.9%,
  destruction-heavy 20.0%, and mixed 19.2% by median; concurrent default
  allocation improved about 3.1x at four workers, with peak working set flat.
- No ownership, move, cleanup, or exactly-once destruction rule changed.

## Version 0.461-beta

### Summary of Changes (since `0.457-beta`):

*Deterministic-destruction Phase 3 checkpoint; `0.457-beta` remains the released `n-1` checkpoint.*

- **A deferred owner cannot be transferred afterwards.** A move, return, field
  extraction, aggregate pack, or owning call transfer after `defer` captured the
  owner is rejected instead of producing two owners for one allocation.
- **Cleanup actions are liveness-tracked.** A transfer outside a conditional
  region removes the dead automatic action; conditional moves and defers keep
  their fallback on the path where they do not execute.
- **`for` initializers have a loop cleanup scope.** They are released on loop
  exit, `break`, and `return`, while `continue` keeps them alive.
- **Explicit defers supersede the automatic action they replace.** Unconditional
  and same-scope defers leave one destruction site; conditional defers retain
  the outer fallback.
- **GC and native cleanup remain separate.** Return edges run cleanup before
  closing the activation root frame; typed native destructors remain independent
  of GC reclamation.

## Version 0.457-beta (released)

### Summary of Changes (since `0.453-beta`):

*Soundness-audit checkpoint; `0.453-beta` is the previous released build.*

- **Immutable borrows are read-only at every write boundary.** Dereference,
  indexed, compound, and address-through-dereference targets now reject writes
  through explicitly immutable borrowed fields and pointers.
- **Mutable field borrows require mutable storage.** A `borrow mut` field can no
  longer be initialized from an immutable binding, and borrowed fields cannot be
  extracted into untracked raw pointers.
- **Mutable aggregate aliases are rejected.** Copying an aggregate that holds a
  `borrow mut` field, or placing conflicting mutable/immutable borrows of the
  same owner path in one aggregate, is now diagnosed.
- **Branch initialization state survives joins.** Partially initialized fields
  and allocated nested values from either branch cannot be overwritten as if the
  field were untouched; completed nested values are tracked without stale
  partial markers.
- **No runtime cost.** These are semantic checks only.

## Version 0.453-beta (released)

### Summary of Changes (since `0.451-beta`):

*Checkpoint after the interprocedural-summary and escape-safety checkpoint.*

- **Effect summaries are complete for the checked bodies.** Every signature
  carries one compact effect byte per parameter. Nested calls merge the callee's
  read, write, alias, and escape effects into the caller's summary; recursive
  calls reuse the declared summary, and unknown or incomplete callees contribute
  the conservative fallback.
- **Call sites use the summary.** A callee write effect is treated as a mutable
  borrow claim at the call boundary, so conflicts with an existing live borrow
  are detected even when the effect arrived through forwarding rather than a
  direct declaration.
- **Borrowed fields stay inside their lifetime.** Aggregate initializers record
  the owner and path of every borrowed field, transitively through nested
  aggregates. Declarations, assignments, calls, returns, and aggregate-field
  storage reject escapes that would outlive the owner. Disjoint paths remain
  usable.
- **Owner mutation respects aggregate borrows.** Moving or replacing an owner
  while an aggregate holds a borrowed field is rejected, and the field-borrow
  metadata participates in the existing live-borrow queries.
- **No runtime cost.** All summaries and escape metadata are compile-time-only.

## Version 0.451-beta (released)

### Summary of Changes (since `0.449-beta`):

*Released checkpoint for the two post-`0.449-beta` borrowing upgrades.*

- **Interprocedural effect summaries.** Added compact read/write/alias/escape
  bits per borrowed parameter and propagated them through nested and recursive
  calls with conservative fallbacks for incomplete callees.
- **Borrow escape safety.** Aggregate borrowed-field origins are tracked across
  nested aggregates; returns, calls, outer-scope assignments, and aggregate
  storage reject escapes while valid same-scope borrowing and disjoint field
  paths remain accepted.
- **No runtime borrow tracking.** The checkpoint adds only semantic state.

## Version 0.449-beta (released)

### Summary of Changes (since `0.445-beta`):

*Opened after borrowed returns kept their effects through nested calls and borrows could expire at reassignment.*

- **Nested borrowed-return calls keep their origin.** Calling a function that
  returns a borrow now preserves the original owner and field path through any
  number of nested or chained calls. Passing such a result directly to another
  call is validated like a direct borrow instead of being rejected as an
  unknown lifetime.
- **Return-source effects are cached in the signature.** The source parameter
  of a declared borrowed return is resolved once when the signature is built.
  Nested calls reuse that compact index rather than rescanning the parameter
  list at every use, preserving modular compilation.
- **Conflicts follow the propagated effect.** Two nested borrowed-return results
  that reach the same owner/path are rejected; disjoint fields remain usable.
  A stored borrowed return remains a live alias even after the binding that
  produced it is rebound or cleared.
- **Borrows expire and rebind with control flow.** A borrowed binding can now be
  replaced by another validated borrow, or cleared with `nullptr`. The old
  owner/path is released before the new borrow is checked, while replacement
  origins must still be live and outlive the binding.
- **Branches keep borrows that are still possible.** Null/rebind merges preserve
  the active borrow's allocation identity. A path that clears a borrow no longer
  hides a borrow still possible on the other path, while a path that releases
  the old field on every arm leaves it available afterward.
- **No runtime cost.** All propagation and expiration state is compile-time
  semantic data. No runtime counters, locks, descriptors, allocations, or
  generated checks were added.

## Version 0.445-beta (released)

### Summary of Changes (since `0.441-beta`):

*Opened after logical conditions and owner-selecting branches became precise borrow state.*

- **Short-circuit conditions refine borrows.** Null facts follow `&&`, `||`, and
  logical negation along their real true/false evaluation paths. A fact is kept
  only when it holds on every path that reaches the selected branch, so the
  false path of `&&` and the true path of `||` do not over-refine.
- **Branches can retain several owner identities.** When both arms borrow
  different owners, the primary owner remains inline and secondary owner/path
  pairs are stored in a compact alternatives vector. Conflicts, liveness, and
  field queries inspect every reachable owner/path combination.
- **Ambiguous owner use stays conservative.** Aliasing, calls, address-taking,
  and returns through a binding with multiple possible owners are rejected rather
  than picking one branch's identity. Direct reads and disjoint field accesses
  remain available.
- **Fast paths remain allocation-free.** Ordinary single-owner borrows keep the
  existing fields. The alternatives vector exists only after a branch merge that
  selects genuinely different owners.
- **No runtime cost.** All new state is compile-time semantic data. No runtime
  counters, locks, descriptors, allocations, or generated checks were added.

## Version 0.441-beta (released)

### Summary of Changes (since `0.437-beta`):

*Opened after null checks became able to discharge a conditional borrow.*

- **A non-null guard can make a conditional borrow definite.** When the path
  where the borrow is absent is known to be null, `view != nullptr` clears the
  conditional marker inside that branch. The else path of `view == nullptr` and
  an early-return guard refine in the same way.
- **The proof is sound and branch-local.** Definite nullness is tracked through
  declarations, assignments, and merges. A non-null check refines only when the
  absent path is provably null; a possible non-null pointer on that path cannot
  be mistaken for the borrow.
- **Refinement is lost where it should be.** A join where both paths continue
  restores the conditional marker unless every continuing path preserves the
  same null-absence proof. The null path never refines.
- **Field paths remain exact.** A refined borrow keeps its owner, mutability,
  and field-path set. Overlapping reads and borrows are still rejected while
  disjoint fields remain available.
- **No runtime cost.** The refinement adds two compile-time booleans and only
  branch-local symbol-table queries. No runtime counters, locks, descriptors,
  allocations, or generated checks were added.

## Version 0.437-beta (released)

### Summary of Changes (since `0.431-beta`):

*Opened after one-sided conditional merges retain a live borrow's field path.*

- **One-sided borrows keep their projection.** When only one arm leaves a live
  borrow, the merged binding keeps that arm's owner and field path instead of
  collapsing to whole-owner access. The binding is marked conditional because
  the borrow may not exist on the other path.
- **Every reachable conflict is still rejected.** Reads, writes, and new borrows
  that overlap the conditional path are rejected, while a sibling field disjoint
  from it remains independently accessible.
- **Definite uses require a definite borrow.** Aliasing a conditional borrow,
  forwarding it to a call, taking a new borrow through it, or returning it is
  rejected with `borrow of '...' may not be live on every branch`. Direct null
  checks and other ordinary pointer uses remain available.
- **Owner identity is adopted only when safe.** A tracked borrow may pair with an
  untracked or null binding on the other path; an inactive owner that owns or
  shares storage still marks the binding unusable.
- **No runtime cost.** The conditional flag and path retention are compile-time
  semantic state only. No counters, locks, descriptors, allocations, or generated
  checks were added.

## Version 0.431-beta (released)

### Summary of Changes (since `0.427-beta`):

*Opened after branch-selected borrow paths became first-class semantic state.*

- **Branches keep every possible field path.** When both arms leave a live borrow
  of the same owner, the merged binding records `.x` and `.y` (or any nested
  pair) instead of degrading to whole-owner access.
- **Every path is checked.** A later mutable borrow, immutable borrow, read, call
  argument, or returned borrow conflicts if it overlaps any possible path, while
  a field disjoint from all alternatives remains usable.
- **Aliases and calls preserve the set.** Branch-selected paths survive pointer
  aliases, borrowed-return calls, and call-argument ordering. Returning a borrow
  from an ambiguous live alias is rejected instead of creating a second mutable
  alias.
- **Owner identity survives the merge.** The merge carries the shared borrow
  owner and adopts the branch allocation identity when both arms agree, so later
  conflict queries use the owner the paths actually reference.
- **Conservative fallbacks remain visible.** A borrow on only one arm falls back
  to whole-owner access, and different owner allocations mark the binding
  unusable.
- **No runtime cost.** Path sets and conflict checks are compile-time only; no
  counters, locks, descriptors, allocations, or generated checks were added.

## Version 0.427-beta (released)

### Summary of Changes (since `0.423-beta`):

*Opened after mutable-borrow exclusivity became path-aware across calls.*

- **Mutable borrows are exclusive by owner and field path.** While a mutable
  borrow is live, a competing mutable borrow or an immutable access to the same
  path or an ancestor/descendant path is rejected. Disjoint sibling fields remain
  independently borrowable.
- **Reads participate in exclusivity.** Reading the borrowed field, a parent or
  child projection, or the whole aggregate while a mutable borrow is live is
  rejected. Reading a disjoint field remains legal.
- **Calls enforce argument-order exclusivity.** Borrows claimed by earlier
  arguments are visible while later arguments are checked, so one call cannot
  receive two overlapping mutable borrows. Disjoint field arguments still pass.
- **Forwarding preserves the conflict.** A mutable field borrow remains tied to
  its caller path through nested borrowed-return helpers, so a later borrow of
  the same path is rejected while a sibling path is accepted.
- **Mutable returned borrows keep their mutability.** Assignment records whether
  the returned borrow is mutable instead of treating every returned pointer as
  immutable.
- **Scope exit releases exclusivity.** Once a borrow's lexical scope ends, the
  owner or field can be borrowed again.
- **No runtime cost.** Conflict checks are semantic only; no counters, locks,
  descriptors, heap allocation, or generated checks were added.

## Version 0.423-beta (released)

### Summary of Changes (since `0.419-beta`):

*Opened after aggregate-field borrows were tied to their owner path.*

- **Field borrows remember the aggregate path.** `&obj.data` and nested
  `&obj.inner.data` resolve to the aggregate binding and record the dotted field
  path, so the borrow cannot outlive the aggregate that owns the storage.
- **Forwarding keeps the path.** A field borrow can pass through `borrow`
  parameters and source-bound borrowed returns; the caller's field identity is
  preserved through the nested call.
- **Overlapping replacement is rejected.** Writing the borrowed field, a parent
  aggregate field, a descendant field, or the whole aggregate while the borrow is
  live is diagnosed. Compound writes use the same rule.
- **Sibling fields stay usable.** Independent fields of the same aggregate remain
  writable while one field is borrowed.
- **Moves and scope still guard the owner.** Moving an owning aggregate, moving an
  owned field, or letting a field borrow leave its owner's scope remains rejected.
- **No runtime cost.** Field identity and overlap checks are semantic only; no
  counters, locks, descriptors, heap allocation, or generated checks were added.

## Version 0.419-beta

### Summary of Changes (since `0.415-beta`):

*Opened after source-bound borrowed returns were hardened.*

- **Borrowed returns name their source.** `borrow <source> T* function ...`
  keeps a return tied to one borrowed parameter. Direct returns, forwarding
  through nested calls, and selecting the declared source among several
  parameters remain valid.
- **Invalid sources are rejected.** A local owner, a temporary allocation, the
  wrong parameter, a moved source, and a source that has left scope are all
  diagnosed. A returned borrow stored in an outer binding is rejected as
  outliving its owner.
- **Live borrows block owner transfer.** Passing an owner to an owning pointer
  parameter while its borrowed return is live is rejected instead of moving the
  owner out from under the borrow.
- **Assignment keeps lifetime identity.** An address-of origin receives a tracked
  allocation identity when a borrowed value is assigned to an existing pointer.
- **No runtime cost.** The checks are semantic only: no counters, locks, heap
  descriptors, or generated borrow checks were added.

## Version 0.415-beta

### Summary of Changes (since `0.411-beta`):

*Opened after borrow provenance crossed function calls.*

- **Borrows remember their owner.** A pointer binding now records its lexical
  scope depth and, when borrowed, the binding whose storage it aliases. `&owner`
  shares the owner's allocation identity instead of creating an unrelated raw
  pointer.
- **Calls preserve the origin.** Borrow arguments resolve through address-of and
  borrowed-return expressions to the originating owner. Nested calls revalidate
  that owner, while a borrowed parameter is its own origin inside the callee.
- **Stale borrows are rejected.** Using or forwarding a borrow after its owner
  leaves scope, moves, or is replaced reports a lifetime diagnostic. A borrow
  assigned to a binding in an outer scope is rejected as outliving its owner.
- **Null release remains legal.** `p = nullptr` still clears a borrowed pointer;
  other rebinding remains conservatively rejected.
- **No runtime cost.** The rule is compile-time only: no counters, locks, heap
  descriptors, or generated checks were added.

## Version 0.411-beta

### Summary of Changes (since `0.407-beta`):

*Opened to record the pointer-root descriptor work completed after 0.407.*

- **The one-word pointer root has its own descriptor.** A root of
  `sizeof(void*)` with one reference at offset zero and no trace callback
  resolves to an immutable process-lifetime descriptor before the memo. Other
  root shapes keep the existing memo and interning path, and descriptor identity
  in `add_slot_state` and `is_pointer_root` is unchanged. The descriptor is not
  pool-owned, so it is not counted by `nrq_gc_layout_count` or
  `nrq_gc_layout_bytes`.

## Version 0.407-beta

### Summary of Changes (since `0.403-beta`):

*Opened after the allocation layout memo grew from one entry to four.*

- **Four descriptors, round-robin replacement.** The single-entry memo became a
  fixed four-slot array with a rotating insertion index. Interleaved typed
  workloads no longer evict every shape on the next allocation.
- **Each entry is proved against its own table.** `layout_memo_matches` compares
  the request field by field with the descriptor's `reference_offsets`, so a
  slot is never trusted by address and the caller's `offsets` pointer is never
  cached or retained.
- **Miss behavior is unchanged.** A miss still calls `nrq_gc_layout_intern`, and
  a null result is not cached. The change adds no heap allocation, lock,
  dynamic growth or other GC behavior.
- **Per-thread hit/miss counters** (`nrq_gc_layout_memo_hits/misses`) expose the
  memo to tests; they are plain thread-local words.
- **Measured on one machine.** 800,000 typed allocations cycling four layouts:
  median 106.676 ns/allocation before, 57.640 after (about 1.85x). The harness
  is a microbenchmark with collection disabled; see
  `reports/performance/0.407-layout-memo.md` for method and caveats.

## Version 0.403-beta

### Summary of Changes (since `0.401-beta`):

*Opened when the owned-pointer type learned which runtime domain releases it.*

- **Resource domains live in the type.** `owned file ptr byte` now builds an
  `OwnedPointer` type that carries its `resource_domain`, so equality, mangling and
  lowering distinguish a file handle from a socket handle without a side table.
- **Construction checks the producer's cleanup domain.** A resource field accepts
  only a value that owns the same domain, a plain `owned ptr T` field rejects
  resource handles, and mismatches are diagnosed at the field initializer.
- **Destruction is domain-specific and null-safe.** `aggregate.owned_drop` calls
  `file.close`, `socket.close`, `mutex.destroy`, `memory.unmap_file` or
  `memory.secure_release` for each field, exactly once, in reverse declaration
  order, recursing through nested aggregates. Zeroed `uninit` slots hold null,
  which every destructor ignores.
- **Moves and reinitialization keep their existing rules.** Resource fields are
  move-only, a transfer invalidates the source, and a field can be reinitialized
  only after a move and only from the same domain.
- **Phase 1 closes.** All five domains are covered, and the deliberate
  conservative restriction on overwriting a partially initialized nested
  aggregate is unchanged.

The socket runtime remains unlinked in this phase, so socket fields are covered by
the semantic-check suite while the other four domains also build, link and run.

## Version 0.401-beta

### Summary of Changes (since `0.397-beta`):

*Opened after initialization completion propagates through nested aggregates.*

- **A completed nested aggregate counts as initialized.** When every field of a nested
  aggregate has been assigned individually, the enclosing path is cleared from the
  uninitialized set, so whole-value reads and copies of the nested aggregate are
  allowed again.
- **Completion propagates to enclosing aggregates.** The same check then runs on the
  parent path, so a fully initialized `Outer` becomes usable as a whole even though its
  nested field was filled in field by field.
- **Completed aggregates participate in the existing ownership rules.** Whole-value
  copies of a completed nested aggregate retain its shared fields, and whole-value
  moves of an owned nested aggregate transfer them without retaining.
- **The guards are unchanged for incomplete aggregates.** Partially initialized
  whole-value use, reads of uninitialized fields and whole-field overwrites of
  aggregates initialized field by field remain rejected.

Still unimplemented: native-resource owned fields.

## Version 0.397-beta   

### Summary of Changes (since `0.396-beta`):

*Opened after two soundness fixes to the `uninit` support.*

- **A partially initialized aggregate cannot be used as a whole.** Copying, moving,
  returning, passing it as an argument or consuming it in any other whole-value
  context reports a diagnostic naming the binding. Member access stays valid because
  it is what initializes fields, and assignment targets stay valid.
- **Overwriting a partially initialized nested aggregate is rejected.** Assigning
  `o.inner` after `o.inner.data` was initialized would overwrite the initialized
  contents without releasing them, so the assignment fails with a precise diagnostic.
  Per-symbol state records the enclosing aggregate whenever a nested field is
  initialized.
- **Existing behavior is preserved.** Field-by-field initialization, nested
  destruction, shared retain/release and moved-field reinitialization are unchanged,
  and neither guard adds runtime state.

Still unimplemented: native-resource owned fields.

## Version 0.396-beta   

### Summary of Changes (since `0.391-beta`):

*Opened after the `uninit` partial-initialization slice.*

- **`var h: T = uninit;` declares a partially initialized aggregate.** Both declaration
  forms accept it, an explicit struct annotation is required, and the binding must be
  `var` because otherwise no field could be initialized.
- **Fields are initialized individually.** Each field starts in the moved/not-live
  state the compiler already tracks, so reading it before assignment is rejected with
  NRQ3033. Assigning `h.field` or a nested path clears that field and its subtree.
- **Destruction releases only initialized fields.** The slot is zeroed, so untouched
  owned and shared fields are null and their release is a no-op, while initialized
  fields and nested aggregates drop recursively in reverse declaration order.
- **Moved and reinitialized fields keep their semantics.** An owned field accepts its
  first assignment because reinitialization now allows a field that is either moved or
  not yet initialized.
- **No cost for ordinary aggregates.** Fully initialized structs are unchanged and
  carry no extra runtime state.

Limitation: a partially initialized aggregate is not yet rejected when used as a whole
value; copying, moving, returning or passing one must be avoided until all fields are
assigned. Native-resource fields remain unimplemented.

## Version 0.391-beta   

### Summary of Changes (since `0.390-beta`):

*Opened after the shared pointer field slice.*

- **Sharedness is part of the type.** `shared ptr T` fields resolve to
  `TypeKind::SharedPointer` (`shared_ptr<T>`), so it survives type resolution and
  `aggregate.owned_drop` can see it. The type is an ordinary pointer for dereference,
  printing, equality, sizing, alignment and LLVM lowering.
- **Copy retains.** Copying an aggregate with shared fields emits
  `aggregate.shared_retain`, which calls `nrq_memory_shared_retain` once per shared
  field at any nesting depth. Construction from `memory.shared_allocate` or
  `memory.shared_retain` takes the initial reference without an extra retain.
- **Destruction releases exactly once.** `nrq_memory_shared_release` runs once per
  shared field in reverse declaration order, recursing into nested aggregates.
- **Move transfers references.** A moved aggregate zeroes its source, so no retain is
  emitted and only the destination releases.
- **Ownership is validated.** Shared fields accept only shared values; owned, borrowed
  and plain raw pointers are rejected. Shared-containing aggregates are copyable,
  while aggregates with owned fields keep their move-only rules.
- **Existing pointers and APIs are unchanged.** `memory.shared_*` and ordinary
  pointers behave as before; `shared` locals keep their move-on-copy semantics.

Still unimplemented: `uninit` partial initialization and native-resource owned fields.

## Version 0.390-beta   

### Summary of Changes (since `0.389-beta`):

*Opened after the owned-pointer-field contract landed.*

- **A moved owned field can be reinitialized from a fresh owner.**
  `h.data = memory.allocate<int>(2)` now uses the same raw-to-owned coercion as struct
  construction instead of requiring an existing `owned_ptr<T>` value. The field must
  still be moved on every path and the owner live and mutable.
- **The nested-allocation restriction is narrower.** It now applies only to owned
  arrays whose elements own resources, including through aggregates.
- **Aggregates holding owned pointers can be nested.** A struct field whose type holds
  `owned ptr` fields is accepted, and recursive destruction already drops it; this
  also removes the earlier one-off field-level exemption.

Still unimplemented: `shared` pointer fields, resource-domain owned fields
(`owned file ptr byte` and friends), and `uninit` partial initialization.

## Version 0.389-beta   

### Summary of Changes (since `0.384-beta`):

*Opened after the first Pre-MMM Phase 1 ownership slice.*

- **`owned ptr T` fields are real owners.** The qualifier is carried in the field type
  (`owned_ptr<T>`), so existing aggregate rules apply unchanged: a struct holding one
  is move-only, copying invalidates the source, and the value is destroyed by
  `aggregate.owned_drop` at scope exit.
- **Destruction gained an owned-pointer case.** The field pointer is extracted, its GC
  root registration released and `nrq_memory_deallocate_checked` called. Fields are
  now dropped in reverse declaration order (LIFO), matching scope and parameter
  destruction.
- **Construction accepts an owner-producing expression** (`memory.allocate<T>(n)` and
  the other resource producers) or transfers an owned local, which is marked moved so
  it cannot be read or destroyed again. An owned field initialized from a borrow is
  rejected.
- **Rejections name the missing capability:** `shared` pointer fields (reference-count
  release at field destruction), resource-domain owned fields such as
  `owned file ptr byte` (per-domain field destruction), and unqualified raw pointer
  fields.

Still unimplemented: `uninit` partial initialization.

## Version 0.384-beta   

### Summary of Changes (since `0.380-beta`):

*Opened after the first Pre-MMM Phase 1 item.*

- **Fields use the same ownership grammar as parameters.** `borrow`, `borrow mut`,
  `shared` and `owned` with an optional resource domain are accepted on struct fields.
- **`borrow` and `borrow mut` pointer fields are implemented.** The parser records the
  qualifier on the field, the checker requires a raw pointer type, and construction
  must supply a borrow rather than an owned value. Writes through an immutable
  borrowed field are rejected; `borrow mut` allows them.
- **Borrowed fields are non-owning.** Aggregate destruction is unchanged and no
  cleanup is generated for them.
- **`owned` and `shared` fields are rejected with named diagnostics** that state the
  missing capability (generated field destruction and reference-count release)
  instead of the misleading nested-allocation message.

Not implemented: borrowed-field lifetime enforcement across scopes and returns,
`uninit` partial initialization, and owned field destruction.

## Version 0.380-beta   

### Summary of Changes (since `0.379-beta`):

*Opened after two declaration upgrades.*

- **Type-first declarations and brace initializers.** `Point p = {1, 2};`,
  `ptr int g = NULL;` and `@Nullable ptr int h = NULL;` are accepted. Braces are
  positional or named (`{ x: 1, y: 2 }`) and the declared type supplies the struct;
  `var`/`let` keep their form and gain `var ptr int g = nullptr;`.
- **`NULL` is an alias of `nullptr`.**
- **Declaration detection is by shape.** A statement starting with a type keyword,
  `ptr`, a generic builtin or two identifiers in a row is a declaration; anything else
  stays an expression statement, so no backtracking is needed.
- **Hardening:** a type-first declaration is recognized only when the name is followed
  by `=` (or `;` for the missing-initializer diagnostic), lookahead is bounded by the
  token count, and a brace initializer on a non-struct type reports its own
  diagnostic and recovers at the closing brace.

Still unsupported: call-style construction `Point(1, 2)`, `Point p = {};` with no
initializer list, and uninitialized declarations (`int x;` is an error).

## Version 0.379-beta   

### Summary of Changes (since `0.378-beta`):

*Opened after two upgrades: qualified builtin generic annotations and `@Nullable`
narrowing.*

- **Qualified builtin generic type annotations (17.4).**
- **`@Nullable` pointer bindings (8.7).** Pointers are non-nullable by default;
  initializing one with `nullptr` requires `@Nullable`.
- **Dereference of a nullable pointer needs a proof.** `p is not nullptr` narrows the
  then-branch, `p is nullptr` narrows the else-branch and the code after an early
  exit.
- **Proofs are branch-scoped** and cleared by any assignment.

Not implemented: `@Nullable` on parameters, returns and fields; narrowing through
`&&`, `||`, `while`, ternaries or helper predicates; nullability as part of the type
itself (a nullable and a plain pointer share one TIR type); interpolation.

## Version 0.378-beta   

### Summary of Changes (since `0.374-beta`):

*Opened after two null-semantics upgrades.*

- **`nullptr` can be assigned to an existing pointer binding.** `p = nullptr;` retypes
  the literal to the destination pointer, so the generated constant is a null pointer
  rather than integer zero and printing still reports `NULL`.
- **`is` and `is not` compare a pointer with `nullptr`.** They lower to ordinary
  equality, so pointer operand rules are shared with `==` and `!=`. `nullptr` adopts
  the pointer type of the other operand; comparing `nullptr` with a non-pointer is a
  compile error instead of a silent integer-zero comparison.

Still unimplemented from §8: `@Nullable` annotations, flow-sensitive non-null
narrowing, interpolation of pointers, cross-function lifetime analysis, and the
interop rules §8.10 requires.

## Version 0.374-beta   

### Summary of Changes (since `0.371-beta`):

*Opened after the first implemented slice of the pointer surface in `SYNTAX.md` §8.*

- **`ptr T` declares a pointer, `&` takes an address and `*` reads or writes through
  one.** `ptr ptr int` adds a second level, so `**pp` works. Dereference lowers as
  `p[0]`, reusing the pointer-element addressing the index expression already uses.
  `&name` yields the existing stack slot's address: the value is neither loaded nor
  copied.
- **Only storage with a stable address and a safe type is addressable.** Variables,
  fields, elements and dereferences qualify; temporaries do not. Aggregates, owned
  arrays and managed references are rejected, because a raw alias into them could
  bypass deterministic destruction or the collector's write barrier. Dereferencing
  `managed<T>` is rejected for the same reason.
- **A pointer prints.** `print(p)` writes the address and never dereferences; a null
  pointer prints `NULL` through `nrq_print_pointer`, so absence is distinguishable
  from address zero.
- **`nullptr` is the null pointer literal.** It carries no intrinsic type; the
  declaration supplies one (`var p: ptr int = nullptr;`). `NULL` remains the
  nullable-value literal from §7.3 and is not implemented yet, and assigning
  `nullptr` to an existing pointer binding is not implemented yet either.
- **Fixed a compiler crash** found by the new tests: `check_compound_assignment` read
  `symbol->type` after allowing targets such as `*p`, for which no single named owner
  exists.

Still unimplemented from §8: nullable pointer types and `is NULL` checks,
interpolation of pointers, cross-function lifetime analysis, and the interop rules
that §8.10 requires the compiler team to document.

## Version 0.371-beta   

### Summary of Changes (since `0.369-beta`):

*Opened after the Stage 3 GC audit hardened the SoA layout pool.*

- **SoA layout descriptors are interned by element size as well as column geometry.**
  Two containers can share reference geometry while differing in payload per row; the
  shared descriptor used to give the second container the first one's element size, so
  tracing overstated the row count and read padding words as references.
- **Allocation refuses descriptors it cannot honour**: an element size that does not fit
  the descriptor field, or a column base that leaves no room for a pointer in one
  element.

## Version 0.369-beta   

### Summary of Changes (since `0.363-beta`):

*Opened after three upgrades: column addressing (2d), row bounds checks (2e), and
reference-column tracing (stage 3).*

- **A column element can be read and written.** `ps[i].field` lowers to a dedicated
  `SoaColumnPtr` op, because a column address multiplies two scales and an ordinary GEP
  would encode the array-of-structs offset. The element count comes from the object
  header, the same word the collector sizes the payload with.
- **A container element has no value.** Bare `ps[i]` is refused; only `ps[i].field`
  names a column, and the member access claims the index expression so nesting cannot
  leak the claim.
- **Every row is bounds-checked** against the header count before any address exists,
  through the same guard and shared cold failure block the owned-array path uses.
- **Managed references in columns are traced.** The layout descriptor gained (offset,
  stride) column pairs; tracing walks each column for the recorded row count, visiting
  only non-null entries. The flat offset table could never describe a column, and the
  gate now refuses only owned arrays and array views.

## Version 0.363-beta   

### Summary of Changes (since `0.361-beta`):

*Opened after three implementation upgrades: SoA stages 2a, 2b and 2c.*

- **`soa<T>` has one authoritative column layout.** `describe_soa` in
  `Lowering/DataLayout` derives every column base and element size from the struct's
  existing physical order, the same function that prints `--emit=layout`, so the
  report cannot drift from what the compiler actually does. Columns are described by
  count-independent quantities (`index_in_bytes` and `element_bytes`), so one
  description serves any element count.
- **The container resolves its element struct.** `soa<Point>` over a named aggregate
  resolves to the real field set at the point of use instead of carrying an opaque
  name through sema.
- **A container can be allocated: `memory.owned_soa<T>(n)`.** One GC object holds every
  column packed in physical order; the header records the element count and the
  payload is `count * element_bytes`. The value is a pointer to that block, and its
  release unregisters the root without freeing, because the block belongs to the
  collector.
- **Reference columns are refused with a named diagnostic.** A struct with a managed
  field cannot be a container element until stage 3 adds a trace callback that walks a
  column with stride; allocating one now would hide a live target from the sweep.
  `NRQ3015` names the offending column.
- **Column addressing is not open yet.** Indexing a container is still rejected, now
  with the analysis of why the ordinary two-step GEP cannot express it recorded in
  `docs/compiler/soa-progress.md`. Stage 2d is the next upgrade.

## Version 0.361-beta   

### Summary of Changes (since `0.357-beta`):

*Opened after 0.357-beta.*

- **`soa<T>` exists as a type and is gated.** `TypeKind::Soa`, `soa<Point>` in type
  position, `soa<Point>`/`SOA_` spelling and mangling, and a hard `NRQ5010` rejection
  of every use until allocation and strided tracing land. Stage 1 of four; the gate is
  the safety barrier and no program can allocate a column yet.

---

## Version 0.357-beta (released)

### Summary of Changes (since `0.353-beta`):

*Released after two runtime upgrades. The E4 benchmark and evaluation that landed
under this window are measurement, not upgrades, and are listed after them.*

- **The sweep resolves the page and the payload size once per object.** The reclaim
  path read `header->page` at eight uses and recomputed the payload size three times
  for one object; both are invariant within the object. Measured 3594 ms before and
  3598 ms after, best of five - neutral, because the compiler was already hoisting
  these loads. Committed for one resolution per object, not for a claimed speedup.
- **The sweep reads the state word once instead of three times.** Young, marked and
  the survivor's rewritten value come from one load, with the flags derived from it.
  Best of five 3594 -> 3471 ms, but the run-to-run spread is wider than that, so no
  speedup is claimed.
- Measurement that is not an upgrade: the AoS/SoA/AoSoA programs and the E4 report
  (`reports/performance/ohr-section-e4.md`).

- **The three layouts were measured on the same compiler.** Two million three-field
  records, two hundred column scans: AoS 642 ms, SoA 260 ms (2.47x), AoSoA 308 ms
  (2.08x), best of five. The effect is bandwidth - 16 MB of column against 48 MB of
  records - and it scales with the number of fields.
- **The explicit SoA form is defined rather than assumed:** a scope owning one array
  per column. Identity, references, aliasing, mutation, iteration, views and
  deterministic destruction are the ordinary owned-array rules, and a reference
  column is traced by the existing array reference-offset walk. The cost is named:
  element-address identity is not expressible.
- **No container type was built.** `soa<T>` needs type parameters the language does
  not have, so the report records that as the remaining work instead of shipping a
  per-record duplicate container. Ordinary structs and collections are unchanged.

---

## Version 0.353-beta (released)

### Summary of Changes (since `0.351-beta`):

*Released after two upgrades. Section E3 measured and acted on.*

- **Alignment buys nothing measurable on this core.** Cache resident, aligned and
  unaligned 16-byte loads both measured 0.157 ns/element, and an 8-byte-misaligned
  base measured 0.158. The 16 MiB run is bandwidth bound and cannot show an
  instruction effect, so it decides nothing.
- **The 16-byte floor on promoted stack arrays was removed**, since it bought no
  measured benefit and cost `bytes + alignment - 1` of frame per promoted array. An
  explicit `memory.owned_array_aligned` request still raises the slot.
- **Heap payload alignment was deliberately left at its natural 8**, where every
  object is aligned to its own elements; raising it would mean carving pages at 16
  bytes for a gain this machine does not show.

---

## Version 0.351-beta (released)

### Summary of Changes (since `0.346-beta`):

*Released after two upgrades. Section E2 investigated and closed with a negative
result.*

- **Nesting a struct in an array pays no extra stride.** An aggregate's size is
  already rounded up to its own alignment, so `length * size` leaves no gap between
  elements and there was nothing to tighten; recorded with the source citation
  rather than a speculative change.
- **Hot/cold field position was measured and does not pay.** Moving a hot field from
  the first cache line of a 128-byte record to the second moved a sequential scan by
  -7.5% and a random scan by -0.9% - the first contrary to the hypothesis and not
  reproduced by the second, so it is variance. Record size (1.83x for two lines),
  object order (2.6x random) and field separation (24x for SoA) were what moved the
  scan, and none of them is field order.
- **No reordering, indirection, header change or tracing change was made**, because
  no measurement supported one. The committed probe
  (`benchmarks/bottleneck_discovery/hot_cold/field_position.cpp`) is the harness that
  would have to show an effect to reopen it.

---

## Version 0.346-beta (released)

### Summary of Changes (since `0.341-beta`):

*Released after two upgrades. Opens Section E - object payload optimization.*

- **The layout report separates interior from trailing padding.** Only the first is
  waste; the second is what makes the stride a multiple of the alignment. The report
  also counts fields that begin past the first cache line, which is the quantity an
  E2 hot/cold decision would have to move.
- **A mixed-width struct is pinned at 16 bytes with no interior gap** (2 trailing,
  24 in declaration order), so an ordering change that starts inserting interior
  padding fails a test instead of showing up as a slower profile.
- **Section E1 inspected and recorded:** the LLVM struct type, access indices, GC
  reference-offset table and layout report all read the one `DataLayout` walk, and
  interior padding is zero by construction for descending power-of-two alignments.

---

## Version 0.341-beta (released)

### Summary of Changes (since `0.336-beta`):

*Released. Closes the object-header investigation and Section D, and opens
Section E. Superseded by 0.346-beta.*

- **The header question is closed with numbers.** Four candidates were laid out by
  the compiler: H48 at 48 bytes, H40 at 40 (legal only where `max_align_t` is 8),
  H32 at 32, and H36 - which is not a size, because 36 used bytes round to
  `sizeof` 40 and land on H40's spans while also narrowing the epoch. Span and
  occupancy for each were computed with the runtime's own formula after checking it
  against the allocation profile on ten payloads.
- **`page_of()` was measured rather than assumed:** mask 0.43-0.53 ns but wrong
  above a page quantum; the cheapest correct fallback 2.53-2.75 ns on ordinary
  pages and 4.89-5.21 ns on large ones, against 2.15-2.75 ns for the field it would
  replace; a per-quantum hash measured 60 ns and was disqualified.
- **Every header field was audited** for its read and write sites: `page` is
  written once and read 18 times, `mark_next` lives inside one mark cycle, and
  `next`/`tlab_next` are both live during marking.
- **H48 is authoritative and enforced in code:** the layout is documented above the
  struct, every field offset is asserted, and the alignment rule is asserted at the
  size. H32 is recorded as rejected, H40 as a documented target-specific
  possibility that is not implemented.
- **Section D audited and closed:** D1's twelve requirements, D2's preserved
  behaviours and D3's obligations each verified, with two gaps fixed - the layout
  was documented only in reports, and nothing checked a live object's layout
  reference. Production/debug equivalence measured: the release preset, with
  validation compiled out, runs the same 210 unit and 263 integration tests.
- **A page asserts it starts on its own size boundary**, which is the premise any
  address-masking page lookup would rest on.

---

## Version 0.336-beta (released)

### Summary of Changes (since `0.333-beta`):

*Released. Continues OHR Section B (compact per-instance metadata). Superseded by
0.341-beta.*

- **The allocation memo never retains the caller's offset table.** The memo cached
  the pointer to the table a caller passed and compared two calls by that pointer.
  The typed entry points document that the collector copies the table, so callers
  build it on the stack: a later allocation could read expired storage, and a
  rebuilt table at the same address was accepted as the previous table. The memo
  now holds only the interned descriptor and compares against the table that
  descriptor owns. The development validator no longer caches caller-supplied
  descriptors by address, for the same reason. Pinned by a test that gives two
  calls the same stack array with different contents, which fails on the previous
  code.
- The five Section B upgrades of 0.333 are listed above and unchanged: one masked
  state word, one write per survivor, one read per flag decision, the
  instance/shared split in the facts, and an assertable encoding.
- Section B is not finished. 210/210 unit, 263/263 integration.

---

## Version 0.333-beta (released)

### Summary of Changes (since `0.331-beta`):

*Released. Opened OHR Section B: compact per-instance metadata. Superseded by
0.336-beta, which continues the section.*

- **One state word with explicit masks replaces four flag bytes.** The marked,
  young and remembered flags and the promotion age were separate fields that the
  sweep rewrote one after another. They are now bits in a single word, defined by
  shifts the runtime owns rather than by C++ bitfield allocation, so the collector
  reads the word once and changes one field without touching a neighbour.
- **A survivor's word is written once.** Clearing the mark, incrementing the age
  and clearing the remembered flag were three read-modify-writes of the same
  word, each re-reading what the previous one had written; the sweep now computes
  the next word from the one it read and stores it once.
- **The flag hot paths read the word once.** Mark enqueue and remembered-set
  insertion each read the same word twice through accessors; both take one read.
- **The facts report the instance/shared split.** The header is 40 bytes that
  describe one instance and 8 that name the shared layout, measured from the
  struct's own field offsets so the split cannot drift from the definition.
- **The encoding is observable.** A diagnostic returns an object's raw state
  word beside its field masks, and a test asserts each field through its own mask
  rather than inferring the encoding from collection outcomes. 209/209 unit,
  263/263 integration.

---

## Version 0.331-beta (released)

### Summary of Changes (since `0.327-beta`):

*Released. Superseded by 0.333-beta, which continues OHR Section A.*

- **Roots describe themselves with the same immutable layouts as objects.** A
  root registration copied its reference layout into the shadow stack and again
  into the per-slot index, with an owned vector for anything wider than two
  offsets. Roots now share the interned descriptor, a conflicting
  re-registration is one pointer compare, and marking a root, the barrier's root
  walk, the reference-count diagnostic and the debug snapshot all read the same
  table.
- **Registration does not take the interning lock on a repeat.** Frame entry
  uses the same thread-local memo as allocation, so a layout this thread has
  already resolved costs one table compare and no allocation.
- **The per-instance reference layout is gone.** `ReferenceLayout`, its inline
  pair, its overflow vector and its matching helper had no callers left; deleting
  them makes the descriptor representation the only one.
- **Array allocation no longer divides per object.** The element count was
  derived by dividing the payload by the element size for a number the caller
  already had. 300,000 four-element arrays: 19.5 ms median before, 18.4 ms after
  (ranges overlap, so this is small; the division is gone in all cases).
- **A caller-supplied descriptor is validated, not cached.** The development
  configuration proves every field of a caller-supplied descriptor on each
  allocation. It is deliberately not memoised by address: a descriptor's
  address can be reused by different contents once the first one is gone.
  Production runs no check on an allocation path. Interned descriptors are a
  different case, and the allocation memo compares against their owned offset
  tables rather than against the caller's.
- **Wide array roots are pinned by a test:** three elements of four reference
  words survive a collection through one shared element layout.
- The measured detail and the remaining Section A work are in
  `reports/performance/ohr-section-a.md`. 207/207 unit, 263/263 integration.

---

## Version 0.327-beta (released)

### Summary of Changes (since `0.321-beta`):

*Released. Superseded by 0.331-beta, which continues the object-representation
stage (OHR Section A).*

- **Structural metadata is shared instead of repeated.** A new immutable
  `nrq_object_layout` describes a payload once - its size, whether it repeats,
  how to visit its references, and any custom trace callback - and every
  instance holds a pointer to it. Layouts are interned by every property that
  changes interpretation, so equal sizes with different reference order or
  different tracing never merge.
- **Allocation does not lock to find a layout.** Each thread remembers the last
  descriptor it used and proves a hit by comparing the offset table: a compiler
  constant table compares by pointer, a rebuilt one by value. Only a genuinely
  new shape reaches the interning table, and descriptors are never reclaimed,
  so a live object can always read its own layout.
- **The object header is 48 bytes, down from 96.** Tracing reads the reference
  map from the descriptor, so a wide record no longer allocates an overflow
  vector per instance, and the payload size and element size are derived from
  `element_count * layout->payload_size` instead of being stored. 100,000
  objects of a 64-byte payload now take 171 pages instead of 245: 408 to 585
  objects per page, 15.7 to 11.2 MB of page storage.
- **A page remembers twice as many reclaimed runs.** The denser header packs a
  page exactly, and the test that holds a live set steady across rounds exposed
  a pre-existing silent drop: a page recorded at most 96 free runs and threw the
  rest away, so churn lost reclaimed space and the heap grew a page per round.
  Run entries are now two 32-bit page-relative fields - the same 1.5 KiB holds
  192 runs - and the pattern stays at 2 pages where it used to reach 3.
- **Descriptor sharing is observable.** The allocation facts report the number
  of live descriptors and the bytes they occupy, and the allocation profile
  reports the 48-byte header, the 112-byte span for a 64-byte payload and 585
  objects per page. 1,000 instances add at most one descriptor and 64 bytes.
- **Limit recorded, not hidden.** A payload beyond 4 GiB, or an array with more
  than 2^32 elements, is rejected rather than represented, because the
  descriptor and the element count are 32-bit. `element_count` cannot truncate.

---

## Version 0.321-beta (released)

### Summary of Changes (since `0.318-beta`):

*Released. Superseded by 0.327-beta, which continues the allocator stage.*

- **A fragmented page's free list is walked once per refill, not once per run.**
  Taking runs restarted the walk for each one, re-reading tombstones and
  refreshing the page maximum each time. One tombstone-and-compaction pass now
  yields a batch and refreshes the maximum once: the fragmentation probe drops
  from 8.42 ms to 7.20 ms median, four mutators from 13.26 ms to 10.01 ms, and
  the mixed churn probe from 7.08 ms to 6.52 ms, with identical refills, pages
  and commit. See `reports/performance/ao-06-tlab-sizing.md`.
- **The cost of a reservation is observable.** New getters report the stranded
  tail (bytes and per-cycle maximum), the number of free runs, the pages that
  hold them, and the bytes handed out by span reuse, and every cursor install now
  charges the same accounting. Before this, a heap reported 394 MB committed for
  196 MB live and nothing explained the difference.
- **An unused reservation tail goes back to its page at the next safepoint.** It
  is returned while the collection is prepared (before the page generation moves,
  so no mutator can still hold the cursor) and on mutator leave: stranded tail
  74,048 -> 0 B per cycle, per-cycle maximum 62,144 -> 0 B, 61,280 B reclaimed and
  reused. Commit and peak working set are unchanged.
- **A reservation never ends part way through an object.** A grant now rounds down
  to a whole number of object spans, so the page tail stays free space instead of
  charged-but-unusable space: 32,672 B per page for a 32 KiB payload and
  195,999,328 B over 6,000 objects became 0, and 4 KiB payloads went from
  3,540,448 B of tail to 0, at identical page counts and commit. A tail index
  upper bound keeps the larger payloads from scanning pages they cannot use, which
  is what the first version of the change cost them (25%, now neutral).
- **TLAB size, page size and object header are unchanged, on measurement.** Refills
  are already one per page at every payload size measured (64 B: one per 408
  objects), the stranded tail is zero, and what remains is the 96-byte object
  header and page quantisation above half a page - neither of which a larger slab
  fixes. A larger slab would trade refills for stranded memory.
- **AO-07 handover: the allocation facts the header work needs are now callable.**
  `nrq_gc_get_allocation_facts` reports the header size, payload alignment, page and
  slab bytes, the minimum slab run and the spare-run capacity;
  `nrq_gc_get_allocation_profile` turns a payload size into its aligned span, the
  objects per page, the slack at the end of that page and the objects one
  reservation serves. A unit test pins the model to the allocator.

---

## Version 0.318-beta (released)

### Summary of Changes (since `0.315-beta`):

*Released. Superseded by 0.321-beta, which continues the allocator stage.*

- **The allocation slow path no longer proves a negative over the page index.** A
  refill that preferred a slab-sized run could only reject a short run by walking
  every indexed page, and in a fragmented heap that walk always failed and was paid
  once per four allocations: the second 200,000-object wave of the fragmentation probe
  took 920-940 ms against 7-8 ms when the first-fit short-run order is restored. The
  slab preference is now answered by an incrementally maintained per-page maximum run
  and a page counter, so the search is skipped in O(1) when no page can host a slab.
  See `reports/performance/ao-02-tlab-refill-order.md`.
- **Short reclaimed runs are reserved in batches.** The refill took one run per lock
  entry, which in a fragmented heap is one entry per 4.2 allocations. It now takes up
  to 24 runs under that one lock and hands them out from the thread without it, so the
  same probe drops from 47,090 locked refills to 2,102 and, with four mutators, from
  17.3 ms to 12.4 ms median (-28%). Reserved runs are returned to their pages at every
  cycle boundary and at thread exit, before the reclaimer can recycle a page.

- **AO-05 boundary audit:** each component of the allocation entry point (per-object
  cycle state, accounting, barrier protocol, payload zero fill, root-slot validation)
  was measured as below this machine's resolution, and the generated-code fast path was
  refused on that evidence. See `reports/performance/ao-05-tlab-boundary.md`.

- **The write barrier stays down for a non-generational program.** Allocating a
  non-young object re-armed the store barrier and the cycle never disarmed it, so a
  program with generations off paid the write-barrier path on every store for
  remembered entries no minor can use. `nrq_gc_store_barrier_active` exposes the policy
  state and a unit test pins it.

---

## Version 0.315-beta (released)

### Summary of Changes (since `0.311-beta`):

*Released. Superseded by 0.318-beta, which continues the allocator stage.*

- **AO-04: large-object pages are carved from runs at their own size.** Pages larger
  than 64 KiB still took their own `VirtualAlloc`/`VirtualFree` pair, so a heap of
  100 KiB arrays paid two system calls per object. They now come from the same 8 MiB
  runs, at their own capacity rounded to the commit granularity; the 64 KiB
  alignment is only imposed on ordinary pages, which are the ones the address-keyed
  directory indexes. 2,000 arrays of 100,000 bytes: first round min 82.7 → 47.5 ms,
  median 88.7 → 49.6 ms, private commit 201.2 → 203.8 MB. Carving them in whole
  64 KiB slots was rejected first: it rounded 100 KiB up to 128 KiB and took commit
  to 256 MB.
- **AO-05: the allocation size is validated once, at the runtime boundary.** The
  entry points checked the payload size and the thread-local bump path repeated the
  same checks; the bump path now only requires that the thread is a mutator, and the
  untyped entry gained the check it had relied on the callee to make.
- **AO-05: a non-generational program keeps the write barrier down.** Allocating a
  non-young object re-armed the store barrier, undoing `nrq_gc_set_generational(0)`
  at the next allocation, and a cycle re-armed it and never disarmed it; both left a
  program with generations off paying the write barrier on every store for remembered
  entries no minor can use. The old-object count moved into the publication walk, so
  counting an object no longer costs a locked instruction on the allocation path.
  `nrq_gc_store_barrier_active` exposes the policy state and a unit test pins it.
- **AO-05: the generated-code TLAB fast path was audited and refused.** The runtime
  call boundary was measured with diagnostic builds that remove the per-object cycle
  state, the accounting update, the barrier protocol, the payload zero fill and the
  root-slot validation one at a time: no variant moved the median, so the work a
  generated fast path could remove is below this machine's noise floor, while the
  invariants it needs are per-thread state that would have to become a frozen public
  ABI. See `reports/performance/ao-05-tlab-boundary.md`.

---

## Version 0.311-beta (released)

### Summary of Changes (since `0.301-beta`):

*Released. Superseded by 0.315-beta, which continues the page-storage stage.*

- **AO-04: allocation reuses the space a sweep freed before the next cycle
  boundary.** An emptied page still cannot be recycled while a mutator may hold a
  cursor into it, but its reclaimed runs are safe to hand out: the sweep skips
  objects born after the cycle started, and clearing the retirement mark keeps the
  boundary from reclaiming a page that is in use again. 1,000,000 rooted
  allocations per round: private commit after the second round 241.6 → 122.0 MB,
  peak working set 247.6 → 128.4 MB, second-round allocation min 72.1 → 22.2 ms
  and median 74.4 → 24.3 ms, page count unchanged at 1,833.
- **AO-04: ordinary page storage is carved from 8 MiB runs.** A growing heap used
  to pay a `VirtualAlloc` reservation and a `VirtualFree` release per 64 KiB page;
  pages now come from runs committed in 1 MiB chunks and carved into aligned
  64 KiB slots, with released slots reused through a bounded free list. First
  (heap-growing) round: min 66.6 → 45.7 ms, median 69.9 → 50.8 ms; steady state
  unchanged; private commit 122.0 → 122.5 MB of bounded chunk slack.
- **AO-04: the idle page pool is bounded and decommitted slots are reusable.** The
  recycle pool held up to 2,048 shells (128 MB committed) after a program stopped
  allocating; it now holds one run's worth (128), and a page that leaves the pool
  decommits its slot while keeping the address for a later wave. Idle private
  commit 145.0 → 19.4 MB, returning to 146.4 MB on a full second wave.

---

## Version 0.301-beta (released)

### Summary of Changes (since `0.298-beta`):

*Released. Superseded by 0.309-beta, which continues with the allocator stage.*

- **AO-03: the allocation counters were audited and the exactness contract is
  documented.** Every counter is now classified as exact-immediately,
  exact-after-flush, internal trigger state or debug telemetry, with the writers and
  readers of each recorded in `reports/performance/ao-03-allocation-accounting.md`.
  No public statistic became approximate and no trigger boundary moved.
- **AO-03: the trigger counter is updated with a store, not a compare-exchange.** All
  three writers of `bytes_since_collection` run under the heap lock, so the retry loop
  could never fail; the flush now does a saturating load and store and the
  locked-instruction loop is gone. No observable change; not claimed as a speedup.
- **AO-03: evaluating the collection trigger once per batch instead of once per
  allocation was measured and refused.** A diagnostic build that skipped the per-object
  phase, threshold and counter reads was slower, not faster (min 25.5 → 26.8 ms,
  median 59.3 → 60.3 ms over 18 interleaved samples), so the fast-path reads stay and
  the trigger is still evaluated per allocation.

---

## Version 0.298-beta (released)

### Summary of Changes (since `0.294-beta`):

*Released. Superseded by 0.301-beta, which continues the allocator stage.*

- **AO-02: GC page storage is committed and released per page.** Pages went through
  `malloc`/`free` and a pool, which left the commit charge of an abandoned page in
  the process. Each page is now committed with `VirtualAlloc`/`VirtualFree`. Private
  commit at 1,000,000 objects: 238 → 122 MB (474 → 242 MB in the second round), page
  count unchanged at 1,833. Memory only; no timing claim.
- **AO-02: the allocation-size guard multiplies instead of dividing.** Every array
  entry point ran a 64-bit `divq` to prove `count * element_size` fits; the high half
  of the product answers the same question. Timing unchanged inside the noise, so
  this is removed instructions, not a claimed speedup.
- **AO-02: the pending counters keep an owner-side mirror.** Only the owning mutator
  writes those counters, so the load before each store read a value the thread already
  had. The exact value is still published after every allocation.
- **AO-02: pages with tail space are indexed instead of rescanned.** A TLAB refill
  walked all 1,833 pages backwards, in a workload where every page is filled to
  capacity, so the walk never found anything. Allocation order is preserved and the
  page and commit counts are identical; 1,000,000 rooted allocations: min 30.0 → 24.9
  ms, median 66.5 → 60.9 ms over 18 interleaved samples each.
- **Measured and reverted:** 128 KiB TLABs (collection 23 → 133 ms, pages stopped
  being recyclable) and an inline memset replacement (neutral, added a branch).
  `reports/performance/ao-02-tlab-publication.md` records both.
- **AO-02 stage total:** 1,000,000 rooted allocations 45.1 → 24.9 ms minimum (−45%)
  since the stage began.

---

## Version 0.294-beta (released)

### Summary of Changes (since `0.291-beta`):

*Released. Superseded by 0.298-beta, which continues the allocator stage.*

- **AO-02: publication stops dirtying published headers.** The publication loop
  cleared the batch link and the published link of every object, so the traversal
  paid a write-back for every header cache line it read. Neither clear is needed.
  Publication of 1,000,000 objects: min 9.75 → 8.45 ms, median 13.87 → 12.54 ms
  (`reports/performance/ao-02-tlab-publication.md`).
- **AO-02: the runtime is built with the full inliner.** `RelWithDebInfo` compiles
  with `/Ob1`, which left the tiny accessors behind the GC hot path as out-of-line
  calls into the C++ library: three in `begin_fast_barrier` alone. The runtime
  directory now uses `/Ob2` for that configuration, with optimization level, debug
  information, checks and diagnostics unchanged. 1,000,000 rooted allocations:
  min 41.4 → 30.4 ms; Stress 03: BUILD median 128 → 89 ms, COLLECT 36 → 31 ms.
  This resets the runtime baseline: earlier reports were taken at `/Ob1`.
- **AO-02: barrier region and root-slot probe inlined.** The rooted entry point
  makes four calls per allocation instead of six; min 28.4 → 27.4 ms on the probe.
- **Measured and reverted:** page-run counter/statistics aggregation and
  word-batched start-bitmap writes were both neutral or worse in an interleaved
  A/B, so neither is in the tree.

- **BO-04: redundant root initialisation removed.** A rooted managed slot was
  zeroed twice — once by the frame preamble before publication and again at its
  alloca point. The alloca-time store is gone when the preamble is guaranteed to
  run (a root frame exists); it stays otherwise, and unobserved slots keep theirs.
  `build_tree`: 55 → 54 instructions, one store per activation removed
  (2,097,151 stores per Stress 02 run). No runtime speedup is claimed
  (`reports/performance/bo-04-root-init.md`).
- **Loop frame reuse:** audited, nothing to hoist — frames are already opened once
  per activation. Pinned by the `gc_loop_frame_once` test.
- **Tests:** `gc_root_null_init` (exactly one zero store per rooted slot) and
  `gc_loop_frame_once` (one frame per activation); unit 194/194, integration
  263/263, Stress 02/03 semantics unchanged.

---

## Version 0.291-beta (released)

### Summary of Changes (since `0.289-beta`):

*Released. Superseded by 0.294-beta, which implements the BO-04 removal.*

- **Handoff-fusion pass removed.** The pass required release-then-publish and
  every lowering site emits publish-then-release, so it could never match; a dead
  rewrite that would silently activate on a future emission change is worse than
  none. Reordering the lowering to feed it was rejected: it would release a pin
  before publishing the value, and any handoff the pass declined would leave a
  value unpinned and unrooted. `nrq_gc_root_store_release` stays as documented ABI
  and keeps its unit test (`reports/performance/bo-03-handoff-fusion-audit.md` §8).
- **Effect:** emitted IR for tests 01–03 changes only in where an unpin sits among
  non-call operations (no call or safepoint is crossed); `build_tree` assembly
  keeps its 55 instructions with different register shuffles; unit 194/194,
  integration 261/261, Stress 02/03 semantic output unchanged.
- **BO-04 audited** (`reports/performance/bo-04-root-init-audit.md`): every rooted
  slot is zeroed twice — once by the frame preamble before publication and again
  at the alloca point — and the duplicate is dominated with no write in between,
  so it is removable under a local structural proof. Frames already open once per
  activation, so loop frame reuse has nothing to hoist.

---

## Version 0.289-beta (released)

### Summary of Changes (since `0.286-beta`):

*Released. Superseded by 0.291-beta.*

- **Handoff fusion audited to completion**
  (`reports/performance/bo-03-handoff-fusion-audit.md`): the pass runs at the end
  of every function translation, matches only release-then-publish, and every
  lowering site emits publish-then-release — so it fires on no current output.
  No second pass and no second runtime entry were added.
- **`nrq_gc_root_store_release` covered** — `GcTest.StoreReleaseRootsBeforeReleasingThePin`
  is the only way the entry is exercised: publication before release keeps the
  object alive through a collection, a null slot releases only the pin, and
  clearing the slot releases the object. Unit suite 194/194.
- **Emission order pinned** — integration `gc_handoff_order` asserts
  `pin_transfer` → `root_store` → `unpin` and the absence of the fused call, so a
  future reorder is noticed instead of silently activating a dead pass.
  Integration suite 261/261.
- **No timing claim:** with no emitted fused form there is no call-site
  difference to measure.

---

## Version 0.286-beta (released)

### Summary of Changes (since `0.281-beta`):

*Released. Superseded by 0.289-beta, which finishes BO-03.*

- **Fused frame open covered by tests** — `GcTest.FusedFrameOpenPublishesStaticRootsAndNests`
  exercises the new entry directly: roots it publishes survive a collection, a
  nested fused frame leaves the outer roots registered, and a zero-slot frame
  opens and ends with a distinct marker. Unit suite 193/193.
- **New integration case** `gc_fused_frame_open`: a recursive program with four
  static roots per activation, asserting the fused call is emitted, the split
  registration is not, and the result survives a collection at every level.
  Integration 259/260; the one failure (`memory_advanced`) predates this version
  and fails in the memory-mapping path.
- **Evidence** (`reports/performance/bo-02-fused-frame-open.md`): one runtime call
  site fewer per batched activation (`build_tree` 12 → 11 call sites, 10 → 9
  runtime calls per internal node, 56 → 55 assembly instructions), no timing
  claim, and the full correctness-coverage matrix for the changed ABI.
- **Deliberately not done:** descriptor-backed root iteration and removing the
  frame vector or the slot indexes — reasons recorded in the report; none of them
  had a provable win at these slot counts.
- **`memory_advanced` repaired:** the case had been failing since the 0.271 bump
  because it pinned the literal `0.266-beta-phase6` and the byte length of its
  own `expect.json`. Expectations may now write `{version}`, which the runner
  resolves from the compiler under test, and the self-referential size assertion
  is gone. Integration suite: 260/260.
- **BO-03 handoff-fusion audit** (`reports/performance/bo-03-handoff-fusion-audit.md`):
  the pass still runs, but it matches release-then-publish while every lowering
  site emits publish-then-release, so it fires on no current output. Its runtime
  entry does not merge barrier regions; both routines it calls open their own.
  The source comments claiming a merged region are corrected; the entry and pass
  are kept, because removing a documented ABI entry is an owner decision.

---

## Version 0.281-beta

### Summary of Changes (since `0.275-beta`):

*Released. Superseded by 0.286-beta, which continues BO-02.*

- **BO-01 verified** (`reports/performance/bo-01-memory-effects.md`): the GC
  memory-effect contracts landed in 0.275 produce instruction-identical output
  for tests 01–03, so that branch is stopped with no performance claim. The only
  observable difference is that LLVM can now prove some generated functions
  cannot throw, which drops their SEH unwind entries while leaving every
  instruction unchanged.
- **BO-02 audited** (`reports/performance/bo-02-frame-audit.md`): the static
  descriptor, trusted/validated split, batched registration, allocation-free
  layout storage and open-addressed slot index already exist; the remaining
  structural cost was the split frame protocol, two runtime calls per activation
  for one operation.
- **Fused frame open** (`nrq_gc_root_frame_open`): generated code can now open a
  frame and publish that frame's static roots in one runtime call. The frame
  record is still pushed before any root is published, and a registration failure
  terminates before anything is observable. Single-slot and zero-slot frames keep
  the separate begin call, where a batched slot array would cost more than it
  saves.
- **Lowering** emits the fused call, erases the eager begin call and rebinds the
  marker every frame-end already reads; IR and assembly show one call site fewer
  per batched activation (`build_tree`: 10 → 9 runtime calls per internal node,
  56 → 55 assembly lines).
- **Tests:** unit suite 192/192; integration 258/259 — `memory_advanced` was
  already failing before this version (its expectation was last updated at
  0.260 and the test aborts in the memory-mapping path, unrelated to GC frames).
  Semantics of Stress 02/03 are identical to the pre-change binaries.

---

## Version 0.275-beta 

### Summary of Changes (since `0.271-beta`):

*Released. Superseded by 0.281-beta, which continues BO-01.*

**BO-01 — runtime memory-effect contracts for the GC protocol (first five steps).**
Compiler-only change; the runtime library, generated code shape and language are
unchanged. The lowering already classified the non-GC runtime but left every GC
entry point without a `memory(...)` attribute, so LLVM had to assume each root
call could read and write any memory, including the caller's locals.

- **Contract table audited** (`reports/performance/bo-01-contracts.md`): every
  emitted GC entry point is classified from its implementation in `gc.cpp`, not
  from its name — argument dereferences, TLS/global state, allocation, collection,
  pin state, callbacks and failure paths.
- **Root-frame protocol** (`root_frame_begin`, `root_frame_end`): `argmem: none`,
  global/TLS effects `ModRef`, errno and target memory untouched.
- **Root publication and registration** (`root_store`, `root_store_release`,
  `root_copy`, `root_register`, `root_register_typed`, `root_register_frame`,
  `root_register_frame_static`, array-root registration): argument memory is read
  and written by contract, so the declarations now say exactly that instead of
  leaving all effects unspecified. `root_unregister` and `unpin` are
  `argmem: none` — they only compare the address.
- **Allocators and pins** (`nrq_gc_allocate*`, `nrq_gc_allocate_array_typed_rooted`,
  `pin`, `pin_transfer`): managed allocators take `argmem: none`; the rooted
  allocate and the pin entries read/write the argument memory they really touch.
- **Mutator, collection and statistics** (`mutator_enter/leave`, `safepoint`,
  `collect`, `collect_minor`, `init`, `shutdown`, policy switches, counters):
  `argmem: none`.
- **Not narrowed:** `other` and `inaccessible` memory stay `ModRef` everywhere in
  this group — the shadow stack, heap bookkeeping, pin table and mutexes are
  runtime state, and no entry point is claimed side-effect free.
- **Verification so far:** emitted IR carries the expected attributes for both
  production and development declarations; the unit suite passes 192/192. No
  assembly change was observed at the `build_tree` call site with `llc -O2` on the
  emitted IR, which is recorded honestly rather than presented as a win; the
  acceptance measurement for BO-01 is the first task under 0.281-beta.

---

## Version 0.271-beta

### Summary of Changes (since `0.269-beta`):

*Superseded by 0.275-beta (BO-01 memory-effect contracts).*

- **No runtime, compiler or language change.** Every file that produces the
  binary or the collector is identical to 0.269-beta; this release carries the
  corrected performance record.
- **Test-03 regression bisected** (`reports/performance/colo-07-residual-attribution.md`):
  the whole step belongs to `e830b65` (sweep pages in address order, one bitmap
  bit at a time: +28 ms, 10.04 M zero-bit iterations per collection). The ColO-01
  set-bit sweep removes it entirely.
- **Residual withdrawn.** The earlier claim that test 03 is 9.6 % slower than
  0.257 came from session drift and is not reproduced: a 16-round interleaved A/B
  measures 24.5 ms (0.257) against 25.0 ms (0.271), and test 02 is 85.0 against
  79.5 ms in the same protocol. `[MEASURED]`
- The stage-03 sweep target (12 ms) is recorded as sitting on this machine's
  measurement noise floor: 12.5 ms in one session, 10.54 ms in another.

### Tests

`nirqoba_unit_tests.exe`: 192/192 passing.

---

## Version 0.269-beta

### Summary of Changes (since `0.266-beta`):

*Checkpoint; superseded by 0.271-beta, which changes no code.* The 0.266
collector regression is removed and the
collector optimisation roadmap (`project_context/ColO_ROADMAP_2.md`) is
implemented:

- **Set-bit sweep** (`25224b3`): the sweep walks only set bits of each page's
  object-start bitmap, so the zero-bit inner loop no longer exists.
- **Bounded sweep chunks** (`9d5b6ac`): a chunk stops after 4,096 bitmap words
  (or its object/byte budget) and charges each page it enters, so sparse pages
  cannot make a chunk walk unbounded bitmap. Debug builds can emit per-chunk
  work and duration events; production compiles them out.
- **Handoff-fusion compile fix** (`43c223f`): the committed fusion in
  `Lowering/TirToMlir.cpp` builds `nrq_gc_unpin` through `ensure_runtime_fn`, so
  programs that reach the fusion compile again. The fusion itself is part of
  0.269 and costs about 2--3 % on build time.
- **Validation**: tests for bridging free-run release and for minor retention of
  unreachable old objects; a fragmentation/reuse workload and a multithreaded
  stress workload in the diagnostic harness.

### Performance (measured, interleaved runs)

Stress Test 2, same 0.269 compiler driver, only the runtime library differs
(`reports/performance/colo-final-integrated.md`):

| Test | 0.257 (`98a1099`) | 0.260 (`904aee0`) | 0.269 | Change vs 0.260 |
| --- | --- | --- | --- | --- |
| 02 mark and sweep, `COLLECT` median | 93.5 ms | 158.0 ms | **90.5 ms** | -42.7 % |
| 03 cycles, `COLLECT` median | 24.5 ms | 58.0 ms | **25.0 ms** | -56.9 % |

The 0.257 and 0.269 03 figures are from the dedicated 16-round A/B
(`colo-07-residual-attribution.md`); the single eight-kit session reported
26.0 and 28.5 for the same two kits, which is drift.

Stage probe (production flags): mode 02 sweep 21.0 ms per collection with zero
zero-bit iterations (was 96.6 ms); mode 03 sweep 12.5 ms (was 31.8 ms). The
same-session interleaved probe (`colo-07` §3) measures 0.257 at 8.98 ms of
sweep and 0.269 at 10.54 ms, with the mark phase 1.66 ms cheaper, i.e. parity
in collection total.

### Known Issues

1. **Test 03 is at parity with 0.257, not 9.6 % slower.** The integrated run
   reported 28.5 ms against 26.0 ms, but a dedicated 16-round interleaved A/B of
   the two kits measured **24.5 vs 25.0 ms (+0.5 ms, +2.0 %)**, within spread
   (21--27 vs 21--29). The +2.5 ms was session drift and is corrected here. The
   original 03 regression is bisected to `e830b65` (per-bit zero scan, +28 ms)
   and is removed by the set-bit sweep (`reports/performance/colo-07-*.md`).
   Surviving structural cost, in the harness workload only: sweep +1.56 ms per
   collection for page-bitmap word reads, offset by 1.66 ms less mark.
2. **Mode 03 stage sweep sits around its 12 ms target, and the verdict depends on
   the session**: 12.5 ms in the ColO-02 session (a 4 % miss), 10.54 ms in the
   interleaved ColO-07 session (a pass). The probe is single-process and the
   machine drifts, so the target is neither provably met nor provably missed.
3. **No multithreaded scaling claim.** The stress harness measures four mutators
   doing equal total work 22 % slower than one mutator; allocation-bound
   throughput does not scale across threads. No synchronisation was weakened.
4. **Minor-collection cost is unmeasured.** Correctness is covered; the page
   skip's performance remains UNVERIFIED and was not optimised.

Full evidence: `reports/performance/colo-01-set-bit-sweep.md`,
`colo-02-bounded-sweep.md`, `colo-03-reclamation.md`,
`colo-04-minor-generational.md`, `colo-05-fragmentation.md`,
`colo-06-multithreaded.md`, `colo-final-integrated.md`.

### Tests

`nirqoba_unit_tests.exe`: 192/192 passing.

---

## Version 0.266-beta (defective)

> **Defect notice.** This release is known-defective and is superseded by
> 0.298-beta. It introduced unexpected performance regressions in the
> collector domain and must not be used for performance comparisons or as a
> baseline. The defects are understood, documented below, and are being fixed
> by the author as part of an architecture restructuring. No new feature work
> should be based on this tag.

### Summary of Changes (since `0.260-beta`):

- **Narrow-layout marking** (`5d62abf`): enumerate a narrow record's children
  without the trace callback.
- **Investigation/docs** (`904aee0`): Java-gap analysis.
- The 0.260 collector work described below is included unchanged.

### Known Issues

1. **Sweep scans zero bits one at a time (severe).** `sweep_chunk_locked`
   tests every bit of each page's object-start bitmap instead of iterating set
   bits. With 96-byte headers and 24-byte payloads, a full 64 KiB page holds
   ~546 objects in 8,192 bit positions, so ~94 % of iterations test zero. On a
   2.1 M-node graph this is **28.65 million zero-bit iterations per
   collection**, costing ~62 ms per collection. Measured: test-02 collection
   rose from 25.1 ms of sweep (86 ms total) to 87.5 ms of sweep (197 ms total);
   test-03 sweep rose from 9.8 ms to 29.2 ms. This is a **2.3–3.9× collection
   regression** and is the primary reason this release is defective.
2. **The slice budget does not bound sparse-page work.** `touched` counts object
   bytes, not bitmap words. A page with a single live object still scans all
   8,192 bits while charging 120 bytes, so a sweep chunk can hold `gc_mutex`
   for far longer than the 256 KiB budget implies.
3. **Release artifacts do not describe this version.** The committed
   `benchmarks/stress_test_2/benchmark_results.json` was generated before the
   0.260 collector commits and is carried forward unchanged; it reports
   pre-regression numbers for post-regression code. It also records
   `compilers: unknown` and `memory_bytes: null`.
4. **Minor-collection page skip and reusable-run page index are unexercised.**
   No committed workload creates old→young references or partially occupied
   pages, so neither `1646b93` nor `1ca3c37` is validated by the suite.
5. **Byte-sized mark/sweep slices did not restore performance.** They reduce
   collector-lock rounds (31,969 → 937 per collection) but not sweep time,
   because the cost is inside each chunk.
6. **Narrow-layout marking produced no measurable improvement** on the official
   workloads (< 0.2 ms, within noise).

### Disposition

Ahnaf is aware of these regressions and is restructuring the architecture. The
collector defect will be fixed by the author; the immediate correction is
scheduled for **0.269-beta** and shipped in 0.271-beta; **0.301-beta** is the working
build from this point on.
Do not tag, benchmark, or ship 0.266-beta.

---

## Version 0.260-beta

> **Post-release note.** This release carries a stale benchmark artifact
> (generated 2026-11-08T03:13, seven hours before the collector commits) and
> introduced the sweep regression documented under 0.266-beta. Use 0.301-beta
> for any measurement.

### Summary of Changes (since `0.257-beta`):

#### 1. Collection Path, Step One (ColO-01, C1-C5)
- **Free-span list integrity:** a freed run that bridged two recorded runs closed the hole in the page's ordered run list by moving the last entry into it, which left the offsets unsorted. Later frees then landed in the wrong slot, adjacent runs stopped merging, and a page fragmented while still reporting its bytes as usable - the per-round collection drift BO-01 section 4 left open. The hole now closes by shifting the tail, and a unit test holds a fixed live set across four allocate/free rounds and requires the page count not to grow.
- **Page index for reusable runs:** pages that record a freed run sit in a compact index with a constant-time swap-remove, so first-fit reuse and thread-local refills no longer walk the page set. The cycle boundary also skips its page walk when the previous sweep emptied no page.
- **Page-ordered sweep:** allocation no longer links every object into a heap-wide list. The sweep walks the page set in allocation order and each page's start bitmap in address order, so a page's dead runs are released in increasing offset order and each release extends the run recorded before it. Survivors stay where they are, and an oversized page keeps a small list of its own.
- **Young-only minors:** each page carries a young-object count, and a minor passes over a page that holds none, so a minor costs O(young pages) instead of O(heap).
- **Byte-sized slices:** mark and sweep slices are bounded by bytes of heap touched instead of 64 objects, so the number of collector-lock rounds no longer grows with the object count.

---

## Version 0.257-beta

### Summary of Changes (since `0.251-beta`):

#### 1. Builder Path, Step One
- **Static frame root tables:** the compiler emits the root-slot table for a `nrq_gc_roots_*` frame itself, so lowering calls `nrq_gc_root_register_frame_static` with a table it built at compile time instead of publishing slots one at a time through the dynamic entry point.
- **Validation without a release cost:** the static entry point trusts the compiler's table in the production configuration; the development configuration validates it and memoises the result, so a malformed table is still caught where diagnostics are expected to cost something.
- **Parity is tested:** a unit test drives both entry points over the same frame shape and requires identical published slots, and the seven integration cases that inspect the emitted call were updated to the static symbol.

#### 2. Profile-Guided Optimization
- **A missing profile runtime is no longer a test failure:** the pinned LLVM/MLIR SDK ships compiler-rt separately, so a toolchain without `clang_rt.profile-x86_64.lib` cannot link `--pgo-generate` at all. The two round-trip cases now report `SKIP` with the missing prerequisite, and the runner counts skips apart from failures.
- **Honest diagnostic:** `NRQ5003` no longer tells the user to re-run `scripts/bootstrap.ps1`, which cannot supply compiler-rt; it names the layout the runtime must be installed into. `scripts/bootstrap.ps1` reports whether that layout is present.

#### 3. Documentation
- **`reports/performance/bo-02-builder-status.md`:** what the builder path costs today, which items remain, and the precondition each one needs.

---

## Version 0.251-beta

### Summary of Changes (since `0.246-beta`):

#### 1. Two Configurations That Cannot Be Mixed
- **Runtime names corrected:** the libraries are `nirqoba_runtime` and `nirqoba_runtime_debug` (they were misspelled `nrqoba_*`), including the library file the driver links and the diagnostic it prints when the pair does not match the chosen configuration.
- **Link-time ABI identity:** each configuration's lowering calls its own ABI symbol from the entry point (`nrq_runtime_abi_v1_production` / `nrq_runtime_abi_v1_development`), and both libraries define the same configuration marker. An object built for one configuration cannot be linked against the other, and linking both libraries at once is a duplicate-symbol error instead of a coin toss.
- **One value per configuration:** `--debug-enabled` is read once into a `RuntimeConfiguration` that derives the emitted instrumentation, the ABI symbol, the library file name and the diagnostic's target. The runtime side derives the same facts from one macro, published `PUBLIC` on each target so it travels with the library.
- **Agreement is tested:** a unit test compares the compiler's derived names against the runtime's in a binary that links both, so a rename on either side fails there instead of in a user's link.

#### 2. What Belongs in Which Configuration
- **`07-diagnostics-classification.md`:** every mechanism is classified as essential correctness, accuracy-critical or pure telemetry, with the rule that decides the class and the proof obligation for removing a check.
- **Invariant audit:** all 92 fatal checks were audited - none sits behind a configuration guard, none is decidable at compile time, and both libraries are built from one source list. `scripts/check_runtime_check_parity.py` now runs after every build and fails if either property is ever broken.
- **Reference diagnostics:** the five `diagnostics_reading_roots()` call sites are audited and recorded as accuracy-critical, because `memory.get_ref_count` publishes their result; the root snapshot is a user of that handshake, not its owner.

#### 3. Statistics and Equivalence
- **Exactness under concurrency:** `gc.get_statistics()` stays exact for unpublished TLAB batches, now verified from a second thread. The cheaper accounting schemes considered are documented with the reason each was rejected.
- **Configuration equivalence:** `scripts/compare_configuration_equivalence.py` builds every integration program for both runtimes and requires identical exit status, stdout and stderr; 119 programs agree. Its first run found a lowering defect where string-literal globals restarted their names per function, which failed every development build of a module with two literal-using functions.

#### 4. Scope Correction
- The socket runtime was implemented ahead of its planned phase and is unlinked: it leaves the runtime source list and the link line, and its unit test is excluded. The source stays in the tree for the phase that owns it.

---

## Version 0.246-beta

### Summary of Changes (since `0.235-beta`):

#### 1. New Debug-Oriented Runtime Configuration
- **Introducing a new debug-oriented configuration:** The compiler now accepts a `--debug-enabled` flag that selects a development configuration end-to-end. This flag is the only way to enable the debug configuration; it is not enabled by default and must be explicitly requested.
- **`build`, `run` and `check` all understand `--debug-enabled`:** The help text explains what the flag does, and the configuration propagates through the entire toolchain.
- **Two libraries from one source list:** `nirqoba_runtime` (renamed from misspelled `nrqoba_runtime`) remains the default production library; `nirqoba_runtime_debug` (renamed from misspelled `nrqoba_runtime_debug`) is built from the same sources with the development configuration defined. The choice is made at link time, so a production binary carries no branch, counter, or symbol for instrumentation it does not use.

#### 2. Production Path
- **Probe telemetry removed:** Slot-index probe length and rehash/growth/compaction counts are read only by tests and by development tooling, so the production build no longer records them. The probe chain itself is unchanged.

#### 3. Development Path
- **Event stream:** The development runtime records allocation, cycle begin/end, promotion, sweep chunk, and root frame events, each with a globally ordered sequence number and a monotonic timestamp, into per-thread rings that never block an allocator.
- **Allocation-site tags:** The compiler declares each lowered managed allocation's source position and tags the object it produced. Site IDs are a stable hash of file, line, and column, so a trace maps back to source without the compiler keeping a table.
- **Root snapshot:** The development runtime reports every registered root slot, its layout, and the object it currently holds, reusing the reference-diagnostic handshake so it never reads a stack that is being published lock-free.
- **Recording is opt-in:** Nothing is recorded until a consumer sets an event mask, so an ordinary development build pays one relaxed load per candidate event.

#### 4. Documentation
- `docs/architecture/06-runtime.md` states the rule that decides which configuration a mechanism belongs to, what the development library adds, and what a trace costs.

---

## Version 0.235-beta

### Summary of Changes (since `0.231-beta`):

#### 1. Builder Bookkeeping (BO-01 head start)
- **Descriptor validation memo:** a frame's root descriptor is validated once per thread and per descriptor content instead of on every activation; a hit requires identical words, so a stack descriptor that reuses a validated address is re-validated rather than trusted.
- **Single-probe slot registration:** registering a root slot performs one index probe instead of a lookup followed by an insertion.

#### 2. Allocation Path
- **Barrier regions without locked instructions:** entering a mutator barrier region updates the thread's own counter with a plain store plus a fence instead of a locked read-modify-write.
- **Allocation counters:** the per-mutator byte and object counters are published with a load and a store instead of two locked read-modify-writes, while staying immediately visible to a statistics query.
- **Idle-heap field stores:** a store to a root slot skips the reader-count handshake while no cycle is running, because a cycle only snapshots roots after the mutator parks.
- **Header construction:** each object header field is written once instead of value-initialising the header and overwriting it.
- **Pointer-root memo:** the rooted allocation path answers its slot check from a small per-thread memo that is retired together with the registration it describes.

#### 3. Collection Stability
- **Reclaimed runs:** a thread-local reservation consumes the whole run a page offers, up to a full TLAB, instead of a fixed 4 KB slice. The slice had been re-fragmenting recycled pages, forcing a reservation every few dozen objects, and pushing later allocations onto fresh pages - which made sweeping cost more cycle after cycle.

---

## Version 0.231-beta

### Summary of Changes (since `0.228-beta`):

#### 1. Allocator Major Optimizations
- **Major Optimizations:** Significant performance improvements to the allocator subsystem, reducing allocation overhead and improving throughput.
- **Stress Test 2 Rerun:** Benchmark stress test 2 rerun to validate allocator improvements.

#### 2. SlotIndex and Stress Test Fixes
- **SlotIndex Feature Fix:** Modified the SlotIndex implementation to correctly track and index stack slots during root analysis.
- **Stress Test 2 Script:** Edited the stress test 2 script for improved benchmarking stability.

---

## Version 0.228-beta

### Summary of Changes (since `0.223-beta`):

#### 1. SlotIndex Refinement and Stress Test Fixes
- **SlotIndex Feature Fix:** Modified the SlotIndex implementation to correctly track and index stack slots during root analysis.
- **Stress Test 2 Script:** Edited the stress test 2 script for improved benchmarking stability.

---

## Version 0.223-beta

### Summary of Changes (since `0.217-beta`):

#### 1. Collection Progress and Runtime Lock Ordering, completed (PO-27 �2.5)
- **Root snapshot completeness:** A cycle folds in what mutators have not published, so a pin recorded in a delta buffer and an object still in an unpublished TLAB batch both survive the snapshot; a unit test requires four such objects to stay alive across a collection and to be reclaimed once released.
- **Registry removal:** A thread that exits with an unpublished batch and a live root slot publishes the batch, drops the stack and only then leaves the registry; a unit test reclaims everything it held, leaves no pin behind, and requires the next cycle to park and finish.
- **Evidence:** The runtime document's lock-order section now cites the test that would fail for each requirement, including the progress test that runs a collector against the rooted allocation path.

---

## Version 0.217-beta

### Summary of Changes (since `0.212-beta`):

#### 1. Lock Order and Collection Progress (PO-27 �2.5)
- **Barrier wait and the caller's own region:** Waiting for regions to drain skipped the calling thread's own entry, which could otherwise wait on a region belonging to the thread doing the waiting.
- **Heap lock under a barrier wait:** The heap lock records whether the calling thread holds it, and the barrier wait reports a lock-order violation instead of deadlocking; every acquisition goes through the instrumented guard.
- **Progress regression test:** A collector runs against a mutator on the rooted allocation path, where a barrier region takes the heap lock, so an inversion stops both threads instead of passing quietly.
- **Documented audit:** The runtime document states the lock order and where each collection-progress requirement is enforced.

---

## Version 0.212-beta

### Summary of Changes (since `0.206-beta`):

#### 1. Root-Liveness Analysis Cost and Audit (PO-27 �2.4)
- **Measured first:** `--verbose` reports functions, slot addresses, loads, liveness probes and address scans, so the rewrite was judged by counters rather than timing.
- **Address attribution:** Derived stack addresses are attributed to their slot in one pass in program order instead of a fixed point that rescanned the function per slot; address scans fell from four times per doubling to linear (34, 66, 130, 258 for 8, 16, 32, 64 managed slots).
- **Load indexing:** Loads are recorded against their slot in one pass, replacing a rescan of every operation per slot (per-slot scans fell from 100,407 to 0 at 64 slots).
- **Root decision:** Rooted slots are decided by walking the live set at each collection point once, instead of probing liveness per candidate per point.
- **Audit:** Fixtures cover each case RC_FIX �4.4 lists, asserting both program output and the emitted root protocol. Collection-point classification was not weakened.

---

## Version 0.206-beta

### Summary of Changes (since `0.201-beta`):

#### 1. Runtime Hardening and Edge-Case Fixes (PO-27 follow-up)
- **Transfer-Pin Delta Retirement:** A delta that cancels out clears its slot instead of leaving an entry that describes nothing but still matches later handoffs.
- **Transfer-Pin Slot Reuse:** Empty slots are reused before the high-water count grows, so returning and releasing distinct objects no longer pushes every handoff past the thirty-second onto the collector mutex; `nrq_gc_transfer_pin_fallbacks()` reports the genuine full-buffer cases.
- **Fallback Ordering:** A fallback settles the mutator's buffered deltas under the collector mutex before applying its own operation, so a release can no longer erase an entry a buffered pin had not reached yet and leave the object pinned for the life of the process.

#### 2. Mutator Region Completeness (PO-27 �2.2)
- **Propagation:** A function needs a region when it holds a managed value or calls one that does, resolved to a fixed point over direct calls. A driver loop that only calls an allocating helper keeps one region instead of entering and leaving per iteration; runtime entry points are not propagated, so querying or toggling collection still needs no region.
- **Recursion:** Region elision now resolves from the optimistic direction and narrows, so a direct or mutually recursive group entered once from `main` shares the outer region instead of duplicating it on every activation.
- **Entry Wrapper:** The wrapper the C runtime calls opens and closes the region, and the elision pass sees func-level as well as LLVM calls, so everything under `main` runs in one region.
- **Evidence:** `tests/integration/positive/gc_region_transitive`, `gc_region_recursive` and `gc_region_mutual` assert one enter and one leave for the whole program, with the safepoint poll still present in the loops that may allocate.

#### 3. Safepoint Poll Bound
- **Comparison:** The countdown is tested at or below zero instead of for equality, so a counter that reaches zero off the polled path or wraps past it can no longer stop a loop from observing collection requests.
- **Evidence:** `tests/integration/positive/gc_safepoint_bound` asserts one polled back edge and the comparison in the optimized module while the program allocates past the poll interval.

#### 4. Root Publication Guarantees
- **Never Dropped:** A type the lowering reports as managed always publishes an offset; a layout walk that returned nothing now registers offset 0 instead of silently skipping the root, with a debug assertion covering the disagreement.
- **Complete Elision:** `tests/integration/positive/gc_root_elision_owned` requires no descriptor, no mutator entry and no poll for a program with no managed storage.
- **Every Level:** `tests/integration/positive/gc_root_at_O0` requires the frame call at `-O0`.

#### 5. Documented Invariants
- **Runtime ABI:** `docs/architecture/06-runtime.md` now states the three root-completeness invariants and the poll bound, each naming the artifact that fails when it is broken.
- **Mutator Regions:** `docs/architecture/06-runtime.md` documents how mutator regions are decided (call-graph propagation, recursion handling, entry wrapper, what is never removed).

---

## Version 0.201-beta

### Summary of Changes (since `0.195-beta`):

#### 1. Mutator Regions Follow the Call Graph (PO-27 �2.2)
- **Propagation:** A function needs a region when it holds a managed value or calls one that does, resolved to a fixed point over direct calls. A driver loop that only calls an allocating helper keeps one region instead of entering and leaving per iteration; runtime entry points are not propagated, so querying or toggling collection still needs no region.
- **Recursion:** Region elision now resolves from the optimistic direction and narrows, so a direct or mutually recursive group entered once from `main` shares the outer region instead of duplicating it on every activation.
- **Entry Wrapper:** The wrapper the C runtime calls opens and closes the region, and the elision pass sees func-level as well as LLVM calls, so everything under `main` runs in one region.
- **Evidence:** `tests/integration/positive/gc_region_transitive`, `gc_region_recursive` and `gc_region_mutual` assert one enter and one leave for the whole program, with the safepoint poll still present in the loops that may allocate.

---

## Version 0.195-beta

### Summary of Changes (since `0.192-beta`):

#### 1. Transfer-Pin Delta Lifecycle (PO-27 �2.1)
- **Retirement:** A delta that cancels out clears its slot instead of leaving an entry that describes nothing but still matches later handoffs.
- **Reuse:** Empty slots are reused before the high-water count grows, so returning and releasing distinct objects no longer pushes every handoff past the thirty-second onto the collector mutex; `nrq_gc_transfer_pin_fallbacks()` reports the genuine full-buffer cases.
- **Ordering:** A fallback settles the mutator's buffered deltas under the collector mutex before applying its own operation, so a release can no longer erase an entry a buffered pin had not reached yet and leave the object pinned for the life of the process.

---

## Version 0.192-beta

### Summary of Changes (since `0.186-beta`):

#### 1. Safepoint Poll Bound
- **Comparison:** The countdown is tested at or below zero instead of for equality, so a counter that reaches zero off the polled path or wraps past it can no longer stop a loop from observing collection requests.
- **Evidence:** `tests/integration/positive/gc_safepoint_bound` asserts one polled back edge and the comparison in the optimized module while the program allocates past the poll interval.

#### 2. Root Publication Edges
- **Never Dropped:** A type the lowering reports as managed always publishes an offset; a layout walk that returned nothing now registers offset 0 instead of silently skipping the root, with a debug assertion covering the disagreement.
- **Complete Elision:** `tests/integration/positive/gc_root_elision_owned` requires no descriptor, no mutator entry and no poll for a program with no managed storage.
- **Every Level:** `tests/integration/positive/gc_root_at_O0` requires the frame call at `-O0`.

#### 3. Documented Invariants
- **Runtime ABI:** `docs/architecture/06-runtime.md` now states the three root-completeness invariants and the poll bound, each naming the artifact that fails when it is broken.

---

## Version 0.186-beta

### Summary of Changes (since `0.181-beta`):

#### 1. Root Registration Completeness
- **One Layout Walk:** The frame descriptor's words and its validity check come from a single derivation, so the table an activation publishes always matches the roots it was built from.
- **Self-Describing Tables:** A descriptor now carries its own word count, and the runtime rejects a truncated table before publishing anything, including on the marking path where the roots are marked from the table itself.
- **Late and Nested Slots:** `tests/integration/positive/gc_root_completeness` collects between declarations and asserts one descriptor holding a slot declared after a collection plus a nested aggregate layout.

---

## Version 0.181-beta

### Summary of Changes (since `0.176-beta`):

#### 1. Optimizer Evidence Matrix Recorded
- **Report:** `reports/performance/po-26-optimizer-evidence.md` maps every requested transform to its owner, its artifact and its verdict, including the one deferred item (SLP) and the two IR properties that limit LLVM.
- **Roadmap:** The optimization roadmap now names the owner and evidence artifact for each transform it lists, so a future cycle does not re-implement a pass the pipeline already runs.

---

## Version 0.176-beta

### Summary of Changes (since `0.171-beta`):

#### 1. Optimizer Evidence Matrix
- **Inlining:** `tests/integration/positive/opt_inlining` proves a scalar helper and a GC-registering helper both disappear into their caller while a real loop survives.
- **Loop-Invariant Hoisting:** `tests/integration/positive/opt_licm` asserts LLVM's hoisting remark for arithmetic that the source computes inside the loop.
- **Loop Vectorization:** `tests/integration/positive/opt_vectorize_owned` requires a vectorized body and vector trip count for an owned-array loop.
- **Justified Absence:** `tests/integration/positive/opt_vectorize_managed_blocked` records that a managed loop keeps its safepoint poll and is therefore not vectorized.
- **Rationale:** PO-7 asks whether the generated IR gives LLVM enough information, not for hand-written replacements of LLVM's own passes. Each claim is now backed by an artifact on the optimized module.

---

## Version 0.171-beta

### Summary of Changes (since `0.168-beta`):

#### 1. Batched Root Publication
- **Frame Root Descriptors:** A function's root set is a compile-time fact, so the lowering emits one read-only descriptor per root set and publishes every root of an activation with a single `nrq_gc_root_register_frame` call instead of one call per slot. Descriptors are interned per module, so functions that register the same layouts share one table in `.rodata`.
- **One Barrier Region Per Activation:** Registration now reserves the frame's roots once and takes a single barrier region, with the whole descriptor validated before any slot is published. Per-slot duplicate counts, in-flight snapshot marking and root frames are unchanged, and a lone root keeps the direct call.
- **Evidence:** `tests/integration/positive/gc_frame_roots` asserts one call and one shared descriptor for four slots of three layouts; `GcTest.FrameRootRegistrationHandlesWideMixedLayouts` covers frames that cross the inline layout storage.

#### 2. Shared Layout Descriptors Across Array Paths
- **One Descriptor Per Element Layout:** Managed array allocation, array root registration and array reference clearing each materialised the element layout in a stack array, one store per offset, on every call. All three now pass the interned read-only descriptor the scalar root paths use, so a managed array allocation site carries no layout stores at all.

#### 3. Optimized-Module Test Assertions
- **Assertions on LLVM Output:** Integration fixtures can now assert on the module that leaves the LLVM pipeline (`expect_optimized_ir_*`), which is the only way to prove that a transform LLVM owns actually ran rather than merely being reachable.
- **Unreachable Code Evidence:** `tests/integration/positive/opt_unreachable_code` proves a folded-away branch and the helper reachable only through it are both absent from the optimized module.

---

## Version 0.168-beta

### Summary of Changes (since `0.162-beta`):

#### 1. Write Barrier and Codegen Protocol Optimization (PO-24)
- **Forward References Through Write Barrier:** References stored through the write barrier are now forwarded directly to the remembered set without an intermediate root-store round trip, eliminating a redundant store and a cache-line bounce per mutator store.
- **Single Read-Only Descriptor Per Reference Layout:** The backend emits one shared descriptor per distinct reference layout instead of one per store site, reducing metadata size and enabling the collector to switch on the descriptor rather than recomputing layout.
- **Elide Redundant Mutator Regions:** A call that does not change the active mutator region now reuses the caller's established region instead of emitting a fresh `mutator_begin`/`mutator_end` pair, cutting protocol overhead in call chains.
- **Integration Assertions for GC Protocol:** Added integration test assertions that verify the exact sequence of emitted GC protocol operations (`mutator_begin`, `mutator_end`, `root_store`, `root_load`, `write_barrier`), so regressions in protocol emission are caught at test time rather than through runtime misbehavior.
- **Documentation:** Added `reports/performance/po-24-write-barrier-codegen.md` with the transformation description, IR evidence, and remaining opportunities.

#### 2. Compiler-Assisted Runtime Optimizations (PO-25)
- **Publish Managed References Directly to Roots:** The lowering path for managed references now emits a root-store at the definition site and skips the intermediate buffer, so the collector sees the reference immediately without a second store.
- **Initialize Parameter Roots with the Barrier Itself:** A reference parameter is rooted by the incoming barrier write rather than by a separate `root_store`, eliminating a redundant operation at function entry.
- **Unpin the Just-Published Reference:** After a managed reference is published to its root slot, the lowering immediately emits a matching `root_unpin` for the temporary, keeping the pin table accurate without a dedicated cleanup pass.
- **Throttle Safepoint Polls with a Countdown:** The safepoint poll on loop back-edges now runs once per 64 iterations via a register-promoted countdown instead of a branch on every iteration, removing the poll overhead from hot loops.
- **Documentation:** Added `reports/performance/po-25-compiler-assisted-runtime.md` with the lowering transformations, IR evidence, and remaining opportunities.

---

## Version 0.162-beta

### Summary of Changes (since `0.158-beta`):

#### 1. Write Barrier and Codegen Protocol Optimization (PO-24)
- **Forward References Through Write Barrier:** References stored through the write barrier are now forwarded directly to the remembered set without an intermediate root-store round trip, eliminating a redundant store and a cache-line bounce per mutator store.
- **Single Read-Only Descriptor Per Reference Layout:** The backend emits one shared descriptor per distinct reference layout instead of one per store site, reducing metadata size and enabling the collector to switch on the descriptor rather than recomputing layout.
- **Elide Redundant Mutator Regions:** A call that does not change the active mutator region now reuses the caller's established region instead of emitting a fresh `mutator_begin`/`mutator_end` pair, cutting protocol overhead in call chains.
- **Integration Assertions for GC Protocol:** Added integration test assertions that verify the exact sequence of emitted GC protocol operations (`mutator_begin`, `mutator_end`, `root_store`, `root_load`, `write_barrier`), so regressions in protocol emission are caught at test time rather than through runtime misbehavior.
- **Documentation:** Added `reports/performance/po-24-write-barrier-codegen.md` with the transformation description, IR evidence, and remaining opportunities.

---

## Version 0.158-beta

### Summary of Changes (since `0.153-beta`):

#### 1. Runtime Synchronization and GC Protocol Optimization � Extended (PO-23)
- **Skip Redundant Root-Store Edges:** Store barriers now check whether the new referent is already reachable from the same root frame before publishing a remembered-set edge, eliminating unnecessary per-store overhead when the edge already exists.
- **Counter-Based Mutator Parking with Dedicated Wakeup:** Replaced the flag-based parking protocol with a per-mutator counter and a dedicated wakeup primitive, so the collector parks and resumes mutators without a shared flag cache-line ping-pong.
- **Single-Pass Frame Root-Slot Release:** Frame teardown now clears all root slots belonging to the frame in one indexed scan instead of walking the entire root table, reducing per-frame cleanup to O(frame slots).
- **Single Published Store-Barrier Switch:** Generational tracking publishes one derived switch with conservative ordering, and aggregate stores read it once per store instead of twice per machine word.
- **Buffered Managed Handoff Pins:** A managed return records a signed transfer-pin delta in the returning thread's own buffer, folded into the pin table when a cycle is prepared, so helper-heavy call chains no longer serialize on the collector mutex.
- **Mutex-Free Root Fast Paths:** Registration and frame end skip the shadow-stack mutex because the collector can only read a stack while its owner is parked, and reference diagnostics is the one other reader, so those paths take the mutex only while diagnostics are enabled.
- **Flat Root-Slot Index:** Active root slots live in one open-addressing table per thread instead of a hash node per slot, so a registration or a frame-end removal touches a single cache line and never allocates.
- **Per-Mutator Barrier Counters:** Each mutator publishes its own barrier depth on its own cache line, so a store barrier never invalidates another thread's counter and the collector polls registered mutators directly instead of maintaining a shared reader count.
- **Documentation:** Added `reports/performance/po-23-runtime-synchronization.md` with the transformation description, structural evidence, and remaining opportunities.

---

## Version 0.153-beta

### Summary of Changes (since `0.147-beta`):

#### 1. Runtime Synchronization and GC Protocol Optimization (PO-23)
- **Per-Mutator Barrier Counters:** Each mutator publishes its own barrier depth on its own cache line, so a store barrier never invalidates another thread's counter and the collector polls registered mutators directly instead of maintaining a shared reader count.
- **Flat Root-Slot Index:** Active root slots live in one open-addressing table per thread instead of a hash node per slot, so a registration or a frame-end removal touches a single cache line and never allocates.
- **Mutex-Free Root Fast Paths:** Registration and frame end skip the shadow-stack mutex because the collector can only read a stack while its owner is parked, and reference diagnostics is the one other reader, so those paths take the mutex only while diagnostics are enabled.
- **Buffered Handoff Pins:** A managed return records a signed transfer-pin delta in the returning thread's own buffer, folded into the pin table when a cycle is prepared, so helper-heavy call chains no longer serialize on the collector mutex.
- **Single Published Store-Barrier Switch:** Generational tracking publishes one derived switch with conservative ordering, and aggregate stores read it once per store instead of twice per machine word.
- **Documentation:** Added `reports/performance/po-23-runtime-synchronization.md` with the transformation description, structural evidence, and remaining opportunities.

---

## Version 0.147-beta

### Summary of Changes (since `0.141-beta`):

#### 1. Runtime Synchronization and GC Protocol Optimization (PO-23)
- **Per-Mutator Barrier Counters:** Each mutator publishes its own barrier depth on its own cache line, so a store barrier never invalidates another thread's counter and the collector polls registered mutators directly instead of maintaining a shared reader count.
- **Flat Root-Slot Index:** Active root slots live in one open-addressing table per thread instead of a hash node per slot, so a registration or a frame-end removal touches a single cache line and never allocates.
- **Mutex-Free Root Fast Paths:** Registration and frame end skip the shadow-stack mutex because the collector can only read a stack while its owner is parked, and reference diagnostics is the one other reader, so those paths take the mutex only while diagnostics are enabled.
- **Buffered Handoff Pins:** A managed return records a signed transfer-pin delta in the returning thread's own buffer, folded into the pin table when a cycle is prepared, so helper-heavy call chains no longer serialize on the collector mutex.
- **Single Published Store-Barrier Switch:** Generational tracking publishes one derived switch with conservative ordering, and aggregate stores read it once per store instead of twice per machine word.
- **Documentation:** Added `reports/performance/po-23-runtime-synchronization.md` with the transformation description, structural evidence, and remaining opportunities.

---

## Version 0.141-beta

### Summary of Changes (since `0.131-beta`):

#### 1. Advanced Memory Optimizations (PO-21)
- **Leaner Object Header:** The managed-object header fits in 96 bytes, and the payload address is derived from the page bump allocator instead of being stored.
- **Per-Mutator Allocation Accounting:** Allocation counters are batched per mutator and published at TLAB publication or batch thresholds, so the fast path never touches a shared atomic.
- **Bounded Page Recycling:** Emptied pages return to a bounded pool and are reused instead of allocating fresh storage for every collection boundary.
- **Cheaper Reference Diagnostics:** The reference-count query compares each visited reference against a single target instead of building a map of the heap.

#### 2. Advanced Garbage Collection Algorithms (PO-22)
- **Breadth-First Marking:** The mark stack is a fixed FIFO ring that keeps the frontier contiguous instead of an intrusive list threaded through object headers, and it still never allocates.
- **Page-Local Object Discovery:** Reference resolution validates addresses against page start bitmaps, and the global object index now only tracks pages without start metadata (large objects), removing a hash insert and erase per ordinary allocation.
- **Cycle State in the Mark Context:** Marking reads its epoch and phase from the cycle's own context rather than shared atomics, and addresses already rejected by page metadata no longer fall through to a hash probe.
- **Flat Remembered Set:** An object's remembered-set membership lives in a header flag, so deduplication is a flag test and minor collections scan one contiguous array instead of a hash set.
- **Adaptive Collection Trigger:** The automatic-collection trigger follows the live heap (live bytes plus half as headroom, doubled when a cycle reclaims little) instead of a fixed one megabyte, and the live heap size is maintained incrementally rather than recomputed by walking every object at each cycle boundary.
- **Documentation:** Added `reports/performance/po-22-advanced-gc-algorithms.md` with the transformation description, structural evidence, and remaining opportunities.

---

## Version 0.131-beta

### Summary of Changes (since `0.127-beta`):

#### 1. Aggregate Layout Optimization & CLI Reporting (PO-20)
- **Widest-Alignment-First Field Ordering:** Struct fields are now laid out in descending alignment order (widest alignment first), minimizing padding and reducing aggregate size. The layout pass operates on the MLIR `nrq` dialect before lowering to LLVM.
- **Accurate Access Alignment Metadata:** Every memory access (`load`, `store`, `gep`) now carries precise alignment metadata reflecting the true field offset and type alignment, enabling LLVM to select optimal vector instructions and avoid unnecessary `memcpy`/`memmove` sequences.
- **Allocation-Path Counter Isolation:** Runtime allocation-path profiling counters are placed on dedicated cache lines, eliminating false sharing between threads and ensuring accurate per-path allocation statistics under contention.
- **Layout Inspection CLI (`--emit=layout`):** Added `--emit=layout` flag to report computed struct layout (field offsets, sizes, alignments, total size, padding) as JSON or text, enabling developers to inspect and verify aggregate memory layout without compiling to object code.
- **Layout Verification Tests:** Added integration tests validating field ordering, offset computation, padding minimization, and alignment metadata propagation through lowering.
- **Documentation:** Added `reports/performance/po-20-layout.md` with layout algorithm description, IR evidence, and CLI usage examples.

---

## Version 0.127-beta

### Summary of Changes (since `0.123-beta`):

#### 1. Profile-Guided Optimization (PO-19)
- **PGO CLI Controls (`--pgo`):** Added compiler flags `--pgo=generate` and `--pgo=use` to instrument binaries with profile counters and consume merged `.profdata` during optimization. Instrumentation is driven by LLVM's PGO pipeline (`InstrProf`) and linked via `clang_rt.profile` on Windows.
- **Profile Generation & Linking:** Binaries built with `--pgo=generate` emit raw profile data (`default.profraw`) at exit. The `llvm-profdata merge` step produces a combined `.profdata` consumed by `--pgo=use` to guide inlining, branch weighting, and hot/cold partitioning.
- **Sample Profile Round-Trip Verification:** Added integration tests verifying profile generation, merging, and consumption produce stable optimization decisions across rebuilds (`pgo_generate`, `pgo_use`, `pgo_roundtrip`).
- **Workload Coverage:** Profile quality validated across compute kernels (matmul, fib), allocator stress, and string processing workloads. Profile-guided inlining reduces code size in cold paths while accelerating hot loops.
- **Documentation:** Added `reports/performance/po-19-pgo.md` with IR evidence, build instructions, and correctness verification methodology.

---

## Version 0.123-beta

### Summary of Changes (since `0.120-beta`):

#### 1. Profile-Guided Optimization (PO-19)
- **PGO CLI Controls (`--pgo`):** Added compiler flags `--pgo=generate` and `--pgo=use` to instrument binaries with profile counters and consume merged `.profdata` during optimization. Instrumentation is driven by LLVM's PGO pipeline (`InstrProf`) and linked via `clang_rt.profile` on Windows.
- **Profile Generation & Linking:** Binaries built with `--pgo=generate` emit raw profile data (`default.profraw`) at exit. The `llvm-profdata merge` step produces a combined `.profdata` consumed by `--pgo=use` to guide inlining, branch weighting, and hot/cold partitioning.
- **Sample Profile Round-Trip Verification:** Added integration tests verifying profile generation, merging, and consumption produce stable optimization decisions across rebuilds (`pgo_generate`, `pgo_use`, `pgo_roundtrip`).
- **Workload Coverage:** Profile quality validated across compute kernels (matmul, fib), allocator stress, and string processing workloads. Profile-guided inlining reduces code size in cold paths while accelerating hot loops.
- **Documentation:** Added `reports/performance/po-19-pgo.md` with IR evidence, build instructions, and correctness verification methodology.

---

## Version 0.120-beta

### Summary of Changes (since `0.114-beta`):

#### 1. Function Call & Linkage Optimization (PO-16 / P1 3.7)
- **Internal Linkage for Generated Helpers:** Set `internal` linkage on all lowered function definitions except the C entry point `main`. Enables LLVM to perform whole-module reasoning, eliminate unused helpers, and select optimal internal calling conventions (`fastcc`).
- **Tail Call Marking (`MarkTailCallsPass`):** Introduced an MLIR pass that detects proven tail-position return sequences and attaches the `tail` call marker. Enables the LLVM x86 backend to emit sibling-call jumps and perform tail-recursion elimination at `-O0` and `-O2`, preventing stack exhaustion under deep recursion.
- **Single-Assignment Stack Slot Promotion:** Enhanced lowering to detect local variables, parameters, fixed arrays, views, and structs that are initialized once and only read. Bypasses redundant entry-block `alloca`, `store`, and `load` sequences by propagating the SSA value directly, cutting unnecessary memory traffic.

#### 2. Target-Aware Code Generation & Architecture Profiles (PO-17 / P1 3.8)
- **Target Profiles (`portable`, `native`, `explicit`):** Introduced structured target configuration resolving `--target-profile`, `--target <triple>`, `--target-cpu`, and `--target-features` once prior to optimization and code generation.
- **Strict CPU & Feature Validation:** Integrated subtarget feature validation (`isCPUStringValid`, `getAllProcessorFeatures`) that rejects invalid target options with diagnostic `NRQ5002` instead of silently ignoring invalid options.
- **Target Metadata Recording:** Lowered modules now embed target configuration module flags (`nrqoba.target.profile`, `triple`, `cpu`, `features`) ensuring transparent artifact profiling and verifiable host vs. portable distinction.
- **Assembly Verification Harness:** Added `assembly_check` mode to `tests/run_tests.py` using `llvm-objdump` to inspect emitted machine instructions and verify SSE2 baseline vs. AVX2 (`ymm`) vs. AVX-512 (`zmm`) code generation.

#### 3. Interprocedural Optimization & Whole-Program LTO (PO-18)
- **IPO Policy Controls (`--ipo`):** Added CLI flag `--ipo=standard|aggressive|minimal` to tune whole-program interprocedural optimization depth. Controls inliner budget, speculative devirtualization, and Attributor analysis runs.
- **Call-Graph Memory Effects Fixpoint:** Implemented a call-graph worklist solver that propagates `llvm.memory_effects` across the module, enabling LLVM to infer precise read/write/capture semantics for all functions including helpers.
- **Whole-Program LTO Configuration:** Backend now configures LLVM's LTO pass pipeline (`ThinLTO` or `FullLTO`) from the resolved IPO policy, enabling cross-module inlining, interprocedural constant propagation, function specialization, and global dead-code elimination without a separate link-time step.
- **IPO Metadata Recording:** Optimized modules embed `nrqoba.ipo.mode` and `nrqoba.ipo.strategy` module flags for transparent artifact inspection.
- **Integration Coverage:** Added test suites for multi-module inlining, symbol visibility, and function specialization under each IPO policy.

---

## Version 0.114-beta

### Summary of Changes (since `0.111-beta`):

#### 1. Optimization Diagnostics & Remarks Reporting (`--remarks`)
- Added compiler CLI flag `--remarks` (with `--remarks=missed` and pass-name filtering like `--remarks=loop-vectorize`).
- Plugs a custom diagnostic handler into LLVM's `DiagnosticInfoOptimizationBase` to report loop vectorization, inlining, and unrolling decisions to stderr without affecting emitted object code.

#### 2. Vector Alignment for Stack Allocations
- Promoted stack buffers for owned arrays and raw memory allocations previously defaulted to scalar natural alignment (`align 8`), forcing LLVM to emit unaligned SIMD instructions (`movups`).
- Implemented a 16-byte alignment floor for promoted stack slots meeting or exceeding vector register widths, generating `align 16` in LLVM IR and enabling selection of aligned SIMD moves (`movaps`). Stricter explicit alignments requested via `owned_array_aligned` continue to take precedence.

#### 3. Floating-Point Reduction Vectorization (`--fast-math`)
- Added an opt-in `--fast-math` flag enabling `reassoc` and `contract` fast-math flags on lowered floating-point operations.
- Relaxes strict IEEE 754 reordering constraints that previously prevented LLVM from vectorizing `f64` loop accumulations, allowing vector reduction synthesis (`llvm.vector.reduce.fadd`) and fused multiply-add (FMA) instruction selection while preserving strict IEEE semantics by default.

#### 4. Vectorized Loop Correctness & Integration Coverage
- Added dedicated test coverage verifying execution semantics under loop vectorization:
  - Runtime non-multiple trip counts (`vectorized_reduction_tail`) exercising scalar loop epilogues.
  - Overflow wrapping semantics (`vectorized_reduction_wrap`) across SIMD reduction trees.
  - Array bounds checking traps (`vectorized_bounds_trap`) ensuring runtime violation detection and exit code 70 remain intact in vectorized loop bodies.
  - Verification of 16-byte alignment and emitted `--remarks` diagnostics.

---

## Version 0.111-beta

### Summary of Changes (since `0.108-beta`):

#### 1. GC Root-Liveness Optimization (PO-11 / P0 3.3)
- Replaced eager activation-wide shadow-stack root registrations with fine-grained backward liveness analysis over TIR values.
- Managed stack slots, write barriers, and root frames are now emitted strictly across collection points where values are actually live, eliminating heavy shadow-stack metadata churn, mutex contention, and unnecessary tracing overhead for short-lived locals.
- Root summary information at collection boundaries is propagated across the call graph.

#### 2. Runtime Function Attributes & Memory Effects Inference (PO-12 / P0 3.4)
- Annotated all `nrq_gc_*` runtime entry points with accurate attributes (`nounwind`, `willreturn`, `nocapture`), allowing LLVM to treat GC bookkeeping and allocation helpers as non-throwing operations.
- Implemented whole-module memory effect inference: lowered functions now publish precise `llvm.memory_effects` (via fixpoint analysis over pointer provenance, stack spills, and external calls) rather than assuming every helper can read and write all memory.

#### 3. Entry-Block Stack Allocation & SSA Promotion (PO-13 / P0 3.5)
- Ensured all temporary stack allocations (aggregate barrier copies, `array.allocate` reference offsets, mapped memory access buffers, profiler records) are allocated exclusively in the function entry block via `alloca_at_entry`.
- Resolves unbounded stack growth inside loops and exposes all stack-allocated temporaries directly to LLVM's `mem2reg` and `SROA` passes.

#### 4. Review Hardening & Optimizations (PO-14 / F2�F6)
- **Parameter Attribute Derivation (F2):** `nocapture` and `readonly` attributes are now derived directly from escape fixpoint analysis and store-destination scanning instead of being blindly assigned from ownership classes.
- **Escape Analysis Store Soundness (F3):** Refused pointer-derived stores through borrowed views, ensuring publishing pointers/references into caller-accessible memory is properly treated as an escape.
- **Linear Stack-Allocation Analysis (F4):** Re-architected candidate descriptor lookups in stack promotion analysis using indexed hash tables, reducing escape analysis time on large view sets from cubic ($O(N^3)$) to linear ($O(N)$).
- **Cold Bounds-Failure Block Sharing (F5):** Deduplicated array bounds check traps so each function shares a single cold, noreturn failure block, drastically cutting redundant basic blocks and instruction bloat.
- **Loop Safepoint Polling Throttling (F6):** Throttled GC safepoint polling on loop back edges to execute once every 64 iterations via a register-promoted bitmask counter, eliminating per-iteration runtime call overhead in tight, allocation-free loops.

---

## Version 0.108-beta

### Changes since 0.105-beta:
- **`eb858e7`** � `feat(opt): implement escape analysis and stack promotion for pointer allocations`
- **`2a96640`** � `docs(report): complete alias analysis report and integration test expectation`
- **`6fec2d7`** � `feat(compiler): alias architecture upgrade`
- **`4bbaf93`** � `feat(compiler): Accurate Runtime Function Attributes`
- **`a5c7a28`** � `benchmark: rerun stress test 2`
- **`1caa5e8`** � `perf(gc): upgraded the internal mechanisms (pt 3)`
- **`0df9174`** � `perf(gc): upgraded the internal mechanisms (pt 2)`
- **`fdd9066`** � `perf(gc): upgraded the internal mechanisms`

---

## Version 0.105-beta

### Changes since 0.102-beta:
- **`ede0f26`** � `feat: gc improvement and new benchmark tests`
- **`678d7c1`** � `perf: add constant propagation and sccp to mlir cleanup`