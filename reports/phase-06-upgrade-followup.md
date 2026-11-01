# Phase 6 Upgrade follow-up

Date: 2026-11-01

## Ownership and contiguous storage

The explicit ownership path now supports unique pointer and owned-array moves,
owned and shared parameter/return contracts, checked borrowed pointer parameters,
read-only array views, and compiler-inserted deterministic cleanup for tracked
owners and supported resources. Resource-bearing structs, tuples, and fixed
arrays transfer as whole values and recursively drop nested owned arrays.
Partial field extraction, replacement, and destructuring are diagnosed so a
nested allocation cannot acquire two independent cleanup paths. `owned_array<T>` stores a contiguous element
buffer and length. `array_view<T>` is a non-owning descriptor that supports
checked nested slicing and function parameters. Views cannot escape through
returns or be written through. Their lexical borrow blocks owner moves, release,
and mutation while the view binding is live.

Ownership state is checked across both `if` branches and conservatively merged;
loop back-edges reject owners left moved. Calls track temporary and named view
borrows against owner transfers and reject duplicate transfers of the same token.
Views cannot be nested into aggregates, returned through aggregate types, or
stored in aggregate/allocated memory. Resource-bearing aggregates move whole;
raw pointer fields with owned, shared, or borrowed ownership are rejected until
field ownership can be declared.

Validation on Windows x64, MSVC, and LLVM/MLIR 23.1.4: RelWithDebInfo build,
CTest 1/1, and integration 81/81 passed after aggregate, branch/loop, and
call-borrow changes. Added coverage exercises nested aggregate return/parameter
cleanup, both branches transferring an owner, possibly-moved branch rejection,
loop back-edge rejection, duplicate and conflicting call arguments, borrowed
view/pointer field restrictions, nested owned-array type rejection, and GC
tracing through an aligned tuple nested in an owned array.

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
writes use the root-store API. TLABs, safepoints, generational or incremental
collection, and concurrent mutation remain unimplemented. Small managed objects
now use aligned 64-KiB bump regions under the collector lock; large objects get
dedicated regions. Sweeping destroys per-object trace metadata and returns a
region only after its live-object count reaches zero. These regions reduce
backing allocator calls but do not reduce global-lock synchronization. TLABs
and concurrent collection need a mutator safepoint protocol before they can be
added safely. A threaded correctness test races root-store
mutation against explicit collection and checks reachability and reclamation;
this does not establish safety for raw uninstrumented writes. No GC benchmark
was run for this follow-up. Root unregistration must match a registration on
the current thread, and generated root frames are checked for valid LIFO
closure.

Owned arrays whose element types contain GC-managed references register their
contiguous backing store as a repeated-layout GC root region. Array cleanup
unregisters that region before releasing the buffer. A unit test verifies the
runtime repeated-offset API, and an integration test verifies an owned array
keeps a managed object alive after its last local pointer is overwritten and
collection runs.

The collector's exact-address object index is now a contiguous open-addressed
table rather than a node-based `unordered_map`. It grows during serialized
allocation, uses tombstones during sweep, and performs lookup without per-object
hash-node allocation. The existing allocation-pressure and cycle/root tests
exercise growth, lookup, sweep, and subsequent reuse; no timings were collected.

## Phase 6.9 native resources

File resources and native mutex handles now have explicit close/destroy behavior
and deterministic scope cleanup. Mutexes reject recursive acquisition,
wrong-thread unlock, and destruction while locked. `defer` runs in LIFO order on
normal scope exit, return, break, and continue. Exception/error unwinding is not
implemented because the language has no unwind model. The runtime remains
Windows-only. Scheduler task and device-memory introspection remain unavailable
because those runtime subsystems do not yet exist.

## Remaining roadmap work

- Ownership is lexical and conservative. Partial aggregate moves, closure
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

The benchmark harness now separates 16-byte allocation, 4-KiB allocation, and
alternating mixed-size allocation workloads, in addition to its 64-byte,
scalar, contiguous-array, graph, and four-thread workloads. The C++/Rust runner
checks checksums for each workload and records repeated percentile samples and
environment metadata. This runner was not executed in this follow-up because
the current environment has neither `cl.exe` in the active shell nor
`rustc.exe`; no GC workload was run.

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
