# AOR Phase 4 performance

**Status:** measured on one machine; no comparative claim beyond that machine.  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

This report separates what was **measured** from what is **derived**. The derived
table is arithmetic from the header sizes and page geometry that
`tests/unit/gc_representation_test.cpp` pins; it is exact but it is not a timing.

## Method

- Benchmark: `benchmarks/aor/phase4/phase4_benchmark.cpp`, built as
  `nirqoba_phase4_benchmark` and linked against `nirqoba_runtime_debug`.
- Workload per shape: 200,000 objects held live through a typed pointer array,
  then the same heap after every reference is dropped. Each object has one managed
  reference so marking walks the same graph in every case.
- One warm-up run, then 9 timed runs; the median is reported.
- Shapes: 64-byte fixed record requested headerless, 1 KiB fixed record requested
  compact, 1 KiB array requested extended.
- Environment: Intel Core i5-1235U, 12 logical processors, Windows 10.0.26300,
  MSVC 19.51 (VS 18 BuildTools), `RelWithDebInfo`, `/O2 /Ob1`, development runtime.

Run it with:

```bat
build\msvc-dev\benchmarks\aor\phase4\nirqoba_phase4_benchmark.exe 200000 9
```

## Measured

Medians of 9 runs, 200,000 objects. The per-object column is the median divided
by the object count.

| Shape | Allocate | Per object | Full GC, all live | Per object | Full GC, all dead |
|---|---|---|---|---|---|
| H0, 64 B record | 5.96 ms | 29.8 ns | 9.58 ms | 47.9 ns | 0.36 ms |
| H8, 1 KiB record | 76.15 ms | 380.7 ns | 16.27 ms | 81.4 ns | 1.62 ms |
| H16, 1 KiB array | 86.17 ms | 430.9 ns | 21.75 ms | 108.8 ns | 1.66 ms |

A second invocation of the same binary (5 runs) produced 5.72 / 81.17 / 75.95 ms
for allocation and 9.11 / 19.15 / 15.72 ms for the live collection. Run-to-run
variation is up to ~20% on this laptop, so the table supports order-of-magnitude
reading, not percentage comparisons between H8 and H16.

What the numbers do say:

- The headerless path is roughly an order of magnitude faster per allocation than
  either headered path at these shapes. Part of that is the smaller payload
  (64 B versus 1 KiB), so it is not a header-only comparison.
- A full collection of 200,000 live objects costs tens of nanoseconds per object,
  consistent with header-sized work per object.
- Collecting the same heap after all references are dropped is much cheaper than
  collecting it live, because whole pages are retired without visiting objects.

`header_bytes` in the benchmark output is the header cost of the live set: 0 for
H0, 1.6 MB for 200,000 H8 objects, 3.2 MB for 200,000 H16 objects.

## Derived per-object overhead

Arithmetic only. Page = 65,536 B, `max_align_t` = 8 B, so 8192 slots and four
1024-byte bitmaps (4096 B) per page; a headerless page adds one state byte per
object. `objects/page = floor(65536 / span)`.

| Shape | Span | Objects/page | Header | Page metadata/object | Total overhead | Share of span |
|---|---|---|---|---|---|---|
| H48, 64 B (pre-Phase 1) | 112 | 585 | 48 B | 7.0 B | 55.0 B | 49.1% |
| H32, 64 B (Phase 3) | 96 | 682 | 32 B | 6.0 B | 38.0 B | 39.6% |
| H8, 64 B | 72 | 910 | 8 B | 4.5 B | 12.5 B | 17.4% |
| H0, 64 B | 64 | 1024 | 0 B | 5.0 B | **5.0 B** | **7.8%** |
| H48, 1 KiB | 1072 | 61 | 48 B | 67.1 B | 115.1 B | 10.7% |
| H32, 1 KiB | 1056 | 62 | 32 B | 66.1 B | 98.1 B | 9.3% |
| H16, 1 KiB | 1040 | 63 | 16 B | 65.0 B | 81.0 B | 7.8% |
| H8, 1 KiB | 1032 | 63 | 8 B | 65.0 B | 73.0 B | 7.1% |

Reading the table:

- For a 64-byte record, H0 removes 33 of the 38 bytes of per-object overhead that
  H32 spent: an 87% reduction in header-plus-metadata, or a 28% smaller footprint
  per live object (69 B versus 96 B including page metadata).
- For a 1 KiB record the header is a small fraction of the span, so the choice
  between H8 and H16 is worth 8 bytes out of ~1,100. H8 wins on paper; the
  measured difference is inside this machine's noise.
- H0 is only competitive because the state table is indexed by object. While it
  was indexed per 8-byte slot, a 64-byte H0 object paid 8 B of metadata and the
  advantage over H8 disappeared.

## What is not claimed

- No H32 or H48 binary was rebuilt for this report, so there is no measured
  before/after timing. The comparison above is arithmetic from the header sizes,
  not a benchmark of the old collector.
- Only one machine and one compiler configuration were measured.
- The workload is synthetic: one live set, one graph shape, no promotion, no
  cross-generational traffic, no pinning.
- Pause times were not measured; the collection columns are whole-cycle costs.
