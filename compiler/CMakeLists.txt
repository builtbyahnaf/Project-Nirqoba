# compiler/CMakeLists.txt
# Builds the Nirqoba compiler library and the `nirqoba` driver executable.
#
# Layout:
#   nirqoba_compiler_lib  — all compiler stages as a static library
#   nirqoba               — thin main() wrapper around the CLI driver

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# TableGen: generate C++ from the nrq dialect definition
# ---------------------------------------------------------------------------
# mlir_tablegen produces the .h.inc and .cpp.inc files the dialect headers
# include. The output lives in the build tree; the include path below exposes
# it to everything that includes NrqDialect.h or NrqOps.h.

set(TABLEGEN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/include/nirqoba/Lowering")

mlir_tablegen(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/nirqoba/Lowering/NrqOps.td"
  -gen-dialect-decls   -o "${TABLEGEN_OUTPUT_DIR}/NrqDialect.h.inc"
)
mlir_tablegen(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/nirqoba/Lowering/NrqOps.td"
  -gen-dialect-defs    -o "${TABLEGEN_OUTPUT_DIR}/NrqDialect.cpp.inc"
)
mlir_tablegen(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/nirqoba/Lowering/NrqOps.td"
  -gen-op-decls        -o "${TABLEGEN_OUTPUT_DIR}/NrqOps.h.inc"
)
mlir_tablegen(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/nirqoba/Lowering/NrqOps.td"
  -gen-op-defs         -o "${TABLEGEN_OUTPUT_DIR}/NrqOps.cpp.inc"
)

# Create a custom target so the compiler library can depend on generation.
add_custom_target(NrqDialectTableGen DEPENDS
  "${TABLEGEN_OUTPUT_DIR}/NrqDialect.h.inc"
  "${TABLEGEN_OUTPUT_DIR}/NrqDialect.cpp.inc"
  "${TABLEGEN_OUTPUT_DIR}/NrqOps.h.inc"
  "${TABLEGEN_OUTPUT_DIR}/NrqOps.cpp.inc"
)

# ---------------------------------------------------------------------------
# Compiler library
# ---------------------------------------------------------------------------

# LLVM components the compiler needs for Phase 1.
nirqoba_llvm_components(NIRQOBA_LLVM_LIBS
  core support target x86codegen x86asmparser x86desc x86info
)

# MLIR libraries needed for the nrq dialect, lowering, and LLVM IR export.
set(NIRQOBA_MLIR_LIBS
  MLIRAnalysis
  MLIRBuiltinToLLVMIRTranslation
  MLIRDialect
  MLIRFuncDialect
  MLIRFuncToLLVM
  MLIRIR
  MLIRLLVMCommonConversion
  MLIRLLVMDialect
  MLIRLLVMToLLVMIRTranslation
  MLIRParser
  MLIRPass
  MLIRReconcileUnrealizedCasts
  MLIRSupport
  MLIRTargetLLVMIRExport
  MLIRTransforms
)

add_library(nirqoba_compiler_lib STATIC
  # Common
  lib/Common/Type.cpp

  # Diagnostics
  lib/Diagnostics/DiagnosticEngine.cpp

  # Frontend
  lib/Frontend/SourceFile.cpp
  lib/Frontend/Token.cpp
  lib/Frontend/Lexer.cpp
  lib/Frontend/AstPrinter.cpp
  lib/Frontend/Parser.cpp

  # Sema
  lib/Sema/SymbolTable.cpp
  lib/Sema/TypeChecker.cpp

  # TIR
  lib/TIR/IR.cpp
  lib/TIR/Builder.cpp
  lib/TIR/Printing.cpp
  lib/TIR/Verifier.cpp

  # Lowering (nrq dialect + TIR→MLIR + passes)
  lib/Lowering/NrqDialect.cpp
  lib/Lowering/NrqToLLVMPass.cpp
  lib/Lowering/PassPipeline.cpp

  # Backend
  lib/Backend/ObjectEmitter.cpp

  # Driver
  lib/Driver/LinkerEnvironment.cpp
  lib/Driver/ProcessRunner.cpp
  lib/Driver/CompilationPipeline.cpp

  # CLI
  lib/CLI/Driver.cpp
)

add_dependencies(nirqoba_compiler_lib NrqDialectTableGen)

target_include_directories(nirqoba_compiler_lib
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/include"   # generated Version.h and TableGen .inc
)

target_link_libraries(nirqoba_compiler_lib
  PUBLIC
    ${NIRQOBA_MLIR_LIBS}
    ${NIRQOBA_LLVM_LIBS}
)

set_property(TARGET nirqoba_compiler_lib PROPERTY CXX_STANDARD 20)
set_property(TARGET nirqoba_compiler_lib PROPERTY CXX_STANDARD_REQUIRED ON)
nirqoba_set_target_warnings(nirqoba_compiler_lib)

# ---------------------------------------------------------------------------
# Driver executable
# ---------------------------------------------------------------------------

add_executable(nirqoba tools/nirqoba/main.cpp)

set_property(TARGET nirqoba PROPERTY CXX_STANDARD 20)
set_property(TARGET nirqoba PROPERTY CXX_STANDARD_REQUIRED ON)
nirqoba_set_target_warnings(nirqoba)

target_link_libraries(nirqoba
  PRIVATE
    nirqoba_compiler_lib
    nrqoba_runtime
)

# Place the binary in build/<preset>/bin/ so scripts can find it predictably.
set_target_properties(nirqoba PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
