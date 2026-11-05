#include "nirqoba/Backend/ObjectEmitter.h"
#include "nirqoba/Backend/TargetInfo.h"

#include <string>
#include <system_error>

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

namespace nirq {
using namespace std;

namespace {

llvm::CodeGenOptLevel codegen_level_for(OptimizationLevel level) {
  switch (level) {
    case OptimizationLevel::O0: return llvm::CodeGenOptLevel::None;
    case OptimizationLevel::O1: return llvm::CodeGenOptLevel::Less;
    case OptimizationLevel::O2:
    case OptimizationLevel::Os:
    case OptimizationLevel::Oz:
      return llvm::CodeGenOptLevel::Default;
    case OptimizationLevel::O3: return llvm::CodeGenOptLevel::Aggressive;
  }
  return llvm::CodeGenOptLevel::Default;
}

}  // namespace

unique_ptr<llvm::TargetMachine> create_target_machine(
    llvm::Module& llvm_module,
    const OptimizationOptions& options,
    const TargetConfiguration& target,
    DiagnosticEngine& diagnostics) {
  const llvm::Triple target_triple(target.triple);
  string error_message;
  const llvm::Target* llvm_target =
      llvm::TargetRegistry::lookupTarget(target_triple, error_message);
  if (llvm_target == nullptr) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "LLVM target lookup failed for '" + target.triple +
                          "': " + error_message);
    return nullptr;
  }

  llvm::TargetOptions target_options;
  unique_ptr<llvm::TargetMachine> target_machine(
      llvm_target->createTargetMachine(
          target_triple,
          target.cpu,
          target.features,
          target_options,
          // PIC keeps the object usable in both position-dependent and
          // position-independent executables.
          llvm::Reloc::PIC_,
          llvm::CodeModel::Small,
          codegen_level_for(options.level)));

  if (!target_machine) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "failed to create LLVM TargetMachine for '" +
                          target.triple + "' (CPU '" + target.cpu + "')");
    return nullptr;
  }

  // Optimization passes and code generation must agree on layout and triple.
  llvm_module.setDataLayout(target_machine->createDataLayout());
  llvm_module.setTargetTriple(target_machine->getTargetTriple());

  // Persist the resolved profile with the module so `--emit=llvm` output and
  // object files can be audited after the fact.
  llvm::LLVMContext& context = llvm_module.getContext();
  const auto add_flag = [&](llvm::StringRef key, const string& value) {
    llvm_module.addModuleFlag(llvm::Module::Error, key,
                              llvm::MDString::get(context, value));
  };
  add_flag("nrqoba.target.profile",
           string(target_profile_name(target.profile)));
  add_flag("nrqoba.target.triple", target.triple);
  add_flag("nrqoba.target.cpu", target.cpu);
  add_flag("nrqoba.target.features", target.features);
  return target_machine;
}

bool emit_object_file(llvm::Module& llvm_module,
                      const string& output_path,
                      llvm::TargetMachine& target_machine,
                      DiagnosticEngine& diagnostics) {
  error_code ec;
  llvm::raw_fd_ostream out(output_path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "cannot open object output file '" + output_path + "': " +
                          ec.message());
    return false;
  }

  llvm::legacy::PassManager code_gen_pm;
  if (target_machine.addPassesToEmitFile(
          code_gen_pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "LLVM target does not support object file emission for '" +
                          target_machine.getTargetTriple().str() + "'");
    return false;
  }

  code_gen_pm.run(llvm_module);
  out.flush();
  return true;
}

}  // namespace nirq
