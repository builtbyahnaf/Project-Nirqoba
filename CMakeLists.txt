cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# Project declaration
# ---------------------------------------------------------------------------
project(nirqoba VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Project-wide options and helpers
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NirqobaOptions)
include(NirqobaLLVMTargets)

# <windows.h> defines min()/max() as macros, which breaks any translation unit
# that also includes LLVM headers (std::min/std::max become syntax errors).
# Windows is the only supported host, so this applies to the whole build.
if(MSVC)
  add_compile_definitions(NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# LLVM / MLIR SDK
# ---------------------------------------------------------------------------
# The SDK path is passed by build.ps1 via -DMLIR_DIR and -DLLVM_DIR.
# Using CONFIG mode ensures we pick up MLIRConfig.cmake / LLVMConfig.cmake
# from the extracted tarball, not from a system installation.

find_package(LLVM "23.1" REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)

message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} found at ${LLVM_DIR}")
message(STATUS "MLIR found at ${MLIR_DIR}")

# Pull in LLVM/MLIR CMake utilities: AddLLVM, AddMLIR, TableGen.
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)

# ---------------------------------------------------------------------------
# Compiler definitions required by LLVM headers
# ---------------------------------------------------------------------------
add_definitions(${LLVM_DEFINITIONS})

# ---------------------------------------------------------------------------
# MLIR / LLVM include directories (available to all targets below)
# ---------------------------------------------------------------------------
include_directories(SYSTEM
  ${LLVM_INCLUDE_DIRS}
  ${MLIR_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------------
# Generated Version header
# ---------------------------------------------------------------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/compiler/include/nirqoba/Version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/nirqoba/Version.h"
  @ONLY
)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/include")

# ---------------------------------------------------------------------------
# Sub-projects
# ---------------------------------------------------------------------------
enable_testing()

add_subdirectory(runtime)
add_subdirectory(compiler)
add_subdirectory(tests)
