# NirqobaOptions.cmake
# Compiler warning flags, install layout helpers, and project-wide build options.
#
# This file is included by the root CMakeLists.txt after project() so that all
# targets below it inherit the settings through target_compile_options / policies.
# It must not call find_package itself.

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# Developer-facing build options
# ---------------------------------------------------------------------------

option(NIRQOBA_WERROR
  "Treat every compiler warning as an error (recommended for CI, optional locally)"
  OFF)

option(NIRQOBA_ENABLE_ASSERTS
  "Enable Nirqoba's own ASSERT() macros independent of MLIR assertions (which are\
 absent from the release SDK). Enabled automatically in RelWithDebInfo."
  OFF)

# ---------------------------------------------------------------------------
# Warning flags — MSVC only in Phase 1 (Windows x64 is the primary target)
# ---------------------------------------------------------------------------

# Collect flags into a list so they compose cleanly with target_compile_options.
set(NIRQOBA_MSVC_WARNING_FLAGS
  /W4             # All reasonable warnings.
  /permissive-    # Strict standards conformance; disables MSVC extensions.
  /EHsc           # Standard C++ exception semantics (needed to link MLIR).
  /wd4100         # Unreferenced formal parameter — MLIR headers trigger this.
  /wd4127         # Conditional expression is constant — generated code pattern.
  /wd4244         # Narrowing conversion — MLIR frequently converts size_t <-> int.
  /wd4267         # Same as 4244 but for size_t.
)

if(NIRQOBA_WERROR)
  list(APPEND NIRQOBA_MSVC_WARNING_FLAGS /WX)
endif()

# ---------------------------------------------------------------------------
# Install layout
# ---------------------------------------------------------------------------
# All installed binaries go to bin/, libraries to lib/, headers to include/.
# This mirrors the LLVM SDK layout so scripts can find tools by convention.

set(CMAKE_INSTALL_BINDIR   "bin"     CACHE STRING "")
set(CMAKE_INSTALL_LIBDIR   "lib"     CACHE STRING "")
set(CMAKE_INSTALL_INCLUDEDIR "include" CACHE STRING "")

# ---------------------------------------------------------------------------
# Helper: apply the standard Nirqoba warning set to a target
# ---------------------------------------------------------------------------

function(nirqoba_set_target_warnings target)
  target_compile_options(${target} PRIVATE ${NIRQOBA_MSVC_WARNING_FLAGS})
endfunction()

# ---------------------------------------------------------------------------
# Helper: mark a target as a Nirqoba library (warning flags + C++20)
# ---------------------------------------------------------------------------

function(nirqoba_add_library target)
  cmake_parse_arguments(ARG "" "" "SOURCES;HEADERS;DEPENDS" ${ARGN})
  add_library(${target} STATIC ${ARG_SOURCES} ${ARG_HEADERS})
  set_property(TARGET ${target} PROPERTY CXX_STANDARD 20)
  set_property(TARGET ${target} PROPERTY CXX_STANDARD_REQUIRED ON)
  nirqoba_set_target_warnings(${target})
  if(ARG_DEPENDS)
    target_link_libraries(${target} PUBLIC ${ARG_DEPENDS})
  endif()
endfunction()
