// runtime/src/args_windows.cpp
// Windows x64 implementation of the process argument bridge.
//
// The descriptor is process-owned static storage, not a heap allocation. It is
// built once by the compiler-emitted entry wrapper and lives until process exit;
// program code never frees it. The argument pointers themselves are owned by the
// CRT and stay valid for the lifetime of the process, so the array shares them
// instead of copying the bytes.
//
// `nrq_args_build` is called once per process by the entry wrapper. Calling it
// again (as the unit tests do) refreshes the one descriptor in place; it does
// not retain the previous argv.

#ifndef _WIN32
#  error "args_windows.cpp is the Windows-only runtime argument implementation."
#endif

#include "nirqoba/runtime/args.h"

#include <cstring>

using namespace std;

static const char kEmptyString[] = "";
static NrqStringArray g_arguments;

extern "C" {

NrqStringArray* nrq_args_build(int32_t argc, const char* const* argv) {
    // Drop the executable path: program arguments start at argv[1].
    const int64_t count = argc > 1 ? static_cast<int64_t>(argc) - 1 : 0;

    g_arguments.length = count;
    g_arguments.items = (count > 0 && argv != nullptr) ? argv + 1 : nullptr;
    return &g_arguments;
}

int64_t nrq_args_length(const NrqStringArray* array) {
    return array ? array->length : 0;
}

const char* nrq_args_get(const NrqStringArray* array, int64_t index) {
    if (!array || !array->items) return kEmptyString;
    if (index < 0 || index >= array->length) return kEmptyString;
    const char* item = array->items[index];
    return item ? item : kEmptyString;
}

int64_t nrq_strlen(const char* text) {
    return text ? static_cast<int64_t>(std::strlen(text)) : 0;
}

}  // extern "C"
