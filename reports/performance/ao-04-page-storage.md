# AO-04 — Page storage, alignment economics and run management

**Base commit:** `7a87eda` (0.301-beta released; AO-03 closed)
**Result commits:** `4c90fc2`, `702ad9c`, `1e77c39`
**Configuration:** production runtime (`NRQ_RUNTIME_DEBUG_ENABLED=0`), MSVC 18,
RelWithDebInfo + `/Ob2`, Windows x64.
**Labels:** MEASURED / SOURCE / PROJECTION. Every timing or memory claim below is
MEASURED unless labelled otherwise, with its sample count.

## 1. Scope

The stage licence is narrow: verify whether page storage wastes memory or system
calls, and act only on what a measurement shows. `AO_ROADMAP_2.md` §7.2 lists the
measurement, §7.3 the candidate (page runs) and the axes it has to be judged on.

## 2. Verification: is there waste, and of what kind?

§7.2 asks for the page count, the requested usable bytes, and the process's committed
private bytes for one representative workload. AO-02/4 already answered it, and this
stage re-used those numbers instead of repeating the run:

| quantity | value |
| --- | --- |
| GC pages for 1,000,000 rooted 24-byte arrays | 1,833 |
| requested usable bytes (1,833 × 64 KiB) | 114.6 MB |
| private commit with the historical over-aligned `operator new` | 238 MB (2.08×) |
| private commit with one `VirtualAlloc` per page (AO-02/4) | 122 MB (1.06×) |

So the hypothesis in §7.1 was right about the cause — an aligned allocation reserves
room to align itself — and AO-02/4 already removed that waste by mapping each page
directly. What remained was the *rate* of system calls: a growing heap still paid one
reservation and one release per 64 KiB page. A standalone microbenchmark measured that
path (MEASURED, 1,833 pages, 9 rounds): 1,833 `VirtualAlloc`/`VirtualFree` pairs cost
min 5.77 ms, median 6.94 ms, while reserving one region and committing it in 1 MiB
chunks cost min 2.82 ms, median 3.10 ms for the same pages. That justified §7.3's
candidate.

## 3. Changes landed

### AO-04/1 — reuse the space a sweep freed before the next boundary (`4c90fc2`)

An emptied page cannot be *recycled* until the next cycle boundary: a mutator that has
not parked may still hold a cursor into it, and its storage must stay mapped until
every cursor is gone. Its reclaimed space is not off limits, though — the sweep already
decided about every slot it covered, and an object materialised afterwards carries a
birth epoch past the cycle, so the sweep skips it. Allocation now takes free runs and
tail space from those pages and clears the retirement mark, which also stops the
boundary from reclaiming a page that has been taken back into use.

The consequence is that a cycle-heavy program no longer needs a whole extra generation
of page storage. MEASURED, 1,000,000 rooted allocations per round, 6 interleaved
processes × 3 rounds:

| | previous | this commit |
| --- | --- | --- |
| private commit after the second round | 241.6 MB | 122.0 MB |
| peak working set | 247.6 MB | 128.4 MB |
| page count | 1,833 | 1,833 |
| second-round allocation, min | 72.1 ms | 22.2 ms |
| second-round allocation, median | 74.4 ms | 24.3 ms |
| all 18 samples, min / median | 26.7 / 69.9 ms | 22.2 / 26.8 ms |

### AO-04/2 — carve ordinary page storage from runs (`702ad9c`)

Ordinary pages now come from 8 MiB reservations, committed in 1 MiB chunks and carved
into 64 KiB slots; the slot stride preserves the 64 KiB alignment the page directory
keys on. A slot released while the heap is busy goes to a bounded free list and is
handed out again without a system call. Acquisition is constant time (one free-list
pop, or a bump of the single active run); release scans the runs, which only happens
when the recycle pool is full. Pages larger than a slot keep their own mapping.

MEASURED, same workload and sample shape as above, against `4c90fc2`:

| | previous | this commit |
| --- | --- | --- |
| first (heap-growing) round, min | 66.6 ms | 45.7 ms |
| first (heap-growing) round, median | 69.9 ms | 50.8 ms |
| steady-state rounds | 23–25 ms | 23–25 ms |
| page count | 1,833 | 1,833 |
| private commit | 122.0 MB | 122.5 MB |

The 0.5 MB is chunk slack: at most 1 MiB is committed beyond what the pages use, per
run, and it is bounded by construction.

### AO-04/3 — bound idle retention and reuse decommitted slots (`1e77c39`)

The recycle pool held up to 2,048 shells — 128 MB of committed storage — for a program
that had stopped allocating. It now holds one run's worth (128 shells); a page that
leaves the pool decommits its slot, returning the commit charge to the OS, and the slot
stays in its run so a later wave recommits it instead of carving new address space.

MEASURED, 24,000 × 1 KiB objects (2,198 pages), private commit:

| | previous | this commit |
| --- | --- | --- |
| after the heap went idle | 145.0 MB | 19.4 MB |
| after a second full wave | 146.0 MB | 146.4 MB |
| after that second wave went idle | 144.6 MB | 19.4 MB |

## 4. The axes §7.3 asks to be judged on

- **Allocation granularity**: one reservation per 8 MiB, one commit per 1 MiB, one slot
  per page. A direct mapping remains for pages larger than 64 KiB.
- **Release granularity**: a slot returns to its run's free list (committed) up to 128
  slots, then `MEM_DECOMMIT` per slot; a page that leaves the pool is a slot release,
  so the commit charge follows live pages, not peak pages.
- **Page lifetime and reuse**: two layers — in-place reuse of a swept page (AO-04/1),
  then the shell pool (128), then run slots. Each layer is bounded.
- **Address-directory compatibility**: slots are 64 KiB apart from a 64 KiB-aligned
  reservation base, so `PageDirectory`'s key (address >> 16) is still unique per page
  and the `find` path is unchanged. The integration suite exercises pointer resolution
  through the directory for indexed and unindexed pages.
- **Commit and decommit**: commit high-water per run; decommit on release above the
  free-list bound; recommit on reuse of a decommitted slot. Verified by the idle probe
  above (146.4 → 19.4 MB after a wave that had been fully released).
- **Fragmentation and peak resident memory**: peak working set follows live pages
  (128.4 MB for a 122 MB heap); the two storage-generation problem of §3/AO-04/1 is
  gone. A heap that oscillates between full and empty keeps its reservations (8 MiB
  granularity) but returns its commit charge.
- **Failure handling and cleanup**: every storage acquisition failure returns nullptr
  and the page is not published; the rollback paths (`allocate_locked`,
  `allocate_tlab`, `acquire_page`) release storage through the same routing entry
  point, so a slot always returns to the run that owns it. A reservation that cannot be
  committed is abandoned rather than retried forever.
- **Thread safety and lock hold time**: every storage path runs under `gc_mutex`; no new
  lock was added, and the acquisition path is shorter than the system call it replaced.

## 5. Invariants preserved

- Page storage is 64 KiB aligned and 64 KiB sized for ordinary pages; object alignment
  (`alignof(std::max_align_t)`) and the span arithmetic are untouched.
- `nrq_gc_page_count`, `nrq_gc_recycled_page_count`, `nrq_gc_page_reuse_count` and
  `nrq_gc_span_reuse_count` keep their meanings.
- Object statistics, birth epochs, sweep decisions and the trigger boundary are
  unchanged; nothing in this stage delays a public statistic.
- The stop-the-world handshake and the park-before-scan protocol were not modified;
  AO-04/1 only widened which pages allocation may fill, not when a page may be recycled.

## 6. Tests (MEASURED)

- `build/msvc-dev/tests/nirqoba_unit_tests.exe`: 196/196, including the two tests added
  here (`EmptiedPagesServeAllocationsBeforeTheNextBoundary`,
  `IdleHeapRetainsOnlyABoundedPagePool`).
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe`: 263/263.

## 7. Limitations

- The probes are single-threaded and disable automatic collection; the multi-threaded
  and generational paths are covered by the suites, not by these measurements.
- The microbenchmark in §2 measures the system-call path alone, so it bounds what a run
  allocator could save, not what it did save; the rendered result is §3/AO-04/2.
- Timing resolution on this machine is roughly ±5% on the median. The AO-04/6 numbers
  are far outside that; AO-04/3's memory change is a memory result, not a timing claim.
- The first-touch cost of a heap that grows (≈27 ms per 114 MB here) remains; it is
  memory that must be faulted in, and this stage only removed the bookkeeping around it.

## 8. Recommendation

- AO-04/1, AO-04/2, AO-04/3: **accept**. Each is measured, each keeps the statistics
  contract, and the rejected alternative in §7.3 — a custom page-run allocator built
  from arithmetic rather than measurement — was justified by measurement here (§2).
- The stage's remaining headroom is first-touch faults on a growing heap, which no
  allocator change removes; further work belongs to workload-level reuse, not to the
  page layer.
