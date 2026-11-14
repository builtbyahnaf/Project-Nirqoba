// runtime/src/io_windows.cpp
// Windows x64 implementation of nrq_print_*, nrq_runtime_init and nrq_runtime_shutdown.

#ifndef _WIN32
#  error "io_windows.cpp is the Windows-only runtime I/O implementation."
#endif

#include "nirqoba/runtime/io.h"
#include "nirqoba/runtime/fatal.h"

#include "runtime_buffers.h"
#include "runtime_shutdown.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <mutex>

static HANDLE get_stdout_handle() {
    static HANDLE h = INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) {
        h = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    return h;
}

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
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return 0;

    if (byte_len > 0) {
        const int32_t err = write_all(h, utf8, static_cast<DWORD>(byte_len));
        if (err != 0) return err;
    }

    const char newline = '\n';
    return write_all(h, &newline, 1);
}

int32_t nrq_print_cstr(const char* utf8) {
    if (utf8 == nullptr) return nrq_print_string("", 0);

    size_t len = 0;
    while (utf8[len] != '\0') ++len;
    return nrq_print_string(utf8, len);
}

int32_t nrq_print_int(int64_t val) {
    HANDLE h = get_stdout_handle();
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return 0;

    char buf[32];
    int len = 0;
    uint64_t uval;
    if (val < 0) {
        buf[len++] = '-';
        uval = static_cast<uint64_t>(-(val + 1)) + 1;
    } else {
        uval = static_cast<uint64_t>(val);
    }

    char digits[24];
    int dcount = 0;
    do {
        digits[dcount++] = static_cast<char>('0' + (uval % 10));
        uval /= 10;
    } while (uval > 0);

    while (dcount > 0) {
        buf[len++] = digits[--dcount];
    }
    buf[len++] = '\n';

    return write_all(h, buf, static_cast<DWORD>(len));
}

// §8.6: a pointer prints as its address and is never dereferenced. A null pointer
// prints as NULL so absence is distinguishable from a legitimate address.
int32_t nrq_print_pointer(const void* value) {
    if (value == nullptr) return nrq_print_string("NULL", 4);
    return nrq_print_uint(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)));
}

int32_t nrq_print_uint(uint64_t val) {
    HANDLE h = get_stdout_handle();
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return 0;

    char buf[32];
    int len = 0;

    char digits[24];
    int dcount = 0;
    do {
        digits[dcount++] = static_cast<char>('0' + (val % 10));
        val /= 10;
    } while (val > 0);

    while (dcount > 0) {
        buf[len++] = digits[--dcount];
    }
    buf[len++] = '\n';

    return write_all(h, buf, static_cast<DWORD>(len));
}

int32_t nrq_print_f64(double val) {
    HANDLE h = get_stdout_handle();
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return 0;

    char buf[64];
    int n = snprintf(buf, sizeof(buf) - 2, "%.15g", val);
    if (n < 0) n = 0;
    buf[n++] = '\n';
    return write_all(h, buf, static_cast<DWORD>(n));
}

int32_t nrq_print_f32(float val) {
    return nrq_print_f64(static_cast<double>(val));
}

int32_t nrq_print_bool(int32_t val) {
    HANDLE h = get_stdout_handle();
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return 0;

    if (val != 0) {
        return write_all(h, "true\n", 5);
    } else {
        return write_all(h, "false\n", 6);
    }
}

int32_t nrq_print_char(uint32_t cp) {
    HANDLE h = get_stdout_handle();
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return 0;

    char buf[8];
    int len = 0;
    if (cp <= 0x7F) {
        buf[len++] = static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        buf[len++] = static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
        buf[len++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        buf[len++] = static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
        buf[len++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[len++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        buf[len++] = static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
        buf[len++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        buf[len++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[len++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    buf[len++] = '\n';

    return write_all(h, buf, static_cast<DWORD>(len));
}

void nrq_runtime_init(void) {
}

void nrq_runtime_shutdown(void) {
    // `system.exit` reaches shutdown from an arbitrary language call site, and
    // the normal entry return reaches it too. Serialise them so a terminating
    // thread cannot close a registry while another thread is walking it.
    static std::mutex shutdown_mutex;
    std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex);

    // Output uses unbuffered WriteFile calls, so there is nothing to flush.
    // FlushFileBuffers on an anonymous pipe blocks until the reader drains it,
    // which deadlocked any parent that waited for this process before reading
    // its stdout. Shutdown now retires runtime-owned buffers only.
    // Program code normally closes native resources explicitly. A live handle
    // still owned at return is nevertheless released here, so process exit is
    // not the owner of last resort. Registries are cleared before their handles
    // are closed, making a stale token invalid rather than race-prone.
    nirq::runtime::release_file_resources();
    nirq::runtime::release_mapping_resources();
    nirq::runtime::release_mutex_resources();

    // `file.read_text` strings are process-lifetime values. They are not
    // released here: the language can copy, return, aggregate and share the
    // pointer without a release point, so freeing the arena at entry return
    // would make a still-reachable alias dangle. The OS reclaims the arena at
    // process termination, after deferred cleanup and static destructors.
    // `release_process_strings()` remains an internal test/forced-teardown hook
    // and must only be used when no string can still be observed.
}

// Console input: reads one line from stdin using Win32 ReadFile.
// Strips trailing '\r' and '\n'. Writes byte count to *out_bytes_read.
// Returns 0 on success or Win32 error code on failure.
int32_t nrq_read_line(char* out_buf, size_t max_bytes, size_t* out_bytes_read) {
    if (!out_buf || max_bytes == 0) return ERROR_INVALID_PARAMETER;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        if (out_bytes_read) *out_bytes_read = 0;
        out_buf[0] = '\0';
        return static_cast<int32_t>(GetLastError());
    }

    // Read byte by byte until '\n', EOF, or buffer exhaustion.
    size_t total = 0;
    while (total < max_bytes - 1) {
        char c;
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(h, &c, 1, &bytes_read, nullptr);
        if (!ok || bytes_read == 0) break;  // EOF or error
        if (c == '\n') break;               // end of line
        if (c == '\r') continue;            // strip carriage return
        out_buf[total++] = c;
    }
    out_buf[total] = '\0';
    if (out_bytes_read) *out_bytes_read = total;
    return 0;
}

}  // extern "C"
