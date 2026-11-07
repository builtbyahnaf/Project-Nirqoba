// Runtime configuration selected at build time.
//
// The runtime is built twice from one set of sources:
//
//   nirqoba_runtime        production path, linked by default
//   nirqoba_runtime_debug  development path, linked by --debug-enabled
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

// The ABI symbol this library defines, as a string. runtime_abi.cpp has to spell
// the definition out, so this constant and that definition are the two places the
// name appears in the runtime; the compiler names the same symbol from
// RuntimeConfiguration.h, and a unit test compares the two.
inline constexpr const char* runtime_abi_symbol_name =
    NRQ_DEBUG_ENABLED ? "nrq_runtime_abi_v1_development"
                      : "nrq_runtime_abi_v1_production";

// The library file this configuration produces. The driver derives the same name
// from the compiler's RuntimeConfiguration.
inline constexpr const char* runtime_library_file_name =
    NRQ_DEBUG_ENABLED ? "nirqoba_runtime_debug.lib" : "nirqoba_runtime.lib";

}  // namespace nirq
