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
#include "nirqoba/Backend/TargetInfo.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"

namespace llvm {
class TargetMachine;
}  // namespace llvm

namespace nirq {

// Creates the target machine for an already-resolved target configuration and
// configures `llvm_module` with the matching target triple, data layout, and
// `nrqoba.target.*` module flags.
//
// Call resolve_target_configuration() first: this function trusts `target`
// and does not re-check the profile/option combinations. Returns nullptr and
// records an NRQ5xxx diagnostic when the triple cannot be resolved.
std::unique_ptr<llvm::TargetMachine> create_target_machine(
    llvm::Module& llvm_module,
    const OptimizationOptions& options,
    const TargetConfiguration& target,
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
