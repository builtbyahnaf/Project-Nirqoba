# Phase 6 allocator benchmark snapshot

Date: 2026-11-01. Windows x64 RelWithDebInfo, MSVC 14.51.36231, LLVM/MLIR 23.1.4. One unit-test run on the current machine; these are smoke measurements without warmup or repeated percentile sampling and must not be generalized.

| Allocator kind | Small: 20,000 pairs (µs) | Mixed: 4,000 pairs (µs) |
|---:|---:|---:|
| 0 general | 2,358 | 2,882 |
| 1 pool | 2,010 | 363 |
| 2 arena | 1,714 | 376 |
| 3 stack | 1,835 | 347 |
| 4 slab | 2,034 | 1,053 |
| 5 buddy | 5,354 | 1,099 |
| 6 free-list | 5,934 | 1,639 |
| 7 TLSF | 11,502 | 2,371 |

Workload details: small requests are 48 bytes with a 128-item live window. Mixed requests range from 16 to 3,984 bytes, also with a 128-item live window. Allocations and frees run through the runtime ownership registry. Region-backed strategies were measured with their existing locking and bookkeeping.

| Threads | Allocate/free pairs | Elapsed (µs) |
|---:|---:|---:|
| 1 | 10,000 | 1,400 |
| 2 | 20,000 | 3,347 |
| 4 | 40,000 | 6,594 |

The multithread case shares one general allocator and records a single elapsed sample for each thread count. It exercises contention correctness and provides a baseline, but does not isolate mutex wait time from allocator work. No allocator changes or speedup claims are implied.
