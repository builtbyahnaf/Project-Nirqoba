// runtime/include/nirqoba/runtime/fatal.h
// Unrecoverable-error helper for the Nirqoba runtime.
//
// nrq_runtime_fatal is called by the compiler-generated code when an internal
// invariant is violated at runtime (e.g. a null pointer that should never be
// null). It is distinct from language-level exceptions (which do not exist in
// Phase 1) and from compile-time diagnostics.
//
// The function never returns.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Writes `message` to the standard error handle and terminates the process
// with exit code 70 (EX_SOFTWARE on POSIX, chosen for consistency). The
// message should not include a trailing newline; the runtime appends one.
//
// This function is marked [[noreturn]] only in C++ translation units because
// the C standard before C23 has no equivalent attribute.
#ifdef __cplusplus
[[noreturn]]
#endif
void nrq_runtime_fatal(const char* message);

#ifdef __cplusplus
}
#endif
