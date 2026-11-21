// The FFI release path.
//
// Ownership can cross the boundary in both directions. When it comes back, the
// buffer was produced by the foreign allocator, so only that allocator can
// release it: the runtime's deterministic releases know the provenance of their
// own pointers and abort on anything they did not hand out. Keeping the foreign
// release in one entry point is what lets the language bind it as the destructor
// of a value whose storage it never allocated.

#include "nirqoba/runtime/ffi.h"

#include <stdlib.h>

void nrq_ffi_free(void* ptr) {
    // Null is the documented "no value" from a C producer, and `free` already
    // makes it a no-op, so one release path covers both outcomes.
    free(ptr);
}
