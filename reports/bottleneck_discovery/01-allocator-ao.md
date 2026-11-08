# 01 — AO: Allocator Architecture and Fast Path

**Domain:** Allocator Optimization (GC heap; `runtime/src/gc.cpp`)
**Verdict:** The allocator's CPU fast path is not the dominant bottleneck in the
measured object-heavy workloads. Its *footprint* cost is. Warm bump allocation
costs ≈23 ns/object; cold allocation costs ≈54 ns/object, and the 31 ns
difference is first-touch page faults caused by 120 bytes of per-object layout.
No AO change can recover that without reducing bytes per object.

---

## 1. Which allocator is on the benchmark path

Nirqoba has two allocators and they are not interchangeable:

| Allocator | Implementation | Used by |
|---|---|---|
| Explicit `memory.allocate` / `nrq_memory_*` | `runtime/src/allocator_windows.cpp` (1,790 lines) | `memory.allocate`, allocator objects, custom allocators |
| Managed GC heap | `runtime/src/gc.cpp` (`allocate_tlab`, `reserve_tlab_locked`, `GcPage`) | `array.allocate<T>` → `nrq_gc_allocate_array_typed_rooted` |

All three stress-test-2 workloads allocate managed nodes with
`array.allocate<TreeNode>` / `array.allocate<Node>`. The lowering emits
`nrq_gc_allocate_array_typed_rooted`
(`compiler/lib/Lowering/TirToMlir.cpp:3079`), so tests 01–03 exercise the **GC
heap only**. `allocator_windows.cpp` implements `nrq_memory_allocate`
(`allocator_windows.cpp:1236`) and is off the measured path.

**Consequence:** any AO work on `allocator_windows.cpp` (free-list design, custom
allocator vtable, profiling hooks) cannot explain, and cannot fix, the benchmark
gaps in tests 01–03. The AO-01 report targeted `gc.cpp`, which is the right
allocator; its numbers and conclusion are re-checked below.

---

## 2. Actual allocation fast path [SOURCE]

`allocate_tlab()` — `runtime/src/gc.cpp:2549-2617`:

```
depth != 0, bytes != 0, element_size != 0, no size overflow        (4 branch groups)
raw_span = 96 + bytes; span = round_up(raw_span, alignof(max_align_t)=8)
span > 64 KiB -> slow path                                         (1 branch)
TLAB generation != page generation -> reset 3 pointers             (1 branch)
!cursor || !end || cursor > end || span > end-cursor -> slow path  (4 branches)
-- bump --
placement new ObjectHeader  (11 field writes: 6 pointers, 4 flags, epoch)
memset(payload, 0, bytes)    (3 stores for 24 B)
ReferenceLayout::assign(offsets, count)  (inline copy for <=2, else std::vector)
for each reference: bounds-check offset against element_size
if (!young): old_object_count.fetch_add(relaxed); raise_store_barrier()
header->tlab_next = pending_objects; pending_objects = header
account_tlab_allocation: 2 relaxed atomic loads + 2 stores + unflushed add
if (collection_phase != Idle): take gc_mutex, publish entire TLAB
```

Caller `nrq_gc_allocate_array_typed_rooted()` — `gc.cpp:2790-2830` — adds:

```
ensure_shadow_stack()                                       (TLS, first call only)
shadow_has_pointer_root(slot): pointer_roots.contains(slot) || slot_states.find(slot)
begin_fast_barrier(): relaxed store + acq_rel compiler fence + phase acquire load
allocate_tlab(...)
*root_slot = object                    (plain store; no barrier)
end_fast_barrier(): acq_rel fence + relaxed store
```

So a normal rooted allocation is: **1 hash/table probe + 1 barrier region + ~20
branches/arithmetic + 21 header/payload writes + accounting**, entirely inside
the runtime translation unit. It is one opaque call from the generated code
(§`05-hidden-bottlenecks.md` §1).

### TLAB and page architecture [SOURCE]

| Constant / mechanism | Value | Lines |
|---|---|---|
| GC page | 64 KiB, 64 KiB-aligned `operator new` | `gc.cpp:51, 1919-1937` |
| TLAB reservation | 64 KiB (`kGcTlabBytes = kGcPageBytes`) | `gc.cpp:677` |
| Large object | span > 64 KiB bypasses TLAB → `allocate_locked` | `gc.cpp:2560` |
| Recycled page pool | up to 2,048 pages (128 MiB) | `gc.cpp:746-750` |
| Free-run index | `span_pages` vector, swap-remove, `total_free_spans` counter | `gc.cpp:714-745, 1768-1913` |
| Refill order | (1) first-fit free run, (2) current page tail, (3) scan `gc_pages` backwards, (4) acquire page | `gc.cpp:2442-2519` |
| Accounting batch | 16 KiB, or the collection threshold if smaller | `gc.cpp:925, 2521-2548` |

The AO-01 changes are present: 64 KiB TLAB
(`gc.cpp:677`), `total_free_spans` early exit (`gc.cpp:755, 1814`), 2,048
recycled pages (`gc.cpp:746`), and `take_free_run_locked` for TLAB refills
(`gc.cpp:1857`). `kGcTlabFreeSpanBytes` from the AO-01 report no longer exists;
the run-splitting logic replaced it. **[PRIOR REPORT]** AO-01's "no remaining
allocator issues" is therefore a statement about a different set of constants
and is not a verification of the current code.

---

## 3. Measured allocation cost [MEASURED]

`benchmarks/bottleneck_discovery/call_probe` against the committed HEAD runtime,
1,000,000 operations, private/free buffers, GC disabled, one registered root
frame:

| Operation | ns/op | Notes |
|---|---:|---|
| `allocate_array_typed` (unrooted, 2 refs) | 52.0 | header + payload + layout + accounting |
| `allocate_array_typed_rooted` (0 refs) | 51.5 | + root validation + barrier |
| `allocate_array_typed_rooted` (2 refs) | 54.6 | layout assign for 2 offsets |
| `root_store` (alternating value) | 1.6 | lock-free idle fast path |
| `pin_transfer` + `unpin` | 5.1 | buffered deltas, no global table |
| frame begin + static register (2 slots) + end | 63 | see BO report |
| full test-03 pair (2 objects + frame + 4 stores) | 85.3 per object | 170.6 ns/pair |

### The cold/warm split — the important result

The same allocation loop, run twice in one process: the first pass grows the
heap to 1,000,000 × 120 B = 114 MiB; the second pass runs after two collections
have recycled the pages, so it allocates from memory the process already owns.

| Pass | ns/object | Difference |
|---|---:|---:|
| cold (first touch, grows heap) | 53.9 | — |
| warm (pages already committed) | 23.2 | **30.7 ns/object (57 %) is OS page-fault cost** |

[MEASURED]. At 4 KiB pages, 1M × 120 B = 29,297 first-touch faults; 30.7 ns × 1M
= 30.7 ms ≈ 1.05 µs/fault, which is a normal Windows demand-zero fault.

**Therefore:**

1. Bulk-allocation benchmarks are measuring **bytes per object at least as much
   as allocator instructions**. Test 01's 140 ms/1M (140 ns/object) contains
   roughly 31 ns of page faults on a cold heap plus the frame/handoff protocol;
   the pure bump path is ~23 ns.
2. The single largest AO lever is **per-object footprint**, i.e. the object
   header (`04-object-header.md`), not the bump pointer.
3. Java's 2.57 ns/object in test 01 is not a like-for-like allocator
   comparison: the JVM commits its heap once (no faults per round), uses a
   ~32-byte header for a 2-field object, and has a JIT-inlined TLAB path. The
   CPU fast-path gap is still real (23 ns vs ~2.6 ns) and is analysed in the BO
   and hidden-bottleneck reports.

### Re-check of the AO-01 report's figures

**[PRIOR REPORT]** AO-01 claims "96-byte header, padded to 128 bytes" and "87.5 %
metadata". The measured layout is:

* `sizeof(ObjectHeader) = 96`, `alignof = 8` (`std::max_align_t` is 8 on MSVC
  x64), so a 24-byte payload occupies **120 bytes**, not 128
  (`04-object-header.md` §1).
* metadata share for test-02's node = 96/120 = **80 %**, not 87.5 %.

The report's conclusion (TLAB/page changes helped allocation) is plausible, but
its arithmetic and its "128 B" model are wrong. The correction is not cosmetic:
the page count (3,841) and every footprint projection depend on 120, not 128.

---

## 4. Is the fast path efficient? Comparison to what is possible

Per-object work in the warm path: 11 header field writes (96 B = 1.5 cache
lines), 3 payload stores, layout copy + 2 bounds checks, 4 relaxed atomics, 1
acquire load, 1 hash probe, one function call boundary. 23.2 ns ≈ 81 cycles at
3.5 GHz. For a bump allocator that is **high** — a Java TLAB bump plus zeroing a
24-byte object is roughly 10–20 cycles — but the extra work is not obviously
waste:

| Cost | Approx. share of 23 ns | Removable? |
|---|---:|---|
| Header construction (11 writes, 2 cache lines) | 15–25 % | Only by shrinking the header |
| Payload zero-fill (3 stores) | <5 % | Language-required today (no definite-init analysis) |
| Layout copy + per-offset bounds validation on every allocation | 10–15 % | Validation is a safety invariant; could be skipped only for compiler-trusted descriptors (see BO) |
| Root-slot hash probe (`shadow_has_pointer_root`) | 5–10 % | Compiler knows the slot is a pointer root; a trusted entry point could skip it |
| Barrier region (store + 2 fences + load) | 5 % | Correctness; cannot remove |
| Accounting (4 atomics, branch) | 5–10 % | Two stores per object; "exact statistics" was deliberate (`06dac9e`) |
| Call boundary, spill/fill, return | 20–30 % | Only inlinable across the runtime boundary (no LTO today) |

**[PROJECTION]** An inlined TLAB bump with a runtime slow path, plus a trusted
descriptor entry point, could plausibly reach 8–12 ns warm, i.e. save 11–15 ns
per object. On test 02 build (~2.1M nodes) that is 23–31 ms out of ~400 ms:
**6–8 % of build, ~4 % of total runtime**. It is worth doing eventually, but it
is not where the 3× collection regression lives.

---

## 5. Fragmentation and reuse — not exercised by the current suite

This is a genuine blind spot, not a finding of health.

* Test 02 reclaims **nothing** (all 2,097,151 nodes live): `release_span_locked`
  is never called, `span_pages` stays empty, `take_free_run_locked` immediately
  returns on `total_free_spans == 0`.
* Test 03 reclaims **everything**: every page reaches `live_objects == 0`,
  becomes `retired`, and is recycled wholesale. The spans it records are cleared
  when the page is recycled (`gc.cpp:1897-1905`). No free run is ever reused.
* Test 01 collects a whole chain, so it also retires pages.

**No committed workload mixes long-lived and short-lived objects in the same
page.** That is exactly the workload `f1d2b42` (run ordering), `1ca3c37` (page
index) and `take_free_run_locked` were written for. Their benefit is therefore
**[UNVERIFIED]** by the suite. The bisect shows them neutral on tests 02/03,
which is expected: they operate on a path those tests never take.

### Complexity of the reuse path

| Operation | Complexity | Lines |
|---|---|---|
| `release_span_locked` insert | O(spans in page) ≤ 96, plus tail shift ≤ 96 entries | `gc.cpp:1768-1810` |
| bridge merge (two neighbours) | O(96) tail shift | `gc.cpp:1780-1795` |
| `take_free_run_locked` first fit | O(pages in `span_pages` × spans/page) | `gc.cpp:1857-1913` |
| page acquire/recycle | O(recycled pages) first fit | `gc.cpp:1919-1990` |

`span_pages` is bounded by pages with free runs, and each page holds ≤ 96 spans.
With page-ordered sweep, frees arrive in ascending address order within a page,
so the common insert extends the previous run in O(1) without shifting; the
O(96) paths are fallbacks for rollback and out-of-order frees. This pairing is
sound, but it only works because page-ordered sweep exists — the two commits
must not be separated.

**Unbounded worst cases to watch:** if a workload frees in random order and keeps
pages half-full, `span_pages` can grow to thousands and a single refill can scan
thousands of pages × dozens of spans. No benchmark measures this. A future
fragmentation workload must exist before any further AO tuning is accepted.

---

## 6. Multithreaded scalability

**[SOURCE]** Per-thread state:

* `thread_mutator` (TLAB cursor/end, pending list, accounting, barrier depth) is
  `thread_local` — allocation itself takes no lock until a refill.
* `reserve_tlab_locked` takes `gc_mutex` and scans `span_pages` / `gc_pages`.
  With 64 KiB TLABs, refills are 1 per 546 test-02 objects, so the lock is
  amortised.
* `pending_bytes` / `pending_count` are relaxed atomics written by the owner and
  cleared by the publisher; `allocation_accounting.bytes_since_collection` is a
  CAS saturating add in the 16 KiB batch flush.
* `store_barrier_active`, `collection_phase`, `generational_collection_enabled`
  are shared atomics read on allocation.

**[UNKNOWN]** No multithreaded benchmark was run. Cross-thread effects that
should be measured before claiming scalability:

1. **False sharing** between adjacent `MutatorState` objects if `mutators` is a
   `std::vector<MutatorState*>` of separately allocated objects — the struct
   should be checked for cache-line alignment (the barrier counter comment says
   "lives on this thread's own cache line", but the struct has no
   `alignas(cache_line)`.
2. `gc_page_generation` invalidation: a collection bumps the generation, which
   discards every thread's TLAB at once. Threads that allocated at different
   rates will refill simultaneously after a cycle (thundering herd on
   `gc_mutex`).
3. `wait_for_fast_barriers()` spins with `std::this_thread::yield()` over all
   registered mutators while holding `mutator_mutex` — a potential O(threads)
   spin per collection, and a global barrier at every cycle start.

These are hypotheses; the only defensible statement today is that
single-threaded allocation is lock-free in the common case and refills are
amortised.

---

## 7. AO conclusions

| Question | Answer | Confidence |
|---|---|---|
| Is the allocation CPU path the main gap? | No. 23 ns warm; collection is 79–100 ns/object **per collection** and there are 25+ collections. | High [MEASURED] |
| Is the allocator slow compared with C/Java? | Yes on CPU path (~10× Java TLAB), but footprint + page faults explain more of test 01's 55× headline than instructions do. | Medium–High |
| Is TLAB sizing/refill a problem? | No evidence of it. 64 KiB TLAB = one refill per 546 nodes. | High [SOURCE+MEASURED] |
| Is there an unfinished AO item that caused the regression? | No. The regression is in sweep, not allocation. | High [MEASURED bisect] |
| Are the AO-01 changes present? | Yes (64 KiB TLAB, 2,048 recycled pages, `total_free_spans`, run-based refill). | High [SOURCE] |
| Are they verified? | No independent before/after exists; the committed benchmark file predates ColO, and the AO report's 128-byte model is wrong. | High |
| Biggest AO lever | Reduce bytes per object (header), which cuts page faults, heap size, TLB pressure and sweep bitmap work together. | High |

### Ranked AO candidates

| Rank | Candidate | Expected impact | Risk | Evidence |
|---|---|---|---|---|
| A1 | Shrink `ObjectHeader` (96 → 64/48) | 27–40 % less heap; 27–40 % fewer page faults; 27–40 % fewer sweep bitmap pages; mark locality improvement [PROJECTION] | Architectural (ABI, lowering) | `04-object-header.md` |
| A2 | Inline TLAB bump in generated code with runtime slow path | 11–15 ns/object warm → 6–8 % of test-02 build [PROJECTION] | High (compiler + runtime ABI) | §4, `05-hidden-bottlenecks.md` §1 |
| A3 | Trusted rooted-allocation entry point (skip `shadow_has_pointer_root` for compiler-known slots) | 2–4 ns/object [PROJECTION] | Moderate (safety invariant) | §2 |
| A4 | Avoid per-offset validation for compiler-emitted descriptors | 1–3 ns/object [PROJECTION] | Moderate | §2 |
| A5 | Measure and tune the reuse path with a fragmentation workload | Unknown; currently unmeasured | Low (measurement first) | §5 |
| A6 | 2 MiB pages / `VirtualAlloc` large pages for the GC heap | Cuts faults/ TLB misses; requires privilege and a fallback | Moderate | `05-hidden-bottlenecks.md` §5 |

A1 dominates A2–A4: it improves build, mark and sweep simultaneously, while
A2–A4 only touch build.
