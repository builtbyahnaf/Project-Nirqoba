AO-01 Optimization Plan — Allocation Path (items 1–11)
Owner: Muhammad Tahmid Ahnaf · Phase: AO-01 · Status: proposed, awaiting owner acceptance Constraint honoured throughout: nothing measured by me has been claimed; no benchmark runs without your explicit authorization (roadmap §2.3 + your standing instruction).

0. Governing rules for this plan
Rule	Consequence in this plan
§2.1 preserve ownership/lifetime/GC semantics	Every stage keeps the LIFO root-frame protocol, park-before-scan handshake, SBOT, and the ReferenceLayout correctness contract
§2.2 no speculative redesign, no removed safety checks	Each item names the invariant it must preserve and the proof obligation; validation is deduplicated, never deleted
§2.3 reproducibility	Stage 0 exists because current evidence cannot pass this bar (3.8× run-to-run swing on T02 COLLECT)
§11 one focused change at a time, owner acceptance per phase	Each item = one commit, one measurement gate, revertable alone
§10 migration boundary	Stage 4 is heap metadata format only — no ownership, borrowing, or GC-policy change
1. Stage map and expected magnitude
Estimates are cycles/object derived from call counts in 02.ll/03.ll and the code at runtime/src/gc.cpp; they are forecasts, not results.

ID	Item	Domain	Est. effect	Confidence	Risk
A0	Evidence integrity (harness + instrumentation)	tooling	unblocks all measurement	—	none
A1	Barrier RMW → store + fence (item 1)	runtime	−150…250 cyc/obj (~15–25%)	high	medium (concurrency proof)
A2	Batched allocation accounting (item 2)	runtime	−40 cyc/obj	high	low
A3	root_store Idle fast path (item 3)	runtime	−80…120 cyc/obj	high	medium (snapshot argument)
A4	Dedupe slot validation (item 4)	runtime	−40…60 cyc/obj	medium	low
A5	Single-pass object init (item 8)	runtime	−20…40 cyc/obj	medium	low
A6	Pin/unpin handoff collapse (item 7)	runtime+codegen	−80…160 cyc/node (T02)	medium	medium
A7	Bump-path prefetch (item 11, part)	runtime	0…−30 cyc/obj	low	low
A8	Frame reuse across iterations (item 5)	codegen	−200 cyc/iteration	high	medium
A9	Inline TLAB fast path (item 6)	codegen+link	−50…100 cyc/obj	medium	high
A10	Descriptor-pointer header, 96→48 B (item 9a)	runtime(+1 global shape)	1.75× less traffic, −20…30 cyc/obj	high	medium-high
A11	Free-span index + page size (item 10)	runtime	−refill scan, −page churn	medium	medium
A12	32 B header stretch (item 9b)	runtime	2.3× less traffic	medium	high (format)
Cumulative forecast: A1–A7 ⇒ T01 ≈ 200–215 ms, T02 build ≈ 520–570 ms; +A8–A10 ⇒ T01 ≈ 120–150 ms (2–2.5× today). Java parity remains a Stage-4 question, per my earlier arithmetic.

2. Stage 0 — Evidence integrity (prerequisite, no perf change)
A0.1 — Neutralize the in-tree __rdtsc profiling. runtime/src/gc.cpp currently carries your uncommitted BuilderProfiling/ScopedTimer block, unconditional and therefore active in release. Proposal: keep the code, wrap it in #ifdef NRQ_BUILDER_PROFILING and add a CMake option defaulting OFF — that preserves your instrumentation without letting it measure itself. I need your yes/no on this because it is your uncommitted work.

A0.2 — Harness correctness (benchmarks/stress_test_2/run_benchmark.py): the CommandOutcome + Popen/tree-kill design from RC_FIX §2.3 — explicit statuses, zero-byte-vs-failure distinction, atomic checkpoint, and a diagnostics output path so a failed run can never overwrite a good artifact (exactly what destroyed the suite result in b3b950f).

A0.3 — Baseline protocol: 5 executions × 3 in-process repeats, report min/median/max and spread per cell; identical machine/compiler/config; hash the executable. Gate: T02 COLLECT spread must fall under ~10% before any item is judged. If it doesn't, that is a finding, not a nuisance.

3. Stage 1 — Synchronization (A1–A3)
Shared root cause: 14–18 lock-prefixed RMWs per object across begin_fast_barrier/end_fast_barrier (gc.cpp:1025/1035), account_tlab_allocation (2182), nrq_gc_root_store (2738), nrq_gc_pin_transfer (3143), nrq_gc_unpin (3103).

A1 barrier_depth is written only by its owner on its own cache line: replace fetch_add(acq_rel)/fetch_sub(release) with relaxed atomic store/load + release/acquire fences. Proof: collector reads only while the region is in flight; the fence keeps the region's memory effects ordered. Rollback: one commit; barrier invariants are covered by the PO-27 tests (cb82d13, d59a910).
A2 Flush pending_bytes/pending_count with the existing 16 KB batch instead of per object. Proof: both are read only under gc_mutex (gc.cpp:3035) and reset in publish_tlab_locked (965); batch granularity is already the published resolution.
A3 When collection_phase == Idle and store_barrier_active == false, a field store may be a plain store. Proof obligation: a mutator cannot park mid-store, so its roots are scanned after the write; document that explicitly and add a race test alongside the existing ones.
4. Stage 2 — Metadata and initialization (A4–A7)
A4 allocate_array_typed_rooted re-validates a slot the compiler registered in the same frame (shadow_has_pointer_root, 2380). Move the "pointer root" bit into the frame descriptor (already built once per frame) so registration decides once. Not a removed check — a deduplicated one.
A5 Replace new (storage) ObjectHeader() (96 B value-init) + ~12 overwriting stores + separate payload memset with one bulk clear over the span plus six field stores.
A6 T02 pays pin_transfer + 2 unpin per node to hand a return value into a slot the caller immediately roots. Introduce a handoff convention that publishes into the caller's existing slot; runtime change plus a small emission change — coordinate with the BO-01 agent (§6).
A7 One _mm_prefetch(cursor + 128) on the bump path. Cheap, measured last, expected to be rejected if it doesn't move the median; that is an acceptable outcome per §5.3.
5. Stage 3 — Object format and reuse index (A10, A11, A12)
A10 — descriptor-pointer header (96 → 48 B). Verified facts that make this viable: the compiler never reads the header size (only nrq_gc_object_header_bytes(), gc.cpp:2988, used by tests/unit/gc_test.cpp:97), payloads are handed back as header + 1, and the compiler already interns reference-offset arrays as private .rdata globals (TirToMlir.cpp:1844). So trace + element_size + ReferenceLayout{2 offsets, unique_ptr, count} = ~56 bytes can collapse into one interned descriptor pointer. This also removes a real latent cost I found while reading: ReferenceLayout::assign heap-allocates a vector for every object with ≥3 reference fields. Blast radius: 106 references to reference_offsets/element_size/trace/birth_epoch in gc.cpp, object_span_bytes, page start bitmaps, find_object_locked arithmetic, plus the 96-byte static_assert and the unit test. No disk format, no compiler ABI.

A11 — free-span index + page sizing. take_free_span_locked still scans gc_pages whenever any span exists anywhere, including spans on retired pages; add a spans-per-page-indexed cursor and evaluate 256 KB pages to cut directory entries and TLB pressure.

A12 — 32 B header (stretch). Pack flags+age+epoch+size, drop per-object page, derive page from the directory in the rare paths. Separate owner approval, separate report, and only if A10's measured result justifies the second format change.

6. Boundary with the BO-01 agent
File / area	Owner
runtime/src/gc.cpp, runtime/include/nirqoba/runtime/gc.h, tests/unit/gc_test.cpp	me (A1–A7, A10–A12)
benchmarks/stress_test_2/run_benchmark.py, results/checkpoint	me (A0.2)
compiler/lib/Lowering/TirToMlir.cpp frame region	BO-01 agent (A8)
Allocation/root-store emission	BO-01 agent (A6 emission half, A9)
Layout-descriptor struct emission	shared; I write the contract, BO-01 emits
Recommendation: runtime commits land first (they're orthogonal and compile independently); the BO-01 agent rebases onto them. A8/A9 then start from a runtime whose per-call cost is already lower, which makes their measurements attributable. I'll write the runtime-side contract for A8 (frame_reuse_begin/reset) so the other agent can build against a fixed interface.

7. Verification per stage
No test suites were requested for now, so gates are structural and, once you authorize runs, empirical:

Unit suite (build/msvc-dev/tests/nirqoba_unit_tests.exe) and integration suite must stay 100% green before any benchmark run — a perf change that breaks GC correctness invalidates its own measurement.
Targeted GC tests per stage: barrier/handshake (PO-27 set), frame LIFO, pin/unpin, sweep/free-span reuse, header layout (gc_test.cpp updated with rationale).
Benchmark gate: T01 ALLOC, T02 BUILD, T03 BUILD independently; full Stress Test 2 suite for regressions; medians + spread recorded; nothing reported as successful without them.
Per-phase report in the roadmap §9 14-section format, with confirmed/hypothesis/unverified labels.
8. Sequenced deliverables
A0 → commit "chore(bench): make results reproducible; gate profiling instrumentation" + re-baseline.
A1–A3 → three commits, one measurement each, one report section each.
A4–A7 → four commits, batch report.
A8–A9 → after BO-01 head start lands; co-authored boundary contract.
A10–A11 → format change with its own report and explicit approval.
A12 → decision point, not a commitment.
9. What I am explicitly not doing
No new allocator algorithm, no size-class redesign, no concurrent-GC work, no workload edits, no removed checks, no /GL+/LTCG switch bundled with a semantic change (A9 evaluates it as an isolated experiment), and no migration-adjacent feature (items, epochs, borrowing) under this track.

10. What I need from you before starting
A0.1: gate your __rdtsc instrumentation behind an off-by-default define (keeps it available) — yes/no?
Stage order: approve A0→A1–A3 as the first reviewable batch, so you can accept/reject before A4+?
BO-01 boundary: confirm the file-ownership table above, or tell me A8 belongs to you/that agent entirely.
Benchmark authorization: when you're ready for stage gates, say so explicitly; until then I will not run the suite.
The plan is ready. Once you exit plan mode I can persist it as tasks/AO01_OPT_PLAN.md (gitignored, matching the tasks/PO27_FIX convention) and begin Stage 0.