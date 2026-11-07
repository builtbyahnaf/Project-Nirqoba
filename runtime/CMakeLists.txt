# Runtime static libraries
#
# Provides the C ABI symbols that the compiler emits calls to:
#   nrq_print_string, nrq_runtime_init, nrq_runtime_shutdown,
#   nrq_runtime_fatal, nrq_runtime_version, nrq_print_*, nrq_pow_*, nrq_gc_*.
#
# This library intentionally avoids the C runtime stdio layer (printf, puts)
# so the boundary remains replaceable when the GC is introduced in Phase 6.5.
# The implementation uses Win32 WriteFile directly on Windows.
#
# The same sources are built twice:
#
#   nirqoba_runtime        production path (default)
#   nirqoba_runtime_debug  development path (selected by --debug-enabled)
#
# Only observation differs between them; see src/runtime_config.h for the exact
# contract. The debug library carries the event stream, allocation-site tags,
# root-stack snapshots and the telemetry counters that the production build
# compiles out. Neither configuration changes collection semantics, and neither
# drops a memory-safety check.
#
# Non-Windows compilation fails with a compile-time #error naming the
# platform milestone, which is intentional: there is no silent stub.

cmake_minimum_required(VERSION 3.28)

set(NIRQOBA_RUNTIME_SOURCES
  src/io_windows.cpp
  src/fatal_windows.cpp
  src/runtime_version.cpp
  src/runtime_config.h
  src/runtime_abi.cpp
  src/math.cpp
  src/file_windows.cpp
  src/sync_windows.cpp
  src/socket_windows.cpp
  src/gc.cpp
  src/gc_debug.cpp
  src/gc_debug_internal.h
  src/slot_index.h
  src/system_windows.cpp
  src/args_windows.cpp
  src/allocator_windows.cpp
  src/mapping_windows.cpp
)

# Configures one runtime target from the shared source list. `debug` selects the
# development configuration; nothing else differs, so a change to the runtime
# cannot land in one path without being visible in the other.
function(nirqoba_add_runtime_target target)
  cmake_parse_arguments(ARG "DEBUG" "" "" ${ARGN})
  add_library(${target} STATIC ${NIRQOBA_RUNTIME_SOURCES})
  set_property(TARGET ${target} PROPERTY CXX_STANDARD 20)
  set_property(TARGET ${target} PROPERTY CXX_STANDARD_REQUIRED ON)
  target_include_directories(${target}
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )
  # PUBLIC, so the definition travels with the library: anything that links this
  # target sees the same configuration the target's own objects were compiled
  # with, and there is no second place to keep in step. The production target
  # states 0 rather than relying on the macro being absent, so a consumer that
  # includes a runtime header always gets an answer.
  if(ARG_DEBUG)
    target_compile_definitions(${target} PUBLIC NRQ_RUNTIME_DEBUG_ENABLED=1)
  else()
    target_compile_definitions(${target} PUBLIC NRQ_RUNTIME_DEBUG_ENABLED=0)
  endif()
  nirqoba_set_target_warnings(${target})
  target_link_libraries(${target} PUBLIC ws2_32 dxgi)
  set_target_properties(${target} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
  )
endfunction()

nirqoba_add_runtime_target(nirqoba_runtime)
nirqoba_add_runtime_target(nirqoba_runtime_debug DEBUG)
