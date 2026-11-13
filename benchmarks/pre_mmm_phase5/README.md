# Pre-MMM Phase 5 — Explicit allocator lifetime

This benchmark isolates the explicit per-allocation-handle path retained after
Phase 4. It measures a shared general allocator through
`nrq_memory_allocate_from` and `nrq_memory_deallocate_checked`, with 10,000
allocate/write/free pairs per worker and median of five runs.

## Experiment

The Phase 4 implementation took `allocator_mutex` and incremented the allocator's
operation pin on every explicit-handle release and reallocation. The ownership
record already acts as a lifetime reference: `nrq_allocator_destroy` and
`nrq_allocator_reset` scan every shard record under the allocator mutex and
refuse while a record owned by that allocator is live or in flight.

The experiment removed the pin acquisition from **release, reallocation, and
final shared release**, while keeping it on allocation. Allocation still has no
record until the allocator returns a pointer, so its pin remains necessary.

## Result

Windows 11 x64, MSVC 14.51, `RelWithDebInfo`, five runs, medians in microseconds:

| Workers | 0.465-beta | Phase 5 | Change |
|---:|---:|---:|---:|
| 1 | 2,330 us | 1,840 us | **-21.0%** |
| 2 | 5,005 us | 3,650 us | **-27.1%** |
| 4 | 16,068 us | 8,622 us | **-46.3%** |

The explicit handle path is still slower than the default path because handle
resolution and the allocation-side operation pin still use the global allocator
registry. The experiment is retained because it removes a redundant
synchronization step without weakening lifetime safety; the focused tests block
inside a custom allocator's deallocate callback and confirm destroy/reset return
0 while the in-flight ownership record exists, then return 1 after it is erased.

Raw samples: `results.json`.
