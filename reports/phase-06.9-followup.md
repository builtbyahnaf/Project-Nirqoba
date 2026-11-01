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
