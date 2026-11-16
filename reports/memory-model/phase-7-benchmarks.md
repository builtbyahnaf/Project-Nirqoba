# Phase 7 — GC Isolation Benchmark Results

**Version:** `0.575-beta`  
**Harness:** `benchmarks/memory-model/phase-7/main.cpp`  
**Raw data:** `benchmarks/memory-model/phase-7/results.json`  
**Build:** MSVC C++20, `/O2 /MD`, RelWithDebInfo runtime  
**Samples:** 7 fresh processes per mode  
**Timer:** `std::chrono::steady_clock` around the measured loop

The harness uses the public runtime C ABI. It measures the same allocation,
root, frame, safepoint, barrier and collection entry points the compiler emits.
It does not compare strategies with different semantics, and no result below is
a claim that one memory strategy is universally faster.

## 1. Collector work

| Measurement | Median | Range | Notes |
| --- | ---: | ---: | --- |
| Managed 8-byte allocation | 54.476 ns/object | 53.618–60.463 | 600,000 objects, unpinned after initialization |
| Full collection after 600k objects | 7.033 ms | 5.893–8.230 ms | one explicit collection |
| Root register + unregister | 64.848 ns/pair | 63.340–65.972 | 1,000,000 pairs |
| Root frame begin + end | 28.101 ns/pair | 27.649–30.032 | 1,000,000 pairs |
| Safepoint poll | 0.377 ns/poll | 0.377–0.473 | 5,000,000 polls in one mutator region |
| Write barrier | 1.782 ns/store | 1.638–1.888 | 5,000,000 alternating root stores |
| Full collection after 500k objects | 6.221 ms | 5.251–6.709 ms | one explicit collection |
| Cycle build | 74.199 ns/object | 70.694–92.579 | 100,000 two-node cycles |
| Cycle collection | 2.181 ms | 2.111–3.294 ms | one explicit collection |
| Four-thread allocation | 110.662 ns/object | 105.843–123.349 | 4 × 100,000, concurrent collections |

Peak resident memory was approximately 44.2 MB for the managed-allocation run,
38.4 MB for the 500k-object collection run, 22.5 MB for the cycle run, 32.8 MB
for the multithreaded run and 9.8 MB for the isolation run. Those values include
the executable, runtime metadata and the process working set; they are not
payload-only measurements.

## 2. Isolation

| Ordinary workload | Median | Range | GC counters |
| --- | ---: | ---: | --- |
| Owned allocation + deallocation | 57.962 ns/op | 56.138–61.436 | unchanged |
| Borrowed array read | 0.072 ns/read | 0.072–0.073 | unchanged |
| Arena allocation | 45.608 ns/op | 44.929–51.097 | unchanged |
| Pool allocation + release | 141.927 ns/op | 132.474–143.491 | unchanged |
| Plain-shared allocation + release | 243.988 ns/op | 235.202–250.097 | unchanged |

For every isolation mode the harness compared GC allocated bytes, collection
count, live-object count, page count and layout count before and after the
measured work. All five remained unchanged. These measurements therefore show
the absence of collector allocation, collection bookkeeping and layout creation
on the ordinary paths; they do not compare the strategies against each other.

## 3. Generated-code evidence

Timing is not sufficient on its own. `tests/phase7_isolation.py` compiles twelve
ordinary fixtures at `-O0` and `-O2`, checks emitted LLVM for every `nrq_gc`
name (declarations included), checks the object symbol table for every `nrq_gc`
symbol, runs the executable, and repeats the check with the development runtime.
A managed control object must contain GC symbols. The complete check passed with
zero ordinary-object GC symbols and eight GC symbols in the control.

## 4. Interpretation and limits

- The safepoint poll and barrier figures are the cost of one ABI call in a tight
  loop; they are not a whole-program percentage.
- Collection latency is one full collection after a fixed load, not a
  worst-case pause distribution or an incremental slice series.
- Four-thread allocation includes concurrent explicit collection requests; the
  scheduler and collection frequency affect the range.
- The harness does not measure language-level root elision or inlining. Those
  are checked by the integration fixtures.
- Peak RSS is process-wide. No retained-memory projection is made beyond the
  reported live-object counts and page counts.
- No Phase 9 optimization was attempted; these numbers establish a Phase 7
  baseline.
