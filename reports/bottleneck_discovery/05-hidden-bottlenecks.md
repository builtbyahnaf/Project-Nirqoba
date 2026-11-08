# 05 — Hidden Bottlenecks

**Domain:** Everything outside AO/BO/ColO/header that shapes the numbers
**Verdict:** The largest hidden costs are architectural, not algorithmic:
the runtime is an opaque non-LTO archive with no memory-effect contracts, the
collector is single-threaded on a 12-thread machine, and the benchmark harness
cannot resolve small changes. Three of the items below are *measurement* defects
that have caused earlier reports to disagree; they are listed with the same
weight as the code defects because they determine what anyone can conclude.

Each item states its evidence label and whether it is a cost, a blind spot, or
both.

---

## H1. The runtime is an opaque, non-LTO archive — every runtime operation is a call

**[SOURCE]** `compiler/lib/Backend/Optimization.cpp:196-200` states the backend
"always performs whole-program LTO", but that is true only of the user program.
The runtime is a prebuilt static library:

* `runtime/CMakeLists.txt` builds `nirqoba_runtime.lib` with MSVC, no `/GL`;
* `compiler/lib/Driver/CompilationPipeline.cpp:405-441` invokes `lld-link.exe`
  with the archive and **no `/LTCG`** (`toolchain/toolchain.json`);
* generated IR declares the runtime entry points as external
  (`build/inv/02_stdout.ll:65-86`).

**Measured shadow:** `build_tree` is 68 instructions with 12 call sites; a
rooted allocation is one call; a handoff is three calls. Per-entry costs
(`02-builder-bo.md` §2): allocation 52 ns, frame registration 63 ns, root store
1.6 ns. The 1.6 ns root store is ~5.6 cycles — almost entirely call overhead for
work that is two stores and a branch.

**Consequence:** BO and AO cannot reach single-digit ns/object without an ABI
change (inlined fast path + runtime slow path). This is the ceiling on
micro-optimisation, not a bug in any one function.

---

## H2. Runtime declarations carry no memory-effect attributes

**[IR]** `build/inv/02_stdout.ll`:

```
declare void @nrq_gc_root_register_frame_static(ptr, i64, ptr) local_unnamed_addr
declare void @nrq_gc_root_store(ptr, ptr) local_unnamed_addr #2
declare void @nrq_gc_pin_transfer(ptr) ... #2
declare noalias ptr @nrq_gc_allocate_array_typed_rooted(...) #2
declare void @nrq_gc_unpin(ptr captures(none)) ... #3
...
attributes #2 = { nounwind }
attributes #3 = { mustprogress nounwind willreturn }
```

Contrast with generated functions, which receive
`memory(readwrite, argmem: none, inaccessiblemem: none, ...)` (`#0`). The
compiler *has* a whole-module memory-effect inference pass
(`compiler/lib/Lowering/PassPipeline.cpp:486-575`), but it only attaches effects
to functions **defined in the module**; external runtime declarations keep the
default "reads and writes all memory".

**Proven consequence in the emitted code:** the dead `mov qword ptr [rsp+30h],0`
before `root_store` (`02_masm.txt`) cannot be removed because the call is
assumed to observe the slot. Every runtime call also forces the compiler to
spill live values and reload anything it might have cached in a register.

**Fix shape:** declare the runtime entry points with precise
`memory(argmem: readwrite)` / `memory(argmem: read)` effects in
`ensure_runtime_fn`, validated against the real implementation. This is a
compiler-side, per-declaration change and needs a test that the attributes match
the runtime's actual writes (a wrong attribute is a miscompile, not a slowdown).

---

## H3. A collection's "begin" contains deferred construction work

**[SOURCE+MEASURED]** `prepare_cycle_locked()` calls `publish_tlab_locked()` for
every mutator (`gc.cpp:2196-2198`), which walks every unpublished object and
updates page counters, the start bitmap, statistics and the heap list.

* Test 02: `begin_cycle` = 0.7 ms/collection.
* Test 03: `begin_cycle` = **11.4 ms/collection**, 11.4 ns per pending object.

That is 11.4 ns of work per object that was deferred from allocation, and it
appears inside `COLLECT`. It is not avoidable by sweeping better. Batching the
accounting per TLAB (counts and bytes, not per object) would move most of it
back to O(1) per TLAB, at the cost of exact per-object page counters at that
instant. **[HYPOTHESIS]** worth ~30–40 % of test-03 collection.

---

## H4. Marking costs ~32 ns per live object and is serial

**[MEASURED]** 66.6 ms / 2,046,001 objects = **32.5 ns/object** (~114 cycles).
Per object the collector does: pop the worklist, read `header->size` and
`reference_offsets`, mark the header (a store into a 240 MiB heap → cache miss),
and for each of two children call `find_object_locked()`:

```
hash probe in PageDirectory (64 KiB chunk -> GcPage*)
test_object_start (bitmap load)
header->page == page validation
payload pointer equality
push onto a 256-entry ring or spill through header->mark_next
```

**[HYPOTHESIS]** the two `find_object_locked` calls dominate: they are two
dependent cache misses (page directory, page struct, bitmap word, header) per
object. No hardware counters were available to split this. `5d62abf`'s narrow
marking changed it by <0.2 ms, which is consistent with per-reference hash
probes being the real cost rather than layout walking.

**Second, larger issue:** marking is single-threaded
(`CollectorThreadGuard`, `run_collection` at `gc.cpp:2250`) on a 12-thread
machine. At 2 M objects it lasts 60–95 ms with eleven cores idle.
**[PROJECTION]** parallel marking with per-thread worklists could cut it 2–4×;
this is the single largest architectural ColO opportunity after the bitmap fix,
but it is a large, correctness-sensitive change (worklist ownership, barrier
handshake, epoch discipline) and is explicitly out of scope for ColO-02.

---

## H5. Bitmap density is 6.7 %, and every per-bit operation pays 15× the object count

**[MEASURED/SOURCE]** One start bit per 8 bytes (`gc.cpp:52`), 8,192 bits per
page, 546 objects in a full page with 120-byte spans → **546/8192 = 6.7 %**
density. Any algorithm that walks bit *positions* rather than set bits does 15×
the necessary work. This is the direct cause of the regression
(`03-collector-colo.md`), and it is also why the header size has a
disproportionate effect on sweep before the loop is fixed.

After the set-bit fix this specific multiplier disappears; density still matters
for bitmap memory (1 KiB per page = 3.75 MiB for 240 MiB of heap).

---

## H6. Page storage is over-aligned through `operator new`

**[SOURCE]** `gc.cpp:1919`:

```cpp
::operator new(capacity, std::align_val_t{kGcPageBytes}, std::nothrow);
```

MSVC implements over-aligned `operator new` with `_aligned_malloc`, which
allocates `size + alignment - 1` bytes and returns an aligned interior pointer.
For a 64 KiB page and 64 KiB alignment, the underlying allocation can be almost
**128 KiB** for 64 KiB of usable space. **[HYPOTHESIS]** this can roughly double
committed memory for the GC heap and is a plausible contributor to the measured
cold/warm allocation gap (30.7 ns/object).

**[UNKNOWN]** Not measured: no working-set sampling was taken for test 02.
A single `GetProcessMemoryInfo`/`PeakWorkingSetSize` sample per workload would
settle it. If confirmed, allocating page runs (e.g. 16 pages per `VirtualAlloc`)
and carving aligned 64 KiB pages from them removes both the alignment waste and
most of the per-page fault/allocator overhead.

---

## H7. Allocation accounting is per object

**[SOURCE]** `account_tlab_allocation()` (`gc.cpp:2521-2548`) does two relaxed
atomic loads, two stores and a batched add for **every object**; the batch only
amortises the CAS, not the atomics. `06dac9e` chose exact statistics
deliberately.

**[MEASURED]** this is part of the 23.2 ns warm allocation. **[PROJECTION]**
per-TLAB accounting (accumulate in the mutator, commit at publish/refill) saves
~2–4 ns/object, at the cost of statistics that lag by up to one TLAB.

---

## H8. Compiler pipeline leaves performance on the table

**[SOURCE]** `Optimization.cpp`:

* `-O3` → `buildPerModuleDefaultPipeline` with `PipelineTuningOptions` unchanged
  (standard inliner threshold); `--ipo=aggressive` (threshold 400, speculative
  devirtualization) exists but the benchmarks use the default.
* PGO is fully implemented (`IRInstr`/`IRUse`/`SampleUse`,
  `Optimization.cpp:238-266`) but the benchmark build does not use it.

**[PROJECTION]** PGO on test 02 could improve branch layout in `build_tree`
(recursive, two children, leaf check) by a few percent. It is a build-policy
change with no correctness risk, but it requires a two-stage benchmark build and
a profile artifact, so it is a measurement-protocol change, not a runtime one.

---

## H9. Benchmark harness defects (blind spots, not code costs)

**[SOURCE]** `benchmarks/stress_test_2/nirqoba/02_mark_and_sweep.nrqba:12` says
it explicitly: *"`time.now()` has 1 ms resolution, so each sample is a batch of
five"*. Confirmed: `time.now` lowers to `nrq_time_now_ms`
(`TirToMlir.cpp:3718`).

| Defect | Effect |
|---|---|
| 1 ms clock | build is one sample; traverse 6–30 ms is 1 ms-quantised |
| build measured once, cold | build medians vary 291–757 ms across runs |
| no min/median/max in JSON | a single thermal spike becomes "the result" |
| `compilers: unknown` | provenance requirement unsatisfied |
| `memory_bytes: null` | footprint differences invisible |
| no per-round raw samples | cannot compute spread or detect drift |

These are why the same commit "measured" 85 ms and 225 ms in different reports.
Any claim below ~25 % requires interleaved repeats; the roadmap's 8-run protocol
exists for that reason.

---

## H10. No workload exercises minor collections, fragmentation, or threads

**[SOURCE]** Tests 01–03 are: allocate-and-keep, mark-and-sweep-a-graph,
allocate-and-drop-everything. None creates old→young references, none leaves a
partially occupied page, none uses more than one thread. Therefore:

* `1646b93` (minor page skip) — **UNVERIFIED**;
* `1ca3c37` (page index) — **UNVERIFIED**;
* `take_free_run_locked` (TLAB run reuse) — **UNVERIFIED**;
* remembered-set and store-barrier mutation paths — **UNVERIFIED**;
* parallel/multithreaded safety — **UNVERIFIED** (and untested by the suite).

This is the largest evidence gap in the project, and ColO-04/05/06 in
`ColO_ROADMAP_2.md` are the correct response.

---

## H11. Type metadata and tracing

**[SOURCE]** Tracing is a function pointer per object (`nrq_gc_trace_fn trace`)
plus up to two inline offsets. For records with >2 references it allocates a
`std::vector<size_t>` per object (`ReferenceLayout::assign`, `gc.cpp:266-276`),
i.e. one heap allocation *and* one pointer chase per wide object. Tests 01–03
use two-reference nodes, so this path is **UNVERIFIED** and unmeasured. It is a
plausible cliff for array/struct-heavy programs and should be measured before any
header compression that shrinks the inline capacity.

---

## H12. Summary — what is actually worth attacking

| Rank | Item | Type | Expected | Confidence |
|---|---|---|---|---|
| 1 | Set-bit sweep (ColO-01) | code | removes 62 ms/col in 02 | High [MEASURED] |
| 2 | Bounded work budget (ColO-02) | code | lock-hold bound, no throughput loss | High |
| 3 | Parallel marking | architecture | 2–4× mark | Medium [PROJECTION] |
| 4 | Runtime memory-effect attributes | compiler | removes spill/reload and dead work around 10 calls/node | Medium |
| 5 | Inlined TLAB fast path | architecture | 11–15 ns/object build | Medium [PROJECTION] |
| 6 | Per-TLAB accounting | code | 2–4 ns/object build + 30–40 % of test-03 begin | Medium |
| 7 | Page-run allocation (aligned carve) | OS/alloc | removes up to 2× commit waste; fewer faults | Medium [HYPOTHESIS] |
| 8 | Header 96 → 64/40 | architecture | 27–47 % footprint | High arithmetic, unmeasured end-to-end |
| 9 | PGO / aggressive IPO | build policy | few % | Low–Medium |
| 10 | Harness rewrite (µs clock, repeats, memory) | measurement | makes 3–9 verifiable | High |

Items 1–2 are the roadmap's phase 0 because they are cheap, local, and remove a
measured 2.3–3.9× regression. Everything above rank 3 needs its own evidence.
