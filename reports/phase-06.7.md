# Phase 6.7 Report — Explicit Allocation and Allocator Strategies

## 1. Phase Overview

- **Phase:** 6.7 — Dynamic Memory Allocation and Selectable Allocator Strategies
- **Implementation date:** 2026-10-31
- **Status:** Implemented allocator system; items blocked on later phases remain (§13)
- **Plan:** [`phase_plans/PHASE_6.7.md`](../phase_plans/PHASE_6.7.md)
- **Objective:** Support explicit memory allocation independently of the GC.
- **Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

## 2. Scope

Implemented typed raw allocation/deallocation, allocation ownership tracking,
invalid and duplicate free diagnostics, selectable general/pool/arena/stack/
slab/buddy/free-list/TLSF strategies, opaque allocator handles and reset/destroy
controls, per-allocation selection, arena-backed regions, limited static
use-after-free diagnostics, and a native C callback interface for custom
allocators. Nirqoba source-level custom allocator declarations,
container/module/application selection, reallocation, and complete pointer
lifetime checking remain incomplete. Function/block selection can use `defer`
to restore the prior thread-local selection.

## 3. Architecture and Design

The Windows runtime exposes an opaque C ABI. A synchronized registry records
the allocator that owns each returned pointer, ensuring deallocation returns to
the same strategy. Selection is thread-local; the default explicit allocator
is general purpose. A per-allocation handle overrides this selection for one
allocation. Regions are arena allocator handles. GC allocations remain on a separate heap. Allocation
failure returns null. Fixed and region strategies use their documented size
and lifetime behavior; stack frees are checked for LIFO order.

## 4. Implementation Details

All eight strategies are implemented in `allocator_windows.cpp`. Region
strategies use free-block metadata or size/order bins as appropriate. Allocator
reset invalidates its outstanding pointers, while destruction is rejected when
an allocator remains selected or owns live allocations. The compiler lowers
allocator handles through pointer/integer conversions and checks source-level
argument counts and integer types. Native embedders can register callbacks;
the runtime tracks their allocation identity and original size/alignment.

## 5. Modified Files

`runtime/include/nirqoba/runtime/allocator.h` defines kinds and handle APIs;
`runtime/src/allocator_windows.cpp` implements strategies and ownership
tracking; runtime CMake includes the source. `TypeChecker.cpp` and
`TirToMlir.cpp` provide language validation and lowering. The integration test
`tests/integration/positive/allocator_strategies` exercises each strategy.

## 6. Language/API Changes

Constructors: `memory.general_allocator()`, `pool_allocator(bytes)`,
`arena_allocator(bytes)`, `stack_allocator(bytes)`, `slab_allocator(bytes)`,
`buddy_allocator(bytes)`, `free_list_allocator(bytes)`, and `tlsf_allocator(bytes)`.
Control APIs: `memory.get_allocator()`, `memory.use_allocator(handle)`,
`memory.reset_allocator(handle)`, `memory.destroy_allocator(handle)`. Explicit
allocations use the current thread's selection; passing handle zero restores
the default allocator.

## 7. Testing

- Command: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Configuration RelWithDebInfo`
- Environment: Windows x64, MSVC, LLVM/MLIR 23.1.4.
- Actual result: build succeeded; CTest passed 1/1; the final expanded suite
  passed 23/23 integration tests. The Release build and full Release test run
  also succeeded; CTest passed 1/1 and all 23 integration tests passed.
- The allocator integration case creates/selects each strategy, allocates,
  reads/writes and frees a typed byte, restores the prior allocator, then resets
  and destroys region strategies.
- Long stress, dedicated fragmentation, and benchmark tests are not present;
  unit tests include pool exhaustion and concurrent allocator users.

## 8. Performance

No allocator benchmarks were run; no relative performance claim is made.

## 9. Known Issues

The compiler rejects uses through direct aliases after `memory.deallocate`,
`memory.secure_release`, or `memory.unmap_file`; it cannot check raw pointer
dereferences generally, aliases hidden in aggregates, or interprocedural
escapes/frees. It tracks direct variable aliases created by declaration or
simple assignment. The compiler does not enforce a complete
ownership/borrowing model. Container, module, application-wide selection, and
user-defined allocator interfaces are absent. Function/block selection can be
paired with `defer` to restore the prior thread-local allocator. This runtime
implementation is Windows-only. Strategy-specific edge and stress tests remain
needed.

## 10. Documentation Changes

Updated `project_context/SYNTAX.md`; added `docs/memory/allocators.md` and
`docs/memory/management.md`.

## 11. Acceptance Criteria

| Requirement | Status |
|---|---|
| Explicit allocation and allocator identity | Implemented |
| Eight built-in allocator strategies | Implemented; basic integration covered |
| Failure, alignment, mismatch and double-free behavior | Partial; invalid frees checked, broader failure/alignment tests absent |
| Direct alias use-after-free diagnostic | Partial; static direct aliases only |
| Per-allocation and region selection | Implemented for typed explicit allocations |
| Native C callback allocator interface | Implemented; unit tested |
| Nirqoba custom allocator declarations and container/module/application selection | Incomplete |
| Function/block allocator selection with deferred restoration | Implemented; integration tested |
| Stress, concurrency and benchmarks | Not run |

## 12. Final Verdict

**FAIL** — the implemented allocator paths build and pass the current
integration suite, but safety and acceptance requirements remain incomplete.
## 13. Completion pass — 2026-11-01

Implemented since the first pass:

- `memory.reallocate<T>(pointer, new_bytes)`, which satisfies the roadmap's
  reallocation requirement. The block keeps its allocator and recorded
  alignment; contents survive up to the smaller size; growth of the most recent
  arena or stack block resizes in place; every other strategy allocates a
  replacement and releases the original. A null result leaves the original
  allocation valid, a zero byte count releases it, and an invalid or already
  freed pointer stops with a runtime diagnostic.
- `memory.set_default_allocator(handle)` / `memory.get_default_allocator()`
  provide the application-wide explicit-allocation policy required by 6.7.3.
  Thread selection still wins; handle zero means the configured default, which
  also governs `memory.allocate<T>(bytes, 0)` and `memory.allocator_kind(0)`;
  a policy default cannot be destroyed while it serves that role.
- Allocator introspection: `nrq_memory_allocator_free_bytes` and
  `nrq_memory_allocator_fragmentation_bytes`, plus `free_statistics()` overrides
  for the pool, slab, arena, stack, buddy, free-list, and TLSF strategies. The
  profiler report now carries `free_bytes`, `largest_free_block`,
  `fragmentation_bytes`, a six-bucket allocation-size histogram, and per-kind
  live bytes, which covers the profiler attribution requirement.
- Benchmarks: `AllocatorTest.BenchmarksSmallLargeAndMixedWorkloads` measures
  20,000 small and 4,000 mixed-size allocation/deallocation pairs on the
  general, pool, and TLSF allocators. Timings are printed for CI visibility; no
  relative performance claim is made.
- Tests: `AllocatorTest.ReallocatePreservesContentsAndAllocatorIdentity`,
  `ReallocateRespectsStackAndRegionLifetimeRules`,
  `DefaultAllocatorPolicyGovernsExplicitAllocations`,
  `ReportsFreeSpaceAndFragmentation`, and
  `ProfilerReportsSizeHistogramAndAllocatorAttribution`. Integration cases
  `memory_reallocate`, `memory_profiler_report`, `memory_default_allocator`, and
  the negative case `reallocate_managed_pointer` exercise the language surface.

Still blocked or not implemented:

- Nirqoba-source `Allocator` declarations (interface methods taking a
  `pointer`). The language has no interface or function-pointer types; those
  arrive with Phase 8 (interfaces/generics) and Phase 13 (C ABI). Native
  embedders can already register a callback allocator through the C ABI.
- Container-level allocator selection. Collector-managed arrays are owned by the
  tracing heap, so they have no explicit allocator to select; container
  integration needs allocator-aware container types, which the language does not
  have yet.
- Complete static pointer lifetime checking. The compiler still tracks only
  direct variable aliases inside one function; `memory.reallocate` deliberately
  does not mark the old pointer dead statically, but the runtime rejects a later
  free of it.
- Custom allocator reset remains unsupported; reallocation reaches custom
  allocators through the generic allocate-copy-free path.

This addendum supersedes the "Not run" entries for benchmarks in section 11;
the overall verdict stays **FAIL** until the blocked items above are resolved.
