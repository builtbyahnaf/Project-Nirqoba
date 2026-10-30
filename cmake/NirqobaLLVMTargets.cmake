# NirqobaLLVMTargets.cmake
# Convenience wrappers around LLVM/MLIR CMake helpers.
#
# Included after find_package(LLVM) and find_package(MLIR) have succeeded.

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# llvm_map_components helper
# ---------------------------------------------------------------------------
# Translates a list of LLVM component names (e.g. "core support x86") into
# the list of imported targets or library names the caller should link.
# This wrapper exists so the rest of the build never calls the LLVM internal
# function directly; if LLVM changes the API the fix lives in one place.

function(nirqoba_llvm_components result_var)
  # ARGN is the list of requested components.
  llvm_map_components_to_libnames(_libs ${ARGN})
  set(${result_var} ${_libs} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Target triple for Phase 1 (Windows x64 native)
# ---------------------------------------------------------------------------
# Used by ObjectEmitter and the version string. Kept here so cmake/ is the
# single authority for platform decisions rather than scattering the string
# across source files.

if(NOT DEFINED NIRQOBA_DEFAULT_TARGET_TRIPLE)
  set(NIRQOBA_DEFAULT_TARGET_TRIPLE "x86_64-pc-windows-msvc"
      CACHE STRING "Default LLVM target triple for the Nirqoba compiler")
endif()
