# Nirqoba Performance Cost Model

**Revision:** 0.266-beta (`904aee0`) + investigation commit `fa799e8`
**Configuration:** production runtime (`NRQ_RUNTIME_DEBUG_ENABLED=0`), `-O3`, MSVC x64
**Purpose:** put a number on every step an object passes through, so a proposed
optimization can be ranked before it is written.

All labels: **[MEASURED]** from `benchmarks/bottleneck_discovery/`, **[PROJECTION]**
derived, **[SOURCE]** read from code. Figures are medians on the investigation
machine; see `reports/bottleneck_discovery/00-method-and-provenance.md` §4 for
the 10–100 % run-to-run variance that applies to build numbers.

---

## 1. One object's lifecycle — measured cost per step

Test-02 node: 24-byte payload (two pointers + an integer), 96-byte header,
120-byte span.

### 1.1 Allocation (new object, heap has space)

| Step | ns/object | Evidence |
|---|---:|---|
| TLAB bump + span arithmetic + branches | ~4–6 | [PROJECTION] from 23.2 warm total minus steps below |
| `ObjectHeader` construction (11 stores, 1.5 cache lines) | ~5–7 | [PROJECTION] 96 B of stores |
| payload zero-fill (3 stores) | ~1 | [SOURCE] `memset` of 24 B |
| `ReferenceLayout::assign` (2 offsets) + 2 bounds checks | ~3–4 | [PROJECTION] |
| accounting (4 relaxed atomics + batch branch) | ~3–4 | [SOURCE] `account_tlab_allocation` |
| root-slot probe + barrier region + call/return | ~4–6 | [MEASURED] frame/root probes |
| **Warm allocation total** | **23.2** | **[MEASURED]** |
| First-touch page fault (fresh heap) | **+30.7** | **[MEASURED]** cold 53.9 vs warm 23.2 |
| **Cold allocation total** | **53.9** | **[MEASURED]** |

### 1.2 Root publication and handoff

| Step | ns/object | Evidence |
|---|---:|---|
| Frame begin + register 2 slots + frame end | **63.0** | [MEASURED] |
| ↳ of which: frame push/pop only | 16.4 | [MEASURED] |
| ↳ of which: per slot (RootInfo copy + 2 hash inserts) | ~21–23 | [MEASURED] 32.2 for 1 slot; 97.1 for 4 |
| `root_store` (idle fast path) | 1.6 | [MEASURED] |
| `pin_transfer` + `unpin` (buffered) | 5.1 | [MEASURED] |
| 3 root stores per internal node | 4.8 | [MEASURED] |
| **Protocol total per internal node** | **~73** | sum of the above |

### 1.3 Collection (per live object, per collection)

| Stage | Test 02 ns/object | Test 03 ns/object | Evidence |
|---|---:|---:|---|
| `begin_cycle` (root scan + TLAB publish) | 0.3 | **13.0** | [MEASURED] 0.69 / 11.38 ms per collection |
| Mark (pop, mark, 2 child lookups) | **32.5** | ~0.0 | [MEASURED] 66.6 ms / 2.046 M |
| Sweep (0.266 per-bit bitmap) | **47.2** | **36.3** | [MEASURED] 96.6 / 31.8 ms |
| Slice loop, phase transitions, finish | 29.8 | 10.9 | [MEASURED] residual |
| **Collection total** | **109.9** | **60.2** | 224.9 / 60.2 ms |

For test 03 the "objects/collection" denominator is 875,000 (dead objects
reclaimed); for test 02 it is 2,046,001 (live objects marked and swept).

### 1.4 Traversal

| Workload | ns/node | Evidence |
|---|---:|---|
| Test 02 sum loop | 7.6 | [MEASURED] 16 ms / 2.097 M, IR `sum_tree` is 29 instructions, zero GC calls |
| Test 01 chain walk | 10 | [MEASURED] |

---

## 2. Per-object totals, per million objects

| Operation | ns/object | ms per 1 M | ms per 2.097 M (test 02) |
|---|---:|---:|---:|
| Build (cold alloc + protocol + recursion) | **266** | 266 | **558** |
| Traverse | 7.6 | 7.6 | 16 |
| Collect, one full cycle at 0.266 | **109.9** | 110 | **234** |
| Collect, one full cycle after ColO-01/02 [PROJECTION] | ~63 | 63 | ~132 |
| Allocate only, warm (no build protocol) | 23.2 | 23 | 49 |
| Allocate only, cold | 53.9 | 54 | 113 |

---

## 3. Where a benchmark's time goes (0.266, per round)

The harness runs 5 collections per round in tests 01/02 (to beat the 1 ms
clock), so collection is repeated on an unchanged live graph.

### Test 02 (`mark_and_sweep`, 2.097 M live nodes)

| Component | ms/round | Share |
|---|---:|---:|
| Build | 558 | 32 % |
| Traverse | 16 | 1 % |
| 5 × collection (234 each) | **1,170** | **67 %** |
| **Total** | **1,744** | 100 % |

### Test 03 (`cyclic_graph_reclamation`, 1 M nodes, all dead)

| Component | ms/round | Share |
|---|---:|---:|
| Build | 135 | 37 % |
| 1 × collection | 88 | 24 % |
| (harness round remainder: allocation of the next round) | 140 | 39 % |

### Test 01 (`managed_object_allocation`, 1 M live nodes)

| Component | ms/round | Share |
|---|---:|---:|
| Allocate (cold heap) | 140 | 62 % |
| Traverse | 10 | 4 % |
| 1 × collection | 77 | 34 % |

**Reading:** the "collection" column in these benchmarks is not a marginal GC
cost. In test 02 it is five full cycles over the same live graph, which is 67 %
of measured time. Any claim that "GC is only 20 % of runtime" contradicts the
harness's own numbers.

---

## 4. Cost at scale ("millions of times")

**[PROJECTION]** linear in object count, from the medians above. Real programs
have mixed types; use this to size, not to predict.

| Scale | Build 02-like | One collect at 0.266 | One collect after fix |
|---:|---:|---:|---:|
| 1 M | 266 ms | 234 ms | ~132 ms |
| 10 M | 2.66 s | 2.34 s | ~1.32 s |
| 100 M | 26.6 s | 23.4 s | ~13.2 s |
| 1 B | 4.4 min | 3.9 min | ~2.2 min |

At 120 bytes/object, 100 M nodes = **12 GiB** of heap; the 64 KiB page model
needs 183,105 pages. At a 64-byte span (`04-object-header.md`) the same graph is
6.4 GiB and 91,553 pages.

---

## 5. Marginal cost of each candidate optimization

Derived from the tables above; "marginal" means the change in total for test 02
unless stated.

| Candidate | Save per object | Test 02 total | Evidence class |
|---|---:|---:|---|
| Set-bit bitmap sweep (ColO-01) | 71 ns/collection | −71 ms (−32 % of collect, −4 % of total round) | [PROJECTION] from measured 28.65 M zero-bit × 2.18 ns |
| Bound work by bitmap words (ColO-02) | 0 throughput | lock-hold bounded; prevents sparse-page stalls | [SOURCE] |
| Drop `ReferenceLayout` 32 → 8 B | 24 B footprint → 12 pages/1,000 objects | −3.7 % collect bitmap, −20 % faults [PROJECTION] | compiled layout probe |
| Header 96 → 64 | 32 B/object | −27 % pages → −27 % faults, −27 % sweep bits | [PROJECTION] |
| Header 96 → 40 | 56 B/object | −47 % pages, exactly 1 cache line/object | [PROJECTION] |
| Inline TLAB fast path | 11–15 ns build | −23…−31 ms (−4…−6 % round) | [PROJECTION] |
| Frame fast path (no per-slot copies) | 30–38 ns build | −63…−80 ms (−4…−5 % round) | [PROJECTION] |
| J3 handoff fusion (shipped in 0.269) | ~2–3 ns build | −4…−6 ms (−0.3 % round) | [PROJECTION] |
| Per-TLAB accounting | 2–4 ns build + 11 ns/object at cycle start (03) | −30…−40 % of test-03 `begin_cycle` | [PROJECTION] |
| Parallel marking (4 threads) | mark 32.5 → ~10 ns/object | −47 ms (−27 % of collect) | [PROJECTION], high risk |

**Ranking rule used in the reports:** prefer changes that reduce bytes per
object or per-object *count of operations* in the collector; prefer local,
reversible changes first; do not trade a measured collection win for an
unmeasured build loss.

---

## 6. Cost of operations that are usually ignored

| Operation | Cost | Source |
|---|---:|---|
| `gc.collect()` call overhead (empty heap) | ~0.1 ms | [PROJECTION] from `begin`/`finish` |
| First-touch fault per 64 KiB page | ~16.8 µs (= 30.7 ns × 546) | [MEASURED] derived |
| Bitmap memory | 1 KiB/page = 3.75 MiB for 240 MiB heap | [SOURCE] |
| Header bitmap density | 6.7 % (546 set / 8,192) | [MEASURED] |
| 1 ms timer quantisation | ±0.5 ms per sample; build is 1 sample | [SOURCE] |
| Process start + runtime init | < 1 ms | [UNKNOWN], not measured |

---

## 7. Model limits

1. Build numbers vary 291–757 ms for the same binary; only the *ratio* of
   components is stable, and even that is ±15 %.
2. The per-step split of the 23.2 ns warm allocation is inferred, not
   instrumented; the total is measured.
3. Mark cost per reference was not separated from per-object cost (no hardware
   counters).
4. Test 02's 5-collection loop weights collection 5× more than a program that
   collects once per build. Do not quote 67 % as a general GC share.
5. All projections assume the object graph and allocator behaviour of tests
   01–03; no fragmentation, minor collection, or multithreading is modelled.
