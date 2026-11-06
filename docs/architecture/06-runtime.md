# 06 — Runtime Architecture

## 1. Runtime ABI Table

The Phase 1 runtime interface is defined in `runtime/include/nirqoba/runtime/`:

| Function | Signature | Purpose |
|---|---|---|
| `nrq_print_string` | `int32_t(const char* utf8, size_t byte_len)` | Writes UTF-8 bytes + `\n` to stdout handle |
| `nrq_runtime_init` | `void()` | Initialises runtime state; GC root boundary in Phase 6.5 |
| `nrq_runtime_shutdown` | `void()` | Flushes stdout buffers; GC cleanup boundary in Phase 6.5 |
| `nrq_runtime_fatal` | `void(const char* message)` | Writes error message to stderr and exits with code 70 |
| `nrq_runtime_version` | `const char*()` | Returns runtime version string (`0.181-beta-phase6`) |
| `nrq_gc_root_register_frame` | `void(void* const* slots, size_t count, const size_t* descriptor)` | Publishes an activation's ordinary roots in one call from a read-only frame descriptor |

## 2. Direct Win32 I/O

The Windows implementation in `runtime/src/io_windows.cpp` deliberately uses `WriteFile` rather than CRT `stdio` (`printf`, `puts`):
- Avoids hidden CRT buffering and internal locking issues.
- Leaves the file handle and buffer management ready for GC heap ownership in Phase 6.5.
- Handles partial writes via a dedicated loop.

## 3. GC Root Publication

Generated code registers roots before any operation that can collect, and the
collector discovers them through the thread's shadow stack.

- **Per-slot registration.** `nrq_gc_root_register` /
  `nrq_gc_root_register_typed` publish one slot with its object size and
  reference layout. They remain the entry points for hand-written native code,
  for a lone root, and wherever the layout is only known at run time.
- **Frame registration.** An activation's root set is a compile-time fact, so the
  lowering emits one `private constant` descriptor per root set and calls
  `nrq_gc_root_register_frame` once with the descriptor and an array of slot
  addresses. The descriptor is interned per module, so functions that register the
  same layouts share one table in `.rodata`, and an activation pays one barrier
  region instead of one per root. The runtime validates the whole table before it
  publishes any slot, keeps the same per-slot duplicate counts, and marks a root
  against an in-flight snapshot exactly as the per-slot entry point does.
