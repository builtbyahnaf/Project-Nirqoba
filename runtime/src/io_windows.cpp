// runtime/src/io_windows.cpp
// Windows x64 implementation of nrq_print_string, nrq_runtime_init and
// nrq_runtime_shutdown.
//
// Implementation notes:
//
//   1. WriteFile is used instead of printf/puts because this boundary must be
//      replaceable when the GC is introduced in Phase 6.5. CRT stdio buffers
//      and internal state would make that replacement harder. WriteFile goes
//      directly to the Win32 handle, which the GC can own and flush on
//      collection cycles.
//
//   2. A partial-write loop handles the case where WriteFile writes fewer
//      bytes than requested (unusual on Windows console handles but possible
//      on redirected handles under low-memory conditions).
//
//   3. The newline is a single '\n'. Windows console mode translates '\n' to
//      "\r\n" when ENABLE_PROCESSED_OUTPUT is set (the default), so the
//      output looks correct in both console and file redirection contexts.
//
//   4. nrq_runtime_init and nrq_runtime_shutdown are currently no-ops. Their
//      ABI exists so Phase 6.5 can wire in GC initialisation without changing
//      the generated entry-point stub.
//
// Non-Windows compilation fails here with an explicit #error. There is no
// silent stub; Linux support is scheduled for its own platform milestone.

#ifndef _WIN32
#  error "io_windows.cpp is the Windows-only runtime I/O implementation. \
Linux support will be added in the dedicated platform milestone."
#endif

#include "nirqoba/runtime/io.h"
#include "nirqoba/runtime/fatal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Returns the cached standard output handle. Obtained once at first call.
// We do not cache it at startup because the handle can be redirected by
// the parent process before it calls WriteFile; caching it lazily is safe
// on single-threaded Phase 1 programs.
static HANDLE get_stdout_handle() {
    static HANDLE h = INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) {
        h = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    return h;
}

// Writes `size` bytes from `data` to `handle`, looping on short writes.
// Returns 0 on success, the Windows error code on failure.
static int32_t write_all(HANDLE handle, const char* data, DWORD size) {
    DWORD written = 0;
    while (written < size) {
        DWORD chunk = 0;
        if (!WriteFile(handle, data + written, size - written, &chunk, nullptr)) {
            return static_cast<int32_t>(GetLastError());
        }
        written += chunk;
    }
    return 0;
}

extern "C" {

int32_t nrq_print_string(const char* utf8, size_t byte_len) {
    HANDLE h = get_stdout_handle();
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        // No output handle — the process was launched without one. Silently
        // succeed so programs piped to /dev/null (NUL on Windows) do not crash.
        return 0;
    }

    // Write the payload bytes (may be zero for an empty string).
    if (byte_len > 0) {
        const int32_t err = write_all(h, utf8, static_cast<DWORD>(byte_len));
        if (err != 0) return err;
    }

    // Write the trailing newline.
    const char newline = '\n';
    return write_all(h, &newline, 1);
}

void nrq_runtime_init(void) {
    // Phase 1: no initialisation needed.
    // Phase 6.5 will initialise the GC heap and root registry here.
}

void nrq_runtime_shutdown(void) {
    // Phase 1: flush stdout by calling FlushFileBuffers. This ensures the
    // last nrq_print_string output is visible before the process exits, even
    // when stdout is redirected to a pipe or file.
    HANDLE h = get_stdout_handle();
    if (h != INVALID_HANDLE_VALUE && h != nullptr) {
        FlushFileBuffers(h);
    }
    // Phase 6.5 will drain the GC and release managed memory here.
}

}  // extern "C"
