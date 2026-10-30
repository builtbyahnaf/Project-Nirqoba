# 06 — Runtime Architecture

## 1. Runtime ABI Table

The Phase 1 runtime interface is defined in `runtime/include/nirqoba/runtime/`:

| Function | Signature | Purpose |
|---|---|---|
| `nrq_print_string` | `int32_t(const char* utf8, size_t byte_len)` | Writes UTF-8 bytes + `\n` to stdout handle |
| `nrq_runtime_init` | `void()` | Initialises runtime state; GC root boundary in Phase 6.5 |
| `nrq_runtime_shutdown` | `void()` | Flushes stdout buffers; GC cleanup boundary in Phase 6.5 |
| `nrq_runtime_fatal` | `void(const char* message)` | Writes error message to stderr and exits with code 70 |
| `nrq_runtime_version` | `const char*()` | Returns runtime version string (`0.1.0-phase1`) |

## 2. Direct Win32 I/O

The Windows implementation in `runtime/src/io_windows.cpp` deliberately uses `WriteFile` rather than CRT `stdio` (`printf`, `puts`):
- Avoids hidden CRT buffering and internal locking issues.
- Leaves the file handle and buffer management ready for GC heap ownership in Phase 6.5.
- Handles partial writes via a dedicated loop.
