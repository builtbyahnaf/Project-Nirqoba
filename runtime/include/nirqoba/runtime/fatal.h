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

// Language-level panic: writes "panic: <message>" to the standard error handle
// and terminates with exit code 70. A null or empty message still names the
// panic, so a bare `panic("")` is visible rather than a silent exit.
//
// Unlike nrq_runtime_fatal this is reached from source, so the message is the
// program's own text; the prefix is what distinguishes a deliberate panic from a
// violated internal invariant in the same log.
#ifdef __cplusplus
[[noreturn]]
#endif
void nrq_panic(const char* message);

// A failed `assert`: writes "assertion failed: <message>" (or just "assertion
// failed" for an assertion without a message) to the standard error handle and
// terminates with exit code 70. Same exit path as a panic so a test can assert on
// one code for both.
#ifdef __cplusplus
[[noreturn]]
#endif
void nrq_assert_failed(const char* message);

#ifdef __cplusplus
}
#endif
