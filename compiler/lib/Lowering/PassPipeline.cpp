#include "nirqoba/Lowering/PassPipeline.h"
#include "nirqoba/Lowering/RegisterDialects.h"

#include <string>
#include <unordered_set>

#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinDialect.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "mlir/Pass/Pass.h"  // completes mlir::Pass for the unique_ptr created below
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Transforms/Passes.h"

#include "nirqoba/Lowering/NrqDialect.h"

// The NrqToLLVM pass is declared here; its definition is in NrqToLLVMPass.cpp.
namespace nirq {
using namespace std;

unique_ptr<mlir::Pass> createNrqToLLVMPass();
}

namespace nirq {

void register_all_dialects(mlir::DialectRegistry& registry) {
  registry.insert<
    mlir::BuiltinDialect,
    mlir::func::FuncDialect,
    mlir::LLVM::LLVMDialect,
    nirq::nrq::NrqDialect
  >();
  mlir::registerBuiltinDialectTranslation(registry);
  mlir::registerLLVMDialectTranslation(registry);
}

void register_dialects(mlir::DialectRegistry& registry) {
  register_all_dialects(registry);
}

namespace {

// MLIR's canonicalizer has no folders for LLVM-dialect integer arithmetic, so
// constants introduced by expression lowering survive until LLVM. Folding them
// here exposes simpler SSA values to SCCP and reduces the IR that reaches the
// backend. The pass is deliberately limited to integer operations with no
// exceptional behavior: division and remainder are skipped because a division
// by zero is not foldable.
class FoldLlvmIntegerConstantsPass
    : public mlir::PassWrapper<FoldLlvmIntegerConstantsPass,
                               mlir::OperationPass<>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(FoldLlvmIntegerConstantsPass)

  llvm::StringRef getArgument() const final {
    return "fold-llvm-integer-constants";
  }
  llvm::StringRef getDescription() const final {
    return "Fold LLVM integer arithmetic and comparisons with constant operands";
  }

  void runOnOperation() override {
    getOperation()->walk([&](mlir::Operation* operation) {
      if (operation->getNumResults() != 1) return;
      auto result_type =
          mlir::dyn_cast<mlir::IntegerType>(operation->getResult(0).getType());
      if (!result_type) return;

      if (auto compare = mlir::dyn_cast<mlir::LLVM::ICmpOp>(operation)) {
        llvm::APInt lhs;
        llvm::APInt rhs;
        if (!constant_integer(compare.getLhs(), lhs) ||
            !constant_integer(compare.getRhs(), rhs)) return;
        bool result = false;
        switch (compare.getPredicate()) {
          case mlir::LLVM::ICmpPredicate::eq: result = lhs == rhs; break;
          case mlir::LLVM::ICmpPredicate::ne: result = lhs != rhs; break;
          case mlir::LLVM::ICmpPredicate::slt: result = lhs.slt(rhs); break;
          case mlir::LLVM::ICmpPredicate::sle: result = lhs.sle(rhs); break;
          case mlir::LLVM::ICmpPredicate::sgt: result = lhs.sgt(rhs); break;
          case mlir::LLVM::ICmpPredicate::sge: result = lhs.sge(rhs); break;
          case mlir::LLVM::ICmpPredicate::ult: result = lhs.ult(rhs); break;
          case mlir::LLVM::ICmpPredicate::ule: result = lhs.ule(rhs); break;
          case mlir::LLVM::ICmpPredicate::ugt: result = lhs.ugt(rhs); break;
          case mlir::LLVM::ICmpPredicate::uge: result = lhs.uge(rhs); break;
        }
        replace_with_constant(operation, result_type,
                              llvm::APInt(1, result ? 1 : 0));
        return;
      }

      if (operation->getNumOperands() != 2) return;
      llvm::APInt lhs;
      llvm::APInt rhs;
      if (!constant_integer(operation->getOperand(0), lhs) ||
          !constant_integer(operation->getOperand(1), rhs)) return;

      llvm::StringRef name = operation->getName().getStringRef();
      llvm::APInt result;
      if (name == "llvm.add") {
        result = lhs + rhs;
      } else if (name == "llvm.sub") {
        result = lhs - rhs;
      } else if (name == "llvm.mul") {
        result = lhs * rhs;
      } else if (name == "llvm.and") {
        result = lhs & rhs;
      } else if (name == "llvm.or") {
        result = lhs | rhs;
      } else if (name == "llvm.xor") {
        result = lhs ^ rhs;
      } else if (name == "llvm.shl") {
        if (rhs.uge(lhs.getBitWidth())) return;
        result = lhs.shl(rhs);
      } else if (name == "llvm.lshr") {
        if (rhs.uge(lhs.getBitWidth())) return;
        result = lhs.lshr(rhs);
      } else if (name == "llvm.ashr") {
        if (rhs.uge(lhs.getBitWidth())) return;
        result = lhs.ashr(rhs);
      } else {
        return;
      }
      replace_with_constant(operation, result_type, result);
    });
  }

 private:
  static bool constant_integer(mlir::Value value, llvm::APInt& result) {
    auto constant = value.getDefiningOp<mlir::LLVM::ConstantOp>();
    if (!constant) return false;
    auto integer = mlir::dyn_cast<mlir::IntegerAttr>(constant.getValue());
    if (!integer) return false;
    result = integer.getValue();
    return true;
  }

  static void replace_with_constant(mlir::Operation* operation,
                                    mlir::Type type,
                                    const llvm::APInt& value) {
    mlir::OpBuilder builder(operation);
    auto constant = builder.create<mlir::LLVM::ConstantOp>(
        operation->getLoc(), type,
        mlir::IntegerAttr::get(type, value));
    operation->getResult(0).replaceAllUsesWith(constant.getResult());
    operation->erase();
  }
};

// LLVM's `cond_br` does not currently canonicalize a constant condition into
// an unconditional branch. SCCP proves the condition constant, but the
// unreachable edge remains until the terminator itself is rewritten. Doing that
// here lets the following canonicalizer and trivial-DCE remove the now-dead
// block and its constants. The branch operands are carried over unchanged, so
// block-argument types and values are preserved.
class FoldConstantLlvmCondBrPass
    : public mlir::PassWrapper<FoldConstantLlvmCondBrPass,
                               mlir::OperationPass<>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(FoldConstantLlvmCondBrPass)

  llvm::StringRef getArgument() const final {
    return "fold-constant-llvm-condbr";
  }
  llvm::StringRef getDescription() const final {
    return "Fold llvm.cond_br with a constant i1 condition";
  }

  void runOnOperation() override {
    getOperation()->walk([&](mlir::LLVM::CondBrOp branch) {
      auto constant =
          branch.getCondition().getDefiningOp<mlir::LLVM::ConstantOp>();
      if (!constant || !constant.getType().isInteger(1)) return;
      auto value = mlir::dyn_cast<mlir::IntegerAttr>(constant.getValue());
      if (!value) return;

      mlir::OpBuilder builder(branch);
      const bool taken = value.getInt() != 0;
      auto unconditional = builder.create<mlir::LLVM::BrOp>(
          branch.getLoc(),
          taken ? branch.getTrueDestOperands()
                : branch.getFalseDestOperands(),
          taken ? branch.getTrueDest() : branch.getFalseDest());
      // Keep loop metadata attached to the surviving edge.
      unconditional.setLoopAnnotationAttr(branch.getLoopAnnotationAttr());
      branch.erase();
    });
  }
};

// ---------------------------------------------------------------------------
// Function-level memory effects
// ---------------------------------------------------------------------------
//
// A generated function's declaration told the optimizer nothing about the
// memory it touches, so every call to it acted as a full memory barrier: loads
// could not be hoisted across calls, identical calls could not be merged, and a
// call whose result is unused could not be removed even when the callee was
// pure arithmetic.
//
// The lowered body already contains the evidence needed to do better. Every
// memory access is classified by address provenance:
//
//   * a pointer derived from an llvm.alloca addresses inaccessible memory: the
//     stack frame the caller cannot name;
//   * a pointer derived from a function argument addresses argument memory,
//     including memory reachable through pointers loaded out of it;
//   * anything else (a pointer loaded from a stack slot, a global, an
//     allocation) is charged to both "other" and argument memory, because it
//     may be reachable from an argument.
//
// Calls inherit their callee's effects. Module functions are analysed to a
// greatest fixpoint starting from "no effects", which handles recursion: since
// effects can only be added as evidence accumulates, a self-recursive
// arithmetic function remains memory(none). Declarations use the effects the
// runtime attribute policy already attached; a declaration with unknown effects,
// a target-specific memory location or an errno access makes the call
// conservative instead.
//
// An attribute is attached only when it actually narrows the default, and any
// operation this pass does not model charges all three locations. A construct
// it does not understand can therefore never produce a false promise.


enum class AddressOrigin { Local, Argument, Unknown };

struct MemoryEffectSummary {
  bool reads_argument = false;
  bool writes_argument = false;
  bool reads_inaccessible = false;
  bool writes_inaccessible = false;
  bool reads_other = false;
  bool writes_other = false;

  bool operator==(const MemoryEffectSummary& other) const = default;

  void add(const MemoryEffectSummary& other_summary) {
    reads_argument |= other_summary.reads_argument;
    writes_argument |= other_summary.writes_argument;
    reads_inaccessible |= other_summary.reads_inaccessible;
    writes_inaccessible |= other_summary.writes_inaccessible;
    reads_other |= other_summary.reads_other;
    writes_other |= other_summary.writes_other;
  }

  bool touches_everything() const {
    return reads_argument && writes_argument && reads_inaccessible &&
           writes_inaccessible && reads_other && writes_other;
  }
};

void charge_access(MemoryEffectSummary& summary, AddressOrigin origin,
                   bool write) {
  switch (origin) {
    case AddressOrigin::Local:
      if (write) summary.writes_inaccessible = true;
      else summary.reads_inaccessible = true;
      return;
    case AddressOrigin::Argument:
      if (write) summary.writes_argument = true;
      else summary.reads_argument = true;
      return;
    case AddressOrigin::Unknown:
      // The address may be reachable from an argument and may also be outside
      // it, so neither location can be excluded.
      if (write) {
        summary.writes_argument = true;
        summary.writes_other = true;
      } else {
        summary.reads_argument = true;
        summary.reads_other = true;
      }
      return;
  }
}

void charge_unknown_effects(MemoryEffectSummary& summary) {
  summary.reads_argument = summary.writes_argument = true;
  summary.reads_inaccessible = summary.writes_inaccessible = true;
  summary.reads_other = summary.writes_other = true;
}

bool llvm_op_accesses_no_memory(llvm::StringRef name) {
  static const std::unordered_set<std::string> memory_free = {
      "llvm.mlir.constant", "llvm.mlir.zero", "llvm.mlir.undef",
      "llvm.mlir.poison", "llvm.mlir.null", "llvm.mlir.addressof",
      "llvm.alloca", "llvm.getelementptr",
      "llvm.add", "llvm.sub", "llvm.mul", "llvm.udiv", "llvm.sdiv",
      "llvm.urem", "llvm.srem", "llvm.and", "llvm.or", "llvm.xor",
      "llvm.shl", "llvm.lshr", "llvm.ashr", "llvm.fadd", "llvm.fsub",
      "llvm.fmul", "llvm.fdiv", "llvm.frem", "llvm.fneg",
      "llvm.icmp", "llvm.fcmp", "llvm.select", "llvm.freeze",
      "llvm.trunc", "llvm.zext", "llvm.sext", "llvm.fptrunc", "llvm.fpext",
      "llvm.fptosi", "llvm.fptoui", "llvm.sitofp", "llvm.uitofp",
      "llvm.bitcast", "llvm.addrspacecast", "llvm.ptrtoint", "llvm.inttoptr",
      "llvm.extractvalue", "llvm.insertvalue", "llvm.mlir.poison",
      "llvm.br", "llvm.cond_br", "llvm.switch", "llvm.return",
      "llvm.unreachable", "llvm.lifetime.start", "llvm.lifetime.end",
  };
  return memory_free.count(name.str()) != 0;
}

// Reads the memory effects already attached to a declaration. Returns false
// when the declaration touches errno or a target-specific location, because the
// caller's attribute cannot describe those through other/argmem/inaccessible.
bool effects_from_attribute(mlir::LLVM::MemoryEffectsAttr attribute,
                            MemoryEffectSummary& summary) {
  using mlir::LLVM::ModRefInfo;
  if (!attribute) return false;
  if (attribute.getErrnoMem() != ModRefInfo::NoModRef ||
      attribute.getTargetMem0() != ModRefInfo::NoModRef ||
      attribute.getTargetMem1() != ModRefInfo::NoModRef)
    return false;
  auto reads = [](ModRefInfo info) {
    return info == ModRefInfo::Ref || info == ModRefInfo::ModRef;
  };
  auto writes = [](ModRefInfo info) {
    return info == ModRefInfo::Mod || info == ModRefInfo::ModRef;
  };
  summary.reads_other = reads(attribute.getOther());
  summary.writes_other = writes(attribute.getOther());
  summary.reads_argument = reads(attribute.getArgMem());
  summary.writes_argument = writes(attribute.getArgMem());
  summary.reads_inaccessible = reads(attribute.getInaccessibleMem());
  summary.writes_inaccessible = writes(attribute.getInaccessibleMem());
  return true;
}

MemoryEffectSummary analyze_body(
    mlir::LLVM::LLVMFuncOp function,
    const llvm::StringMap<MemoryEffectSummary>& known_callees,
    const llvm::StringSet<>& callees_with_known_effects) {
  MemoryEffectSummary summary;
  if (function.isExternal()) return summary;

  llvm::DenseMap<mlir::Value, AddressOrigin> origins;
  // Every parameter is argument memory, including aggregates: a pointer
  // extracted from a by-value array view still points into the caller's memory.
  for (mlir::BlockArgument argument : function.getBody().front().getArguments())
    origins[argument] = AddressOrigin::Argument;

  auto origin_of = [&](mlir::Value value) {
    const auto found = origins.find(value);
    return found == origins.end() ? AddressOrigin::Unknown : found->second;
  };
  auto record_pointer = [&](mlir::Value value, AddressOrigin origin) {
    if (mlir::isa<mlir::LLVM::LLVMPointerType>(value.getType()))
      origins[value] = origin;
  };

  for (mlir::Block& block : function.getBody()) {
    for (mlir::Operation& operation : block) {
      if (auto load = mlir::dyn_cast<mlir::LLVM::LoadOp>(operation)) {
        const AddressOrigin origin = origin_of(load.getAddr());
        charge_access(summary, origin, /*write=*/false);
        // A pointer read out of argument memory still addresses memory
        // reachable from that argument. A pointer read out of stack memory
        // could address anything, so it stays unknown.
        record_pointer(load.getResult(),
                       origin == AddressOrigin::Argument ? AddressOrigin::Argument
                                                         : AddressOrigin::Unknown);
        continue;
      }
      if (auto store = mlir::dyn_cast<mlir::LLVM::StoreOp>(operation)) {
        charge_access(summary, origin_of(store.getAddr()), /*write=*/true);
        continue;
      }
      if (auto alloca = mlir::dyn_cast<mlir::LLVM::AllocaOp>(operation)) {
        record_pointer(alloca.getResult(), AddressOrigin::Local);
        continue;
      }
      if (auto gep = mlir::dyn_cast<mlir::LLVM::GEPOp>(operation)) {
        record_pointer(gep.getResult(), origin_of(gep.getBase()));
        continue;
      }
      if (auto call = mlir::dyn_cast<mlir::LLVM::CallOp>(operation)) {
        const mlir::FlatSymbolRefAttr callee = call.getCalleeAttr();
        const std::string name =
            callee ? callee.getValue().str() : std::string();
        if (!name.empty() && callees_with_known_effects.count(name) != 0) {
          summary.add(known_callees.lookup(name));
        } else {
          charge_unknown_effects(summary);
        }
        for (mlir::Value result : call.getResults())
          record_pointer(result, AddressOrigin::Unknown);
        continue;
      }
      if (llvm_op_accesses_no_memory(operation.getName().getStringRef())) {
        // Propagate provenance through pointer casts so a cast of an argument
        // is still argument memory.
        if (operation.getNumOperands() == 1 && operation.getNumResults() == 1)
          record_pointer(operation.getResult(0),
                         origin_of(operation.getOperand(0)));
        continue;
      }
      charge_unknown_effects(summary);
    }
  }
  return summary;
}

mlir::LLVM::ModRefInfo mod_ref_info(bool reads, bool writes) {
  using mlir::LLVM::ModRefInfo;
  if (reads && writes) return ModRefInfo::ModRef;
  if (reads) return ModRefInfo::Ref;
  if (writes) return ModRefInfo::Mod;
  return ModRefInfo::NoModRef;
}

void attach_memory_effects(mlir::LLVM::LLVMFuncOp function,
                           const MemoryEffectSummary& summary) {
  if (function.isExternal()) return;
  using mlir::LLVM::ModRefInfo;
  const ModRefInfo other = mod_ref_info(summary.reads_other, summary.writes_other);
  const ModRefInfo argument =
      mod_ref_info(summary.reads_argument, summary.writes_argument);
  const ModRefInfo inaccessible =
      mod_ref_info(summary.reads_inaccessible, summary.writes_inaccessible);
  if (other == ModRefInfo::ModRef && argument == ModRefInfo::ModRef &&
      inaccessible == ModRefInfo::ModRef)
    return;
  auto attribute = mlir::LLVM::MemoryEffectsAttr::get(
      function.getContext(), other, argument, inaccessible,
      ModRefInfo::NoModRef, ModRefInfo::NoModRef, ModRefInfo::NoModRef);
  function.setMemoryEffectsAttr(attribute);
}

class InferFunctionMemoryEffectsPass
    : public mlir::PassWrapper<InferFunctionMemoryEffectsPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(InferFunctionMemoryEffectsPass)

  llvm::StringRef getArgument() const final {
    return "infer-function-memory-effects";
  }
  llvm::StringRef getDescription() const final {
    return "Attach memory effects inferred from each generated function body";
  }

  void runOnOperation() override {
    mlir::ModuleOp module = getOperation();
    llvm::StringMap<MemoryEffectSummary> known;
    llvm::StringSet<> has_known_effects;
    llvm::SmallVector<mlir::LLVM::LLVMFuncOp> definitions;

    for (mlir::LLVM::LLVMFuncOp function :
         module.getOps<mlir::LLVM::LLVMFuncOp>()) {
      MemoryEffectSummary summary;
      if (function.isExternal()) {
        if (!effects_from_attribute(function.getMemoryEffectsAttr(), summary))
          continue;
      } else {
        definitions.push_back(function);
      }
      const std::string name = function.getSymName().str();
      known[name] = summary;
      has_known_effects.insert(name);
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (mlir::LLVM::LLVMFuncOp function : definitions) {
        const MemoryEffectSummary analyzed =
            analyze_body(function, known, has_known_effects);
        MemoryEffectSummary& current = known[function.getSymName()];
        if (analyzed == current) continue;
        current = analyzed;
        changed = true;
      }
    }

    for (mlir::LLVM::LLVMFuncOp function : definitions)
      attach_memory_effects(function, known[function.getSymName()]);
  }
};


// Adds the semantics-preserving MLIR cleanup shared by the pre- and
// post-lowering stages.
//
// The canonicalizer folds constants, removes trivially dead operations whose
// results are unused, and simplifies control flow. SCCP then propagates known
// constants through SSA values, block arguments, and conditional control flow,
// proving edges and blocks unreachable. A second canonicalization folds the
// operations exposed by SCCP before CSE merges structurally identical pure
// operations. Trivial DCE runs last and drops the operations and blocks made
// dead by those simplifications.
//
// Every cleanup pass relies on MLIR's side-effect interfaces, so calls, runtime
// operations, GC-root operations, ownership transfers, and cleanup calls are
// retained: an operation without a proven memory-effect model is never treated
// as removable or deduplicable.
//
// Order matters: folding and propagation simplify the CFG before CSE looks for
// equal expressions, and the final canonicalization plus trivial DCE removes
// the now-dead operands and blocks without leaving a second cleanup round.
void add_cleanup_passes(mlir::PassManager& pm) {
  pm.addPass(mlir::createCanonicalizerPass());   // structural folding
  pm.addPass(std::make_unique<FoldLlvmIntegerConstantsPass>());
  pm.addPass(mlir::createSCCPPass());            // propagation + reachability
  pm.addPass(std::make_unique<FoldLlvmIntegerConstantsPass>());
  pm.addPass(std::make_unique<FoldConstantLlvmCondBrPass>());
  pm.addPass(mlir::createCanonicalizerPass());   // fold SCCP results
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createTrivialDeadCodeEliminationPass());
}

}  // namespace

void build_lowering_pipeline(mlir::PassManager& pm, bool optimize) {
  // MLIR-level cleanup catches duplicated constants, unreachable blocks, and
  // dead alias-forming operations before they reach LLVM. At -O0 the direct
  // lowering is preserved for inspection and debugging.
  if (optimize) {
    add_cleanup_passes(pm);
  }

  // Step 1: Lower nrq dialect ops to LLVM dialect calls.
  pm.addPass(createNrqToLLVMPass());

  // Step 2: Lower func.func to llvm.func (standard MLIR conversion).
  pm.addPass(mlir::createConvertFuncToLLVMPass());

  // Step 3: Resolve any remaining unrealized cast ops left by the two
  // conversions above; this is required before LLVM IR export.
  pm.addPass(mlir::createReconcileUnrealizedCastsPass());

  if (optimize) {
    // Canonicalization and CSE are legal on LLVM-dialect IR too. Running the
    // same cleanup again removes duplicate constants and casts introduced by
    // the conversion passes while leaving every call and runtime operation
    // intact.
    add_cleanup_passes(pm);
  }

  // Step 4: Describe the memory each generated function actually touches. This
  // runs last so it sees the final body. It runs at every optimization level:
  // the attribute is a property of the program, not of a pass run.
  pm.addPass(std::make_unique<InferFunctionMemoryEffectsPass>());
}

}  // namespace nirq
