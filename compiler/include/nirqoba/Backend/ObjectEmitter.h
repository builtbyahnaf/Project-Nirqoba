// Backend/ObjectEmitter.h — emits a COFF object file from an LLVM module.
//
// The target machine is created separately from code generation so the driver
// can run target-aware LLVM optimization passes and inspect the optimized IR
// before object emission.
#pragma once

#include <memory>
#include <string>

#include "llvm/IR/Module.h"
#include "nirqoba/Backend/Optimization.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"

namespace llvm {
class TargetMachine;
}  // namespace llvm

namespace nirq {

// Creates the target machine described by `options` and configures
// `llvm_module` with the matching target triple and data layout.
//
// Initializes the LLVM target registry on first use. Returns nullptr and
// records an NRQ5xxx diagnostic when the triple or CPU model cannot be
// resolved.
std::unique_ptr<llvm::TargetMachine> create_target_machine(
    llvm::Module& llvm_module,
    const OptimizationOptions& options,
    DiagnosticEngine& diagnostics);

// Writes the object file for `llvm_module` to `output_path` using the code
// generation options embedded in `target_machine`.
//
// Preconditions:
//   - `target_machine` was created by create_target_machine() for this module.
//   - `output_path` is a writable filesystem path.
bool emit_object_file(llvm::Module& llvm_module,
                      const std::string& output_path,
                      llvm::TargetMachine& target_machine,
                      DiagnosticEngine& diagnostics);

}  // namespace nirq
