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
  unit/gc_debug_test.cpp
  unit/gc_slot_index_test.cpp
  unit/slot_index_test.cpp
  unit/allocator_test.cpp
  unit/file_test.cpp
  unit/mapping_test.cpp
  unit/sync_test.cpp
  # unit/socket_test.cpp is excluded while the socket runtime is unlinked: the
  # test links the socket entry points, so it can only build once that runtime
  # is part of the build again. The file stays in the tree.
  unit/tir_test.cpp
  unit/nrq_dialect_test.cpp
  unit/mlir_pipeline_test.cpp
  unit/temporary_artifact_test.cpp
  unit/cli_test.cpp
  unit/optimization_test.cpp
  unit/target_test.cpp
)

set_property(TARGET nirqoba_unit_tests PROPERTY CXX_STANDARD 20)
set_property(TARGET nirqoba_unit_tests PROPERTY CXX_STANDARD_REQUIRED ON)
nirqoba_set_target_warnings(nirqoba_unit_tests)

target_include_directories(nirqoba_unit_tests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_SOURCE_DIR}/runtime/src"                # internal SlotIndex header
    "${CMAKE_SOURCE_DIR}/compiler/include"
    "${CMAKE_BINARY_DIR}/include"                    # generated Version.h
    "${CMAKE_BINARY_DIR}/compiler/include"           # TableGen .inc files
)

target_link_libraries(nirqoba_unit_tests
  PRIVATE
    nirqoba_compiler_lib
    # The unit tests audit observation as well as behaviour, so they link the
    # development runtime. Its configuration definition is PUBLIC on that target,
    # so the test binary is compiled from the same macro the library was. The
    # production runtime is exercised end to end by tests/run_tests.py, which
    # builds programs without --debug-enabled.
    nirqoba_runtime_debug
)

# Register with ctest so `ctest --preset windows-msvc-dev` finds it.
add_test(
  NAME nirqoba_unit_tests
  COMMAND nirqoba_unit_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
