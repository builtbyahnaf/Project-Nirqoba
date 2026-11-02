# Phase 6 Upgrade: direction and deferred work

This report records the active memory direction under the Phase 6 Upgrade and
the performance roadmap. It is not a Phase 6.9 acceptance report. Phase 6.9
diagnostics and platform utilities remain tracked separately in
`reports/phase-06.9.md`.

## Direction

- **Owned memory is the default.** Moves transfer one cleanup responsibility;
  supported owners are destroyed deterministically on scope exit, return,
  `break`, and `continue`.
- **Borrows are temporary and non-owning.** Pointer and array-view borrows are
  checked statically where the current language can represent their lifetime.
  Shared ownership and tracing GC remain explicit memory domains.
- **Keep allocator identity and safety checks.** The runtime continues to
  verify the allocator that owns each pointer. The Windows general allocator
  uses the Windows process heap; region allocators and custom callbacks retain
  their per-instance serialization. The general heap opts out of the extra
  runtime allocator mutex because Windows heap calls already synchronize
  concurrent access.
- **Keep GC correctness ahead of concurrency claims.** The collector remains
  precise, non-moving, and stop-the-world. Managed roots and stores use the
  synchronized runtime interfaces where required. No claim is made that
  uninstrumented concurrent pointer writes are safe.
- **Use contiguous owned arrays and checked borrowed views.** Owned arrays
  preserve contiguous storage, allocator selection, and explicit alignment.
  Immutable views and exclusive mutable views do not own or extend the
  lifetime of their source; locally scoped views can participate in stack
  placement when escape analysis proves they do not escape.
- **Measure before making performance claims.** The C++/Rust benchmark runner
  records repeated samples and environment metadata. Comparative results
  require both toolchains and controlled runs; no Rust comparison is claimed
  while `rustc.exe` is unavailable.

## Work deliberately held for future phases

The following work is still required by the broader roadmap and is not
silently counted as complete:

- **Allocator:** portable non-Windows allocator implementations; thread-local
  allocation caches; reducing remaining global `allocator_mutex` registry
  synchronization and production bookkeeping; a controlled comparison against
  platform and mature general-purpose allocators.
- **Ownership:** native-resource pointer field moves; returned-reference
  lifetime rules beyond pointers and array views; interprocedural and
  alias-rich escape analysis beyond the conservative local-array promotion now
  implemented; closure capture ownership; cleanup semantics for a future error
  or exception unwinding model.
- **Collector:** GC-aware generated functions use thread-local bump allocation
  for small objects, with headers published at safepoints before collection.
  Cooperative function-entry and loop-backedge polls coordinate stop-the-world
  collection. TLAB refill, oversized and unregistered native allocations,
  object metadata, and managed-reference stores still use the collector mutex.
  Size classes and free-slot reuse, generational or incremental collection,
  and write barriers remain open. GC benchmarking is deliberately deferred for
  this work.
- **Numerical memory:** allocator-aware tensor containers and shape/stride
  types; GPU device-buffer ownership and backend integration; SIMD-oriented
  container APIs. Contiguous owned-array storage, alignment, checked immutable
  views, exclusive mutable views, nested slicing, and conservative local stack
  placement now provide the CPU storage foundation.
- **Benchmarking through 6U:G:** equivalent optimized Nirqoba, C++, and Rust
  workloads on the same host, with warmups, repeated latency percentiles,
  memory measurements, and compiler/target metadata. The runner exists, but the
  comparative gate is pending the Rust toolchain and controlled execution.
  Comparative benchmarking is excluded from the current Phase 6 Upgrade work.
- **Platform support:** the runtime and explicit allocator remain Windows x64
  implementations. Portability requires separate platform backends and
  platform-specific regression runs.

The native resource example in Phase 6 documentation refers to deterministic
cleanup as a language/runtime property; it does not make networking a Phase 6
requirement. Socket work already present in the repository is preserved, but
this direction does not extend it.

## Evidence for the current allocator change

The general allocator still delegates to the Windows process heap and keeps
the ownership registry. It skips only the additional per-allocator operation
mutex; the global ownership registry lock remains in place. Custom and region
allocator serialization is unchanged. The concurrent general-heap regression
writes and checks independent allocations across eight workers before
releasing them.

Validation executed for this milestone:

- `cmake --build --preset windows-msvc-dev` — passed.
- `ctest --preset windows-msvc-dev --output-on-failure` — passed (1/1 test).
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe --configuration RelWithDebInfo` — passed (113/113 cases).

No allocator benchmark was run, and this change makes no performance claim.
