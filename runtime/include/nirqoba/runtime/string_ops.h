// runtime/include/nirqoba/runtime/string_ops.h
// Text operations for the language's two string types.
//
// `string` is a borrowed NUL-terminated byte buffer: the language does not own
// it and never releases it. `string_owned` is a buffer this runtime allocated
// from the deterministic allocator; `nrq_file_text_release` hands it back, and
// the compiler installs that release as the value's destructor.
//
// Every function here that returns a buffer returns a *newly allocated* one, so
// it is a `string_owned` on the language side. Nothing is ever borrowed back out
// of an owned buffer: a caller that wants to keep part of a string gets a copy,
// which is what makes the ownership of the result unambiguous.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Byte length of `text`, treating null as the empty string. Backs `string.length`
// as well as the concatenation sizing below.
int64_t nrq_string_length(const char* text);

// 1 when both strings hold the same bytes, 0 otherwise. A null argument is the
// empty string, so two nulls are equal and null matches "".
int32_t nrq_string_equals(const char* left, const char* right);

// Concatenates two byte strings into a newly allocated buffer. Either argument
// may be null (empty).
char* nrq_string_concat(const char* left, const char* right);

// The `index`th byte of `text` as an unsigned code unit, or 0 when `index` is
// outside the buffer. A NUL-terminated buffer has no separate length word, so
// this is the one place a byte index is checked against the text itself; reading
// the byte directly would run off the end for an out-of-range index.
int32_t nrq_string_byte_at(const char* text, int64_t index);

// Copies `text[start, end)` into a newly allocated buffer. Both bounds are
// clamped to the string's byte length, and a reversed or empty range yields the
// empty string, so no slice can read past the buffer.
char* nrq_string_slice(const char* text, int64_t start, int64_t end);

// Base-10 text of an integer, newly allocated: "-42" for -42.
char* nrq_string_from_int(int64_t value);

// Shortest round-trippable decimal text of a double, newly allocated. An
// infinity or NaN formats by name, so no value is silently rendered as zero.
char* nrq_string_from_float(double value);

// Leading base-10 integer in `text`, skipping leading whitespace. Text with no
// digits is 0, which is the only answer a total function can give.
int64_t nrq_string_to_int(const char* text);

// Leading decimal floating-point value in `text`, skipping leading whitespace.
// Text with no number is 0.0.
double nrq_string_to_float(const char* text);

#ifdef __cplusplus
}
#endif
