# Runtime static library: nrqoba_runtime
#
# Provides the C ABI symbols that the compiler emits calls to:
#   nrq_print_string, nrq_runtime_init, nrq_runtime_shutdown,
#   nrq_runtime_fatal, nrq_runtime_version.
#
# This library intentionally avoids the C runtime stdio layer (printf, puts)
# so the boundary remains replaceable when the GC is introduced in Phase 6.5.
# The implementation uses Win32 WriteFile directly on Windows.
#
# Non-Windows compilation fails with a compile-time #error naming the
# platform milestone, which is intentional: there is no silent stub.

cmake_minimum_required(VERSION 3.28)

add_library(nrqoba_runtime STATIC
  src/io_windows.cpp
  src/fatal_windows.cpp
  src/runtime_version.cpp
)

set_property(TARGET nrqoba_runtime PROPERTY CXX_STANDARD 20)
set_property(TARGET nrqoba_runtime PROPERTY CXX_STANDARD_REQUIRED ON)

target_include_directories(nrqoba_runtime
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

nirqoba_set_target_warnings(nrqoba_runtime)
