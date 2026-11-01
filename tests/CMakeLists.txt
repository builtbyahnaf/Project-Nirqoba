# tests/CMakeLists.txt
# Unit test executable (C++) and registration with ctest.
# Integration tests are driven by tests/run_tests.py, not ctest.

cmake_minimum_required(VERSION 3.28)

enable_testing()

add_executable(nirqoba_unit_tests
  support/unit_test_main.cpp

  unit/lexer_test.cpp
  unit/parser_test.cpp
  unit/sema_test.cpp
  unit/diagnostics_test.cpp
  unit/gc_test.cpp
  unit/allocator_test.cpp
  unit/sync_test.cpp
  unit/tir_test.cpp
  unit/nrq_dialect_test.cpp
  unit/cli_test.cpp
)

set_property(TARGET nirqoba_unit_tests PROPERTY CXX_STANDARD 20)
set_property(TARGET nirqoba_unit_tests PROPERTY CXX_STANDARD_REQUIRED ON)
nirqoba_set_target_warnings(nirqoba_unit_tests)

target_include_directories(nirqoba_unit_tests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_SOURCE_DIR}/compiler/include"
    "${CMAKE_BINARY_DIR}/include"                    # generated Version.h
    "${CMAKE_BINARY_DIR}/compiler/include"           # TableGen .inc files
)

target_link_libraries(nirqoba_unit_tests
  PRIVATE
    nirqoba_compiler_lib
    nrqoba_runtime
)

# Register with ctest so `ctest --preset windows-msvc-dev` finds it.
add_test(
  NAME nirqoba_unit_tests
  COMMAND nirqoba_unit_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
