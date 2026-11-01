# Phase 6 Upgrade follow-up

Date: 2026-11-01

## Ownership and contiguous storage

The explicit ownership path now supports unique pointer and owned-array moves,
owned and shared parameter/return contracts, checked borrowed pointer parameters,
read-only array views, and compiler-inserted deterministic cleanup for tracked
owners and supported resources. `owned_array<T>` stores a contiguous element
buffer and length. `array_view<T>` is a non-owning descriptor that supports
checked nested slicing and function parameters. Views cannot escape through
returns or be written through. Their lexical borrow blocks owner moves, release,
and mutation while the view binding is live.

Validation on Windows x64, MSVC, and LLVM/MLIR 23.1.4: RelWithDebInfo build,
CTest 1/1, and integration 65/65 passed. Integration cases cover array
allocation/drop/transfer, bounds and range checks, view passing and nesting,
and invalid owner operations while borrowed.

## GC safety state

GC remains an opt-in, precise, non-moving mark-and-sweep domain. Allocation
headers and payloads share one allocation. Root descriptors and registration
counts are validated, metadata allocation failures fail safely, and diagnostic
counters saturate instead of wrapping. Pinned references are included in the
optional reference diagnostics. Ordinary correctness tests exercise cycles,
roots, pins, typed arrays, concurrent allocation, allocation pressure, and
reclamation; the unit suite no longer prints timing measurements.

The collector still serializes allocation, root mutation, pinning, and collection
with one mutex. It is not concurrent with mutator pointer writes unless those
writes use the root-store API. TLABs, bump regions, safepoints, generational or
incremental collection, and concurrent mutation remain unimplemented. They are
not safe to add as allocator-only fast paths without first defining and testing
a mutator safepoint protocol. A threaded correctness test races root-store
mutation against explicit collection and checks reachability and reclamation;
this does not establish safety for raw uninstrumented writes. No GC benchmark
was run for this follow-up.

## Phase 6.9 native resources

File resources and native mutex handles now have explicit close/destroy behavior
and deterministic scope cleanup. Mutexes reject recursive acquisition,
wrong-thread unlock, and destruction while locked. `defer` runs in LIFO order on
normal scope exit, return, break, and continue. Exception/error unwinding is not
implemented because the language has no unwind model. The runtime remains
Windows-only. Scheduler task and device-memory introspection remain unavailable
because those runtime subsystems do not yet exist.

## Remaining roadmap work

- Ownership is lexical and conservative. Aggregate/partial moves, closure
  capture, general returned references, and arbitrary interprocedural lifetime
  inference are not supported. Views are deliberately not storable in
  aggregates.
- Array constructors use the selected allocator and natural element alignment;
  allocator-specific containers, explicit SIMD alignment, mutable views, and
  tensor/GPU buffer types remain open.
- The explicit allocator still uses global synchronization and allocation
  ownership bookkeeping. Per-thread fast paths require allocator thread-safety
  and lifetime review; no lock removal has been made without evidence.
- The C++/Rust benchmark matrix and runner exist, but comparative runs require
  a Rust toolchain and controlled release configuration. No comparative
  performance claim is made. Phase 6U:G remains a later measurement task; GC
  benchmarking is intentionally deferred.

## Explicit allocator synchronization

Stateful operations now use a per-allocator mutex. Allocation selects and pins
the allocator under the registry lock, performs allocator work outside that
global lock, then records ownership and profiler data under the registry lock.
The temporary pin prevents reset or destruction while a callback or allocator
operation is in flight. A blocking custom-allocator regression test verifies
that an explicit allocation prevents concurrent destruction until registration
completes. Different allocator instances can perform allocation work in
parallel; calls through the same allocator remain serialized.

The live-allocation registry, shared reference counts, selection state, and
profiler snapshot still use global synchronization. Deallocation and
reallocation still hold that lock while invoking allocator operations. This is
a correctness-preserving first reduction of allocator lock scope, not a claim
that contention has been eliminated. The existing allocator benchmark matrix
remains the measurement path; no GC benchmark was run.
