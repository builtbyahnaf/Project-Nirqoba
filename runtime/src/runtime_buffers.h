// Internal runtime buffer ownership.
//
// Some runtime entry points return a NUL-terminated string whose lifetime is the
// process, because the language's `string` is a borrowed pointer and has no
// destruction hook. The file-reading builtin is the current case. Those buffers
// are immutable for the rest of the process lifetime: copies, returns,
// aggregate fields and sharing all copy the pointer, so there is no sound
// per-string release point. The arena is process-owned; the OS reclaims it at
// termination. `release_process_strings()` is an internal test/forced-teardown
// hook, not part of normal shutdown.
//
// This is not a general allocator and is not exposed through the public C ABI.
#pragma once

#include <cstddef>

namespace nirq::runtime {

// Returns storage for `bytes` bytes, or nullptr when the arena cannot commit a
// block. The returned storage is zeroed only when the underlying block is fresh;
// callers must initialise every byte they observe.
char* allocate_process_string(std::size_t bytes) noexcept;

// Releases every committed block. Any pointer previously returned from this
// arena becomes invalid. Safe to call repeatedly, including when empty. Normal
// runtime shutdown does not call this; only tests or a forced teardown with no
// live aliases may.
void release_process_strings() noexcept;

// Committed payload bytes, excluding block headers. Used by tests and runtime
// diagnostics to prove that shutdown released the arena.
std::size_t process_string_bytes() noexcept;

}  // namespace nirq::runtime
