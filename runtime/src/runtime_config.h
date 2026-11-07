// Runtime configuration selected at build time.
//
// The runtime is built twice from one set of sources:
//
//   nrqoba_runtime        production path, linked by default
//   nrqoba_runtime_debug  development path, linked by --debug-enabled
//
// The two differ only in observation. The debug configuration carries the event
// stream, allocation-site tags and root-stack snapshots, and it keeps the
// telemetry counters (slot-index probes, reuse accounting, root-diagnostic
// synchronisation) that a production build compiles out. Both configurations
// implement the same collection protocol, the same barrier handshake and the
// same root discipline: a debug build never changes GC semantics, and a
// production build never loses a memory-safety check.
//
// Everything the production path keeps is a correctness invariant. Everything
// the debug path adds is observation. That split is what makes it safe to
// measure one build and ship the other.
#pragma once

#if defined(NRQ_RUNTIME_DEBUG_ENABLED) && NRQ_RUNTIME_DEBUG_ENABLED
#define NRQ_DEBUG_ENABLED 1
#else
#define NRQ_DEBUG_ENABLED 0
#endif

namespace nirq {

// True in the debug configuration. The compiler folds this at compile time, so
// guarded code below disappears entirely from a production build.
inline constexpr bool runtime_debug_enabled = NRQ_DEBUG_ENABLED != 0;

}  // namespace nirq
