// runtime/include/nirqoba/runtime/args.h
// Process argument bridge for `main(string[] args)`.
//
// The compiler lowers a `string[]` value to a pointer to an NrqStringArray.
// The array is allocated by the runtime from the process arguments and lives
// for the whole process, so no program code ever frees it.
//
// `items` holds the arguments *after* the executable path (the C `argv[0]`),
// which is what a program normally means by "its arguments".
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NrqStringArray {
  int64_t length;
  const char* const* items;
} NrqStringArray;

// Builds the argument array from the CRT's `argc`/`argv`. Returns nullptr only
// if the runtime cannot allocate the descriptor; every accessor accepts
// nullptr and behaves as an empty array.
NrqStringArray* nrq_args_build(int32_t argc, const char* const* argv);

// Number of arguments (0 for a null array).
int64_t nrq_args_length(const NrqStringArray* array);

// Argument at `index`, or "" when `index` is out of range.
const char* nrq_args_get(const NrqStringArray* array, int64_t index);

// Byte length of a NUL-terminated string (0 for nullptr). Backs `string.length`.
int64_t nrq_strlen(const char* text);

#ifdef __cplusplus
}
#endif
