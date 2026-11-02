# Phase 6.9 follow-up: deterministic native resources

Date: 2026-11-01

The advanced runtime now includes owned file handles and owned native mutexes
in addition to the profiler, mappings, pinning, secure zeroization, reference
diagnostics, and runtime introspection documented in the original Phase 6.9
report.

File handles support explicit read/write operations and deterministic close.
The compiler tracks ownership transfer and rejects use after move or close.
Mutex handles support create, lock, unlock, and destroy. The runtime rejects
recursive locking, wrong-thread unlock, invalid handles, and destroying a locked
mutex. Compiler-generated cleanup destroys mutexes at scope exit; an explicit
deferred unlock runs first due to LIFO cleanup.

Validation: RelWithDebInfo build succeeded, CTest passed, and integration cases
cover file resource reads/writes, ownership transfer, mutex serialization,
deferred unlock, automatic destruction, and invalid use after destroy.

Limits: wrappers are Windows-only. Socket, graphics, and GPU resources are not
available because their runtime subsystems do not exist. Cleanup on exception
unwinding is not supported. Task count and device-memory introspection are also
unavailable until a scheduler and device runtime are implemented.

File resources now use monotonically issued opaque tokens instead of heap
addresses. A closed token can never become valid again if the system allocator
reuses its former metadata address. A runtime regression closes one file,
reopens another, and verifies the stale token cannot write or close the new
resource. RelWithDebInfo validation passed: CTest 1/1 and integration 85/85.

File reads, writes, and close now use per-resource locking with shared resource
records. The global registry lock only covers token lookup/removal, so slow I/O
on one file does not hold up other resource operations. Concurrent close waits
for any operation that already acquired that file and rejects later operations.
A four-thread write/close regression is included.
