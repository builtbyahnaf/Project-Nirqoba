# Phase 6.7 Report â€” Explicit Allocation and Allocator Strategies

## 1. Phase Overview

- **Phase:** 6.7 â€” Dynamic Memory Allocation and Selectable Allocator Strategies
- **Implementation date:** 2026-10-31
- **Status:** Partial implementation
- **Objective:** Support explicit memory allocation independently of the GC.
- **Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

## 2. Scope

Implemented typed raw allocation/deallocation, allocation ownership tracking,
invalid and duplicate free diagnostics, selectable general/pool/arena/stack/
slab/buddy/free-list/TLSF strategies, opaque allocator handles and reset/destroy
controls, per-allocation selection, arena-backed regions, and a limited static
use-after-free check for direct aliases. Custom allocator implementations,
scoped selection, reallocation, and complete pointer lifetime checking remain
incomplete.

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
argument counts and integer types.

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
  passed 19/19 integration tests. A Release build and full Release test run
  also succeeded; CTest passed 1/1 and all 19 integration tests passed.
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
dereferences generally, pointer aliases created by later assignment, or
interprocedural escapes/frees. The compiler does not enforce a complete
ownership/borrowing model. Container/scope selection and user-defined allocator
interfaces are absent. This runtime
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
| Custom allocator and container/function/module selection | Incomplete |
| Stress, concurrency and benchmarks | Not run |

## 12. Final Verdict

**FAIL** â€” the implemented allocator paths build and pass the current
integration suite, but safety and acceptance requirements remain incomplete.
