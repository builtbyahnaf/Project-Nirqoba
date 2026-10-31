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

extern "C" {

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
