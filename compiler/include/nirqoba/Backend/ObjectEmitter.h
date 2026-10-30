// Backend/ObjectEmitter.h — emits a COFF object file from an LLVM module.
//
// Phase 1 target: x86_64-pc-windows-msvc, CodeGenOpt::Default, Reloc::PIC_.
//
// The triple and data layout are taken from LLVMDialect rather than hard-coded
// here so the MLIR module and the TargetMachine always agree.
#pragma once

#include <string>

#include "llvm/IR/Module.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"

namespace nirq {

// Writes the object file for `llvm_module` to `output_path`.
// Returns true on success; records NRQ5xxx diagnostics and returns false on
// any TargetMachine or code-generation failure.
//
// Preconditions:
//   - `llvm_module` was produced by mlir::translateModuleToLLVMIR.
//   - `output_path` is a writable filesystem path.
bool emit_object_file(llvm::Module& llvm_module,
                      const std::string& output_path,
                      DiagnosticEngine& diagnostics);

}  // namespace nirq
