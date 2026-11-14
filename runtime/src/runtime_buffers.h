// Internal runtime buffer ownership.
//
// Some runtime entry points return a NUL-terminated string whose lifetime is the
// process, because the language's `string` is a borrowed pointer and has no
// destruction hook. The file-reading builtin is the current case. Those buffers
// are allocated from one arena owned by the runtime: individual strings stay
// valid until shutdown, and shutdown releases the whole arena in one
// deterministic operation instead of leaking every read until process exit.
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
// arena becomes invalid. Safe to call repeatedly, including when empty.
void release_process_strings() noexcept;

// Committed payload bytes, excluding block headers. Used by tests and runtime
// diagnostics to prove that shutdown released the arena.
std::size_t process_string_bytes() noexcept;

}  // namespace nirq::runtime
