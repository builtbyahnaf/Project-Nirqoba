# Phase 8 benchmarks — manual memory

**Compiler:** `0.583-beta`  
**Runtime:** production and debug (`--debug-enabled`)  
**Target:** x86_64-pc-windows-msvc  
**Samples:** 7 fresh processes per workload and configuration

Run:

```text
python benchmarks/memory-model/phase-8/run_benchmark.py --runs 7
```

The runner compiles each workload with `-O2` and with
`-O2 --debug-enabled`, checks that both builds produce the same checksum, and
records cycles per operation, total cycles, process elapsed time, peak sampled
RSS, and the full sample range in `results.json`.

## Workloads

| Workload | Operations | What it measures |
|---|---:|---|
| `ordinary_alloc` | 1,000,000 | `memory.allocate<byte>` + `memory.deallocate` baseline |
| `manual_alloc` | 1,000,000 | `memory.manual_allocate<byte>` + `memory.manual_release`, same reads/writes |
| `manual_alloc_from` | 1,000,000 | manual allocation from one explicit `memory.general_allocator()` handle |
| `manual_fallback` | 1,000,000 | manual allocation with no explicit release; lexical fallback runs |
| `manual_borrow` | 10,000,000 | scoped `&block[0]` access with a store through the borrow |
| `manual_managed_root` | 100,000 | manual storage containing a `managed<int>` field, then `gc.collect()` |

Manual reallocation, manual alignment selection and FFI transfer are not
benchmarked because the supported contract rejects or does not expose them.

## Results

Cycles are the platform counter returned by `time.cycles()`; they are not
converted to nanoseconds because its frequency is not part of the language
contract. Lower is better. The ranges are minimum–maximum over seven samples.

| Workload | Configuration | Median cycles/op | Range | Std. dev. | Peak RSS |
|---|---|---:|---:|---:|---:|
| ordinary_alloc | production | 142.231 | 134.896–144.292 | 3.063 | 9,859,072 |
| ordinary_alloc | debug | 143.355 | 133.677–162.191 | 8.849 | 9,846,784 |
| manual_alloc | production | 144.432 | 136.401–165.505 | 10.780 | 9,863,168 |
| manual_alloc | debug | 139.643 | 133.710–161.297 | 10.454 | 9,838,592 |
| manual_alloc_from | production | 340.480 | 322.896–344.830 | 6.847 | 9,871,360 |
| manual_alloc_from | debug | 341.453 | 330.468–363.813 | 10.123 | 9,871,360 |
| manual_fallback | production | 142.417 | 136.729–144.386 | 2.463 | 9,846,784 |
| manual_fallback | debug | 143.628 | 137.201–148.855 | 3.547 | 9,854,976 |
| manual_borrow | production | 1.704 | 1.535–1.724 | 0.077 | 9,875,456 |
| manual_borrow | debug | 1.626 | 1.569–1.703 | 0.045 | 8,974,336 |
| manual_managed_root | production | 345.422 | 338.557–349.782 | 3.753 | 15,650,816 |
| manual_managed_root | debug | 361.555 | 355.062–403.000 | 14.995 | 15,925,248 |

Ratios from the production medians:

- `manual_alloc / ordinary_alloc`: **1.0155**
- `manual_alloc_from / ordinary_alloc`: **2.3939**
- `manual_fallback / ordinary_alloc`: **1.0013**
- `manual_borrow`: **1.704 cycles/access**
- debug/production overhead on `manual_managed_root`: **1.0467×**

## Interpretation and limitations

- Manual and ordinary explicit allocation use the same runtime allocator and
  ownership registry. The measured **1.0155×** ratio is not a speedup claim; it
  shows that the manual category adds no separate heap or hidden allocation path
  to this workload. The difference is within the observed sample spread.
- Explicit allocator handles are slower in this workload because the handle
  path records and resolves allocator identity instead of using the default
  inline path. This is an existing allocator property, not a manual-category
  penalty.
- The debug runtime's timing is close to production because these workloads do
  not trigger debug-only GC instrumentation; the small differences are within
  normal run-to-run variation. `manual_managed_root` shows a modest debug cost
  because it uses collector roots.
- Peak RSS is sampled while the process is alive and can miss a shorter peak.
- The timer is not converted to nanoseconds; compare cycles within this host,
  not across machines.
- Native file/socket/resource latency is excluded because it measures the host,
  not ownership dispatch. FFI latency does not exist because the language has no
  FFI surface.
- No comparison to a hypothetical unmanaged C allocation is made: such a
  comparison would not have equivalent ownership and cleanup semantics.
