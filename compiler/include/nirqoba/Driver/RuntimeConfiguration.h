// RuntimeConfiguration.h — the single statement of what a configuration is.
//
// Three facts have to agree for a build to mean anything:
//
//   1. what the translator emits (instrumentation hooks or nothing),
//   2. which ABI symbol the emitted module requires, and
//   3. which runtime library the driver links.
//
// Previously the CLI flag reached each of them on its own path, so a caller that
// consulted two of the three could produce a build whose parts disagreed. They
// now hang off one value: pass a RuntimeConfiguration and every derived fact
// follows from it. The runtime side mirrors this in runtime/src/runtime_config.h,
// and the link enforces the pair (`nrq_runtime_abi_v1_*` in
// runtime/src/runtime_abi.cpp).

#pragma once

namespace nirq {

enum class RuntimeConfiguration {
  // The default. The program is linked against nirqoba_runtime, and the lowering
  // emits no observation hooks at all: there is nothing to strip, because
  // nothing was emitted.
  Production,
  // Selected by `--debug-enabled`. The program is linked against
  // nirqoba_runtime_debug and the lowering emits allocation-site declarations and
  // tags that only that library provides.
  Development,
};

constexpr RuntimeConfiguration runtime_configuration(bool debug_enabled) {
  return debug_enabled ? RuntimeConfiguration::Development
                       : RuntimeConfiguration::Production;
}

constexpr bool is_development(RuntimeConfiguration configuration) {
  return configuration == RuntimeConfiguration::Development;
}

// The symbol the emitted entry point calls. Each runtime library defines exactly
// one of these, so the module and the library must have been built for the same
// configuration or the link fails.
constexpr const char* runtime_abi_symbol(RuntimeConfiguration configuration) {
  return is_development(configuration) ? "nrq_runtime_abi_v1_development"
                                       : "nrq_runtime_abi_v1_production";
}

// The library file the driver links, resolved through the same search paths for
// both configurations.
constexpr const char* runtime_library_name(RuntimeConfiguration configuration) {
  return is_development(configuration) ? "nirqoba_runtime_debug.lib"
                                       : "nirqoba_runtime.lib";
}

// The CMake target that produces `runtime_library_name`. Spelled here so the
// diagnostic a missing library produces names the target to build.
constexpr const char* runtime_library_target(RuntimeConfiguration configuration) {
  return is_development(configuration) ? "nirqoba_runtime_debug"
                                       : "nirqoba_runtime";
}

}  // namespace nirq
