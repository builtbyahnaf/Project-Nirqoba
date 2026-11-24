// runtime/src/string_windows.cpp
// Windows implementation of the text operations behind `string` and
// `string_owned`.
//
// The CRT's formatting and parsing do the numeric work: this file is the
// boundary between them and the language's ownership rules, so every buffer it
// creates comes from the same counted allocator as `file.read_text_owned` and is
// released by the same `nrq_file_text_release` the compiler installs as a
// `string_owned` value's destructor.

#ifndef _WIN32
#  error "string_windows.cpp is the Windows-only runtime string implementation."
#endif

#include "nirqoba/runtime/string_ops.h"

#include "nirqoba/runtime/file.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// A string operand may be null (an uninitialized or failed producer), and the
// language's own empty string is a valid value, so null is read as empty rather
// than rejected: every operation here is total.
const char* or_empty(const char* text) {
    return text ? text : "";
}

size_t byte_length(const char* text) {
    return text ? std::strlen(text) : 0;
}

// Copies `chars` bytes into freshly owned text. The call is the single place a
// string operation touches the allocator, so every producer reports its buffer
// the same way to the live-text counter.
char* own_copy(const char* source, size_t chars) {
    char* buffer = nrq_owned_text_allocate(static_cast<int64_t>(chars) + 1);
    if (!buffer) return nullptr;
    if (chars != 0) std::memcpy(buffer, source, chars);
    buffer[chars] = '\0';
    return buffer;
}

}  // namespace

int64_t nrq_string_length(const char* text) {
    return static_cast<int64_t>(byte_length(text));
}

int32_t nrq_string_equals(const char* left, const char* right) {
    return std::strcmp(or_empty(left), or_empty(right)) == 0 ? 1 : 0;
}

char* nrq_string_concat(const char* left, const char* right) {
    const char* first = or_empty(left);
    const char* second = or_empty(right);
    const size_t first_length = std::strlen(first);
    const size_t second_length = std::strlen(second);
    char* buffer = nrq_owned_text_allocate(
        static_cast<int64_t>(first_length + second_length) + 1);
    if (!buffer) return nullptr;
    if (first_length != 0) std::memcpy(buffer, first, first_length);
    if (second_length != 0)
        std::memcpy(buffer + first_length, second, second_length);
    buffer[first_length + second_length] = '\0';
    return buffer;
}

int32_t nrq_string_byte_at(const char* text, int64_t index) {
    const char* source = or_empty(text);
    if (index < 0) return 0;
    const int64_t length = static_cast<int64_t>(std::strlen(source));
    if (index >= length) return 0;
    return static_cast<int32_t>(static_cast<unsigned char>(source[index]));
}

char* nrq_string_slice(const char* text, int64_t start, int64_t end) {
    const char* source = or_empty(text);
    const int64_t length = static_cast<int64_t>(std::strlen(source));
    if (start < 0) start = 0;
    if (start > length) start = length;
    if (end < start) end = start;
    if (end > length) end = length;
    return own_copy(source + start, static_cast<size_t>(end - start));
}

char* nrq_string_from_int(int64_t value) {
    char digits[32];
    const int written = std::snprintf(digits, sizeof(digits), "%lld",
                                      static_cast<long long>(value));
    if (written < 0) return own_copy("", 0);
    const size_t chars = static_cast<size_t>(written) < sizeof(digits)
                             ? static_cast<size_t>(written)
                             : sizeof(digits) - 1;
    return own_copy(digits, chars);
}

char* nrq_string_from_float(double value) {
    // `%g` drops trailing zeros, which is what makes 2.5 print as "2.5" rather
    // than "2.500000", and the 15 significant digits keep a round trip through
    // nrq_string_to_float exact for ordinary values.
    char digits[64];
    int written = std::snprintf(digits, sizeof(digits), "%.15g", value);
    if (written < 0) return own_copy("", 0);
    if (static_cast<size_t>(written) >= sizeof(digits)) {
        written = std::snprintf(digits, sizeof(digits), "%g", value);
        if (written < 0) return own_copy("", 0);
    }
    const size_t chars = static_cast<size_t>(written) < sizeof(digits)
                             ? static_cast<size_t>(written)
                             : sizeof(digits) - 1;
    return own_copy(digits, chars);
}

int64_t nrq_string_to_int(const char* text) {
    const char* source = or_empty(text);
    char* end = nullptr;
    const long long value = std::strtoll(source, &end, 10);
    if (end == source) return 0;
    return static_cast<int64_t>(value);
}

double nrq_string_to_float(const char* text) {
    const char* source = or_empty(text);
    char* end = nullptr;
    const double value = std::strtod(source, &end);
    if (end == source) return 0.0;
    return value;
}
