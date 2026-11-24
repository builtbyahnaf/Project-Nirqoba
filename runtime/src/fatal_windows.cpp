// runtime/src/fatal_windows.cpp
// Windows implementation of nrq_runtime_fatal.
//
// Implementation notes:
//   - Uses WriteFile on the standard error handle for the same reason as
//     io_windows.cpp: avoids a CRT stdio dependency so the boundary stays
//     clean for GC integration in Phase 6.5.
//   - Exit code 70 (EX_SOFTWARE) is chosen to match the POSIX convention for
//     internal software errors. It is distinct from the compiler exit codes
//     (1-4) and from language-level exit codes (0 and user-controlled int).
//   - The message is written to stderr before ExitProcess so the caller's
//     diagnostic appears even if stdout is closed.

#ifndef _WIN32
#  error "fatal_windows.cpp is Windows-only. Linux support is a future milestone."
#endif

#include "nirqoba/runtime/fatal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

using namespace std;

namespace {

// One write per fragment, with no CRT stdio: the same reason the fatal path uses
// WriteFile. A missing or redirected standard error handle simply writes nothing;
// the exit code is the part a caller can rely on.
void write_standard_error(const char* text) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr || text == nullptr) return;
    const DWORD len = static_cast<DWORD>(std::strlen(text));
    if (len == 0) return;
    DWORD written = 0;
    WriteFile(h, text, len, &written, nullptr);
}

// `prefix` + `message` + newline. The prefix is written even when the message is
// empty, so `panic("")` and `assert(false)` name themselves in the log.
[[noreturn]] void write_prefixed_and_exit(const char* prefix,
                                          const char* message) {
    write_standard_error(prefix);
    if (message != nullptr && message[0] != '\0') write_standard_error(message);
    write_standard_error("\n");
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h != nullptr) FlushFileBuffers(h);
    ExitProcess(70);
}

}  // namespace

extern "C" {

[[noreturn]] void nrq_panic(const char* message) {
    write_prefixed_and_exit("panic: ", message);
}

[[noreturn]] void nrq_assert_failed(const char* message) {
    if (message != nullptr && message[0] != '\0')
        write_prefixed_and_exit("assertion failed: ", message);
    write_prefixed_and_exit("assertion failed", nullptr);
}

[[noreturn]] void nrq_runtime_fatal(const char* message) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h != nullptr && message != nullptr) {
        const DWORD len = static_cast<DWORD>(std::strlen(message));
        if (len > 0) {
            DWORD written = 0;
            WriteFile(h, message, len, &written, nullptr);
        }
        const char newline = '\n';
        DWORD written = 0;
        WriteFile(h, &newline, 1, &written, nullptr);
        FlushFileBuffers(h);
    }
    ExitProcess(70);
}

}  // extern "C"
