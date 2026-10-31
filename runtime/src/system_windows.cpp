// runtime/src/system_windows.cpp
// Windows x64 implementation of System, Time, GC, and Memory Allocation functions for Nirqoba Phases 6.5-6.7 using Win32 API.

#ifndef _WIN32
#  error "system_windows.cpp is the Windows-only runtime system implementation."
#endif

#include "nirqoba/runtime/system.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>


extern "C" {

int64_t nrq_time_now_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    // Combine high and low parts into a 64-bit integer (100-ns intervals since Jan 1, 1601).
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    // Difference between Windows epoch (1601) and Unix epoch (1970) in 100-nanosecond units:
    // 116444736000000000ULL
    const unsigned __int64 kEpochDiff = 116444736000000000ULL;
    if (uli.QuadPart < kEpochDiff) return 0;

    unsigned __int64 unix_100ns = uli.QuadPart - kEpochDiff;
    // 10,000 intervals of 100ns = 1 millisecond.
    return static_cast<int64_t>(unix_100ns / 10000ULL);
}

void nrq_system_exit(int32_t exit_code) {
    ExitProcess(static_cast<UINT>(exit_code));
}

}  // extern "C"
