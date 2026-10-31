#include "nirqoba/Backend/ObjectEmitter.h"
#include "nirqoba/Backend/TargetInfo.h"
#include "nirqoba/Version.h"  // NIRQOBA_DEFAULT_TARGET_TRIPLE

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

// Ensure the X86 target backend is linked in. LLVM uses a registration
// pattern: including this header runs the static initialiser.
#include "llvm/Support/TargetSelect.h"

namespace nirq {

std::string default_target_triple() {
  // Phase 1: always x86_64-pc-windows-msvc on the primary platform.
  // NIRQOBA_DEFAULT_TARGET_TRIPLE is set in cmake/NirqobaLLVMTargets.cmake.
  return NIRQOBA_DEFAULT_TARGET_TRIPLE;
}

bool emit_object_file(llvm::Module& llvm_module,
                      const std::string& output_path,
                      DiagnosticEngine& diagnostics) {
  // --- One-time LLVM target initialisation.
  // These calls are idempotent (they are guarded internally by llvm::once_flag)
  // and must happen before LookupTarget.
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  // --- Resolve the target triple.
  const std::string triple = default_target_triple();
  const llvm::Triple target_triple(triple);
  std::string error_message;
  const llvm::Target* target = llvm::TargetRegistry::lookupTarget(target_triple, error_message);
  if (target == nullptr) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "LLVM target lookup failed for '" + triple + "': " + error_message);
    return false;
  }

  // --- Build the TargetMachine.
  // CodeGenOptLevel::Default corresponds to -O2. Phase 1 uses it because the
  // SDK has no debug-config MLIR and the plan explicitly chooses Default.
  // Reloc::PIC_ is used so the emitted object works whether linked into a
  // position-dependent or position-independent executable.
  llvm::TargetOptions target_options;
  std::unique_ptr<llvm::TargetMachine> target_machine(
      target->createTargetMachine(
          target_triple,
          /*CPU=*/"generic",
          /*Features=*/"",
          target_options,
          llvm::Reloc::PIC_,
          llvm::CodeModel::Small,
          llvm::CodeGenOptLevel::Default));

  if (!target_machine) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "failed to create LLVM TargetMachine for '" + triple + "'");
    return false;
  }

  // Set the data layout on the module so it agrees with the TargetMachine.
  llvm_module.setDataLayout(target_machine->createDataLayout());
  llvm_module.setTargetTriple(target_triple);

  // --- Open the output file.
  std::error_code ec;
  llvm::raw_fd_ostream out(output_path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "cannot open object output file '" + output_path + "': " + ec.message());
    return false;
  }

  // --- Emit the COFF object.
  llvm::legacy::PassManager code_gen_pm;
  if (target_machine->addPassesToEmitFile(
          code_gen_pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "LLVM target does not support object file emission for '" + triple + "'");
    return false;
  }

  code_gen_pm.run(llvm_module);
  out.flush();
  return true;
}

}  // namespace nirq
