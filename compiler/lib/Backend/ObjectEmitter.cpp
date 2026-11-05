#include "nirqoba/Backend/ObjectEmitter.h"
#include "nirqoba/Backend/TargetInfo.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "llvm/ADT/StringMap.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

// Ensure the X86 target backend is linked in. LLVM uses a registration
// pattern: including this header runs the static initialiser.
#include "llvm/Support/TargetSelect.h"
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

// Converts the host feature map into the comma-separated form accepted by
// LLVM's TargetMachine. Sorting keeps diagnostic output and object files
// reproducible for the same host.
string host_feature_string() {
  llvm::StringMap<bool> features = llvm::sys::getHostCPUFeatures();
  vector<pair<string, bool>> ordered;
  ordered.reserve(features.size());
  for (const auto& feature : features) {
    ordered.emplace_back(feature.first().str(), feature.second);
  }
  sort(ordered.begin(), ordered.end());

  string result;
  for (const auto& feature : ordered) {
    if (!result.empty()) result.push_back(',');
    result.push_back(feature.second ? '+' : '-');
    result += feature.first;
  }
  return result;
}

}  // namespace

unique_ptr<llvm::TargetMachine> create_target_machine(
    llvm::Module& llvm_module,
    const OptimizationOptions& options,
    DiagnosticEngine& diagnostics) {
  // Target registration is idempotent and must happen before LookupTarget.
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  const string triple = options.target_triple.empty()
      ? default_target_triple()
      : options.target_triple;
  const llvm::Triple target_triple(triple);
  string error_message;
  const llvm::Target* target =
      llvm::TargetRegistry::lookupTarget(target_triple, error_message);
  if (target == nullptr) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "LLVM target lookup failed for '" + triple + "': " +
                          error_message);
    return nullptr;
  }

  string cpu = options.target_cpu.empty() ? "generic" : options.target_cpu;
  string features = options.target_features;
  if (options.target_profile == TargetProfile::Native) {
    cpu = llvm::sys::getHostCPUName().str();
    features = host_feature_string();
  }

  llvm::TargetOptions target_options;
  unique_ptr<llvm::TargetMachine> target_machine(
      target->createTargetMachine(
          target_triple,
          cpu,
          features,
          target_options,
          // PIC keeps the object usable in both position-dependent and
          // position-independent executables.
          llvm::Reloc::PIC_,
          llvm::CodeModel::Small,
          codegen_level_for(options.level)));

  if (!target_machine) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "failed to create LLVM TargetMachine for '" + triple +
                          "' (CPU '" + cpu + "')");
    return nullptr;
  }

  // Optimization passes and code generation must agree on layout and triple.
  llvm_module.setDataLayout(target_machine->createDataLayout());
  llvm_module.setTargetTriple(target_machine->getTargetTriple());
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
