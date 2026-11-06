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
#include "llvm/ADT/DenseSet.h"
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

// Walks one function body and charges every memory access it performs.
//
// `slot_value_origins` maps a stack slot (an llvm.alloca result) to the origin
// of the pointer values stored into it. The toolchain spills every parameter
// into a stack slot, so without this the pointer loaded back out of the slot
// would have to be treated as unknown and the function would lose all
// precision. When `collect_slots` is set the walk only records those origins;
// the charging pass then runs a second time with them available.
void scan_body(mlir::LLVM::LLVMFuncOp function,
               const llvm::StringMap<MemoryEffectSummary>& known_callees,
               const llvm::StringSet<>& callees_with_known_effects,
               llvm::DenseMap<mlir::Value, AddressOrigin>& slot_value_origins,
               bool collect_slots, MemoryEffectSummary& summary) {
  if (function.isExternal()) return;

  llvm::DenseMap<mlir::Value, AddressOrigin> origins;
  llvm::DenseMap<mlir::Value, mlir::Value> root_slot;
  // Every parameter is argument memory, including aggregates: a pointer
  // extracted from a by-value array view still points into the caller's memory.
  for (mlir::BlockArgument argument : function.getBody().front().getArguments())
    origins[argument] = AddressOrigin::Argument;

  auto origin_of = [&](mlir::Value value) {
    const auto found = origins.find(value);
    return found == origins.end() ? AddressOrigin::Unknown : found->second;
  };
  // The origin of the memory a pointer addresses.
  auto address_origin = [&](mlir::Value address) {
    const AddressOrigin origin = origin_of(address);
    if (origin != AddressOrigin::Local) return origin;
    const auto slot = root_slot.find(address);
    if (slot == root_slot.end()) return AddressOrigin::Unknown;
    const auto stored = slot_value_origins.find(slot->second);
    return stored == slot_value_origins.end() ? AddressOrigin::Unknown
                                              : stored->second;
  };
  auto record_value = [&](mlir::Value value, AddressOrigin origin) {
    origins[value] = origin;
  };
  auto record_derived_pointer = [&](mlir::Value value, mlir::Value address) {
    record_value(value, address_origin(address));
  };

  for (mlir::Block& block : function.getBody()) {
    for (mlir::Operation& operation : block) {
      if (auto load = mlir::dyn_cast<mlir::LLVM::LoadOp>(operation)) {
        const AddressOrigin origin = origin_of(load.getAddr());
        charge_access(summary, origin, /*write=*/false);
        // A pointer read out of argument memory still addresses memory
        // reachable from that argument; a pointer read out of a stack slot
        // addresses whatever was stored into it.
        record_derived_pointer(load.getResult(), load.getAddr());
        continue;
      }
      if (auto store = mlir::dyn_cast<mlir::LLVM::StoreOp>(operation)) {
        const ::mlir::Value address = store.getAddr();
        charge_access(summary, origin_of(address), /*write=*/true);
        if (!collect_slots) continue;
        const auto slot = root_slot.find(address);
        if (slot == root_slot.end()) continue;
        const AddressOrigin stored = address_origin(store.getValue());
        const auto existing = slot_value_origins.find(slot->second);
        if (existing == slot_value_origins.end())
          slot_value_origins[slot->second] = stored;
        else if (existing->second != stored)
          // Two different origins reachable through the same slot: keep the
          // conservative one. Argument and Local both widen to Unknown, which
          // charges the access to every location.
          existing->second = AddressOrigin::Unknown;
        continue;
      }
      if (auto alloca = mlir::dyn_cast<mlir::LLVM::AllocaOp>(operation)) {
        record_value(alloca.getResult(), AddressOrigin::Local);
        root_slot[alloca.getResult()] = alloca.getResult();
        continue;
      }
      if (auto gep = mlir::dyn_cast<mlir::LLVM::GEPOp>(operation)) {
        record_value(gep.getResult(), origin_of(gep.getBase()));
        const auto slot = root_slot.find(gep.getBase());
        if (slot != root_slot.end()) root_slot[gep.getResult()] = slot->second;
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
          record_value(result, AddressOrigin::Unknown);
        continue;
      }
      if (llvm_op_accesses_no_memory(operation.getName().getStringRef())) {
        // Propagate provenance through pointer casts so a cast of an argument
        // is still argument memory.
        if (operation.getNumOperands() == 1 && operation.getNumResults() == 1)
          record_value(operation.getResult(0),
                       origin_of(operation.getOperand(0)));
        continue;
      }
      charge_unknown_effects(summary);
    }
  }
}

MemoryEffectSummary analyze_body(
    mlir::LLVM::LLVMFuncOp function,
    const llvm::StringMap<MemoryEffectSummary>& known_callees,
    const llvm::StringSet<>& callees_with_known_effects) {
  if (function.isExternal()) return MemoryEffectSummary{};

  llvm::DenseMap<mlir::Value, AddressOrigin> slot_value_origins;
  MemoryEffectSummary ignored;
  scan_body(function, known_callees, callees_with_known_effects,
            slot_value_origins, /*collect_slots=*/true, ignored);
  MemoryEffectSummary summary;
  scan_body(function, known_callees, callees_with_known_effects,
            slot_value_origins, /*collect_slots=*/false, summary);
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
    llvm::StringMap<mlir::LLVM::LLVMFuncOp> definitions;
    llvm::SmallVector<mlir::LLVM::LLVMFuncOp> worklist;

    for (mlir::LLVM::LLVMFuncOp function :
         module.getOps<mlir::LLVM::LLVMFuncOp>()) {
      const std::string name = function.getSymName().str();
      if (function.isExternal()) {
        MemoryEffectSummary summary;
        if (!effects_from_attribute(function.getMemoryEffectsAttr(), summary))
          continue;
        known[name] = summary;
        has_known_effects.insert(name);
        continue;
      }

      // Definitions start at the optimistic empty summary. The worklist below
      // grows them monotonically, so a recursive function whose calls are
      // analysed before its body still reaches the correct greatest fixpoint.
      known[name] = MemoryEffectSummary{};
      has_known_effects.insert(name);
      definitions[name] = function;
      worklist.push_back(function);
    }

    // Reverse call graph: callee -> callers whose summary may grow when the
    // callee's effects become less optimistic. Only direct calls to definitions
    // can change a summary; declarations are fixed by their attributes.
    llvm::StringMap<llvm::SmallVector<std::string>> callers;
    for (const auto& entry : definitions) {
      const std::string caller_name = entry.getKey().str();
      mlir::LLVM::LLVMFuncOp function = entry.getValue();
      function.walk([&](mlir::LLVM::CallOp call) {
        const mlir::FlatSymbolRefAttr callee = call.getCalleeAttr();
        if (!callee) return;
        const std::string callee_name = callee.getValue().str();
        if (definitions.count(callee_name) == 0) return;
        callers[callee_name].push_back(caller_name);
      });
    }

    // Chaotic worklist iteration. Each summary holds six monotone bits, so a
    // function can change at most six times; a dependency change therefore
    // re-analyses only the callers that can still learn something instead of
    // rescanning every body to a global fixpoint.
    llvm::DenseSet<mlir::Operation*> queued;
    for (mlir::LLVM::LLVMFuncOp function : worklist)
      queued.insert(function.getOperation());

    while (!worklist.empty()) {
      mlir::LLVM::LLVMFuncOp function = worklist.pop_back_val();
      queued.erase(function.getOperation());

      const std::string name = function.getSymName().str();
      const MemoryEffectSummary analyzed =
          analyze_body(function, known, has_known_effects);
      if (analyzed == known[name]) continue;
      known[name] = analyzed;

      const auto callers_it = callers.find(name);
      if (callers_it == callers.end()) continue;
      for (const std::string& caller_name : callers_it->second) {
        const auto definition = definitions.find(caller_name);
        if (definition == definitions.end()) continue;
        if (!queued.insert(definition->second.getOperation()).second) continue;
        worklist.push_back(definition->second);
      }
    }

    for (const auto& entry : definitions)
      attach_memory_effects(entry.getValue(), known[entry.getKey()]);
  }
};


// Adds the semantics-preserving MLIR cleanup shared by the pre- and
// post-lowering stages.
//
// LLVM cannot infer a tail call: the `tail` marker is part of the IR contract,
// and only the lowering knows that a call's result is returned unchanged. A
// marked call lets code generation reuse the caller's frame (a sibling call)
// instead of growing the stack, which is what makes deep recursion through a
// chain of calls finish. At -O2 LLVM's own pipelines recover the information, so
// this pass matters most at -O0, where no optimization runs to find it, and it
// is cheap everywhere else.
//
// A call is marked only when the operation after it is a `return` of exactly
// that value (or a void call followed by a void return) and the callee has the
// caller's prototype. The adjacency requirement also excludes functions that
// must tear down a GC root frame after the call: that teardown is a call, so the
// user call is no longer in tail position and marking it would be a lie.
class MarkTailCallsPass
    : public mlir::PassWrapper<MarkTailCallsPass,
                               mlir::OperationPass<>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(MarkTailCallsPass)

  llvm::StringRef getArgument() const final { return "mark-tail-calls"; }
  llvm::StringRef getDescription() const final {
    return "Mark calls whose result is returned unchanged as tail calls";
  }

  void runOnOperation() override {
    getOperation()->walk([&](mlir::LLVM::CallOp call) {
      if (call.getTailCallKind() !=
          mlir::LLVM::tailcallkind::TailCallKind::None)
        return;
      auto ret = mlir::dyn_cast_or_null<mlir::LLVM::ReturnOp>(
          call->getNextNode());
      if (!ret) return;
      const bool forwards_result =
          call.getNumResults() == 1 && ret.getNumOperands() == 1 &&
          ret.getOperand(0) == call.getResult();
      const bool forwards_nothing =
          call.getNumResults() == 0 && ret.getNumOperands() == 0;
      if (!forwards_result && !forwards_nothing) return;

      auto caller = call->getParentOfType<mlir::LLVM::LLVMFuncOp>();
      if (!caller || caller.isExternal()) return;
      auto callee = mlir::SymbolTable::lookupNearestSymbolFrom<
          mlir::LLVM::LLVMFuncOp>(call, call.getCalleeAttr());
      if (!callee || callee.isExternal()) return;
      if (callee.getFunctionType() != caller.getFunctionType()) return;
      if (callee.getCConv() != caller.getCConv()) return;
      if (callee.getFunctionType().isVarArg()) return;

      call.setTailCallKind(mlir::LLVM::tailcallkind::TailCallKind::Tail);
    });
  }
};

// Only the C entry point is visible outside the object file. Every other
// generated function is reached exclusively from this module's own code, so its
// linkage should say so: external linkage forces the linker to keep functions
// the program never calls, hides their bodies from interprocedural reasoning,
// and (because the symbol is public) keeps LLVM from deleting them. `main` stays
// external because the C runtime starts there. Dropping the functions a program
// never reaches is then LLVM's job: dead-function elimination runs in every
// optimized pipeline and can only see these functions at all once they stop
// looking externally visible. At -O0 nothing is removed, which is what a debug
// build wants.
class InternalizeFunctionsPass
    : public mlir::PassWrapper<InternalizeFunctionsPass,
                               mlir::OperationPass<>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(InternalizeFunctionsPass)

  llvm::StringRef getArgument() const final { return "internalize-functions"; }
  llvm::StringRef getDescription() const final {
    return "Give every generated function except the C entry point internal "
           "linkage";
  }

  void runOnOperation() override {
    getOperation()->walk([&](mlir::LLVM::LLVMFuncOp function) {
      if (function.isExternal()) return;
      if (function.getSymName() == "main") return;
      function.setLinkage(mlir::LLVM::Linkage::Internal);
    });
  }
};

// A mutator region is the span in which a thread holds a GC registration:
// `nrq_gc_mutator_enter` establishes it and `nrq_gc_mutator_leave` ends it. Every
// generated function that can touch a managed value used to open its own nested
// region, which is pure overhead for a callee: its caller already holds the
// region, so the callee's enter/leave only bumps and restores a nesting counter
// that no collection can observe in between (the region never drops to zero).
//
// This pass removes the pair from functions that provably always run inside an
// established region. A function establishes its own region when it can be
// entered from outside the module's own calls: the C entry point, a function with
// public linkage, or a function whose address is taken (a callback, which the
// runtime or native code may invoke at any time). Every other function is only
// reached through direct calls, so it is covered once each of its callers runs
// inside a region - a fixed point that starts from the functions that establish
// one. Recursion never qualifies until the recursive cycle has an outside entry
// point, which keeps the analysis conservative.
class ElideNestedMutatorRegionsPass
    : public mlir::PassWrapper<ElideNestedMutatorRegionsPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ElideNestedMutatorRegionsPass)

  llvm::StringRef getArgument() const final {
    return "elide-nested-mutator-regions";
  }
  llvm::StringRef getDescription() const final {
    return "Drop mutator enter/leave pairs for functions that always run "
           "inside an established mutator region";
  }

  void runOnOperation() override {
    mlir::ModuleOp module = getOperation();
    llvm::StringMap<mlir::LLVM::LLVMFuncOp> defined;
    module.walk([&](mlir::LLVM::LLVMFuncOp function) {
      if (!function.isExternal()) defined[function.getSymName()] = function;
    });
    if (defined.empty()) return;

    // Every symbol a function could be entered through without a direct call to
    // it: its address was taken somewhere in the module.
    llvm::StringSet<> address_taken;
    module.walk([&](mlir::LLVM::AddressOfOp address) {
      address_taken.insert(address.getGlobalName());
    });

    // Reverse call graph over definitions in this module. Indirect calls cannot
    // name a callee, so they are covered by the address-taken rule instead. A
    // function only provides a region to its callees if it opens one itself: a
    // function that never touches a managed value emits no enter/leave, so its
    // callees still have to establish their own.
    llvm::StringMap<llvm::SmallVector<llvm::StringRef, 4>> callers;
    llvm::StringSet<> opens_region;
    for (auto& entry : defined) {
      entry.second.walk([&](mlir::LLVM::CallOp call) {
        auto callee = call.getCallee();
        if (!callee) return;
        if (*callee == "nrq_gc_mutator_enter") {
          opens_region.insert(entry.first());
          return;
        }
        if (defined.count(*callee)) callers[*callee].push_back(entry.first());
      });
    }

    // A function that opens a region and can be entered from outside the
    // module's own calls establishes the region for everything it calls.
    auto establishes_region = [&](llvm::StringRef name) {
      if (!opens_region.count(name)) return false;
      if (name == "main") return true;
      if (address_taken.count(name)) return true;
      auto function = defined.find(name);
      if (function == defined.end()) return true;
      return function->second.getLinkage() != mlir::LLVM::Linkage::Internal;
    };

    // Greatest fixpoint: start from every region-opening function that can be
    // reached only through calls, then drop the ones that still have a caller which
    // is neither an establisher nor covered. Starting from the optimistic side is
    // what handles recursion: a recursive function always has itself as a caller,
    // and that caller is exactly the function whose nested region is being removed,
    // so the edge cannot be used to prove it needs one. Removing entries to a fixed
    // point keeps the answer sound, because a function only survives while every way
    // into it is already inside a region.
    llvm::StringSet<> inside_region;
    for (auto& entry : defined) {
      llvm::StringRef name = entry.first();
      if (!opens_region.count(name) || establishes_region(name)) continue;
      auto found = callers.find(name);
      if (found == callers.end() || found->second.empty()) continue;
      inside_region.insert(name);
    }
    for (bool changed = true; changed;) {
      changed = false;
      llvm::SmallVector<llvm::StringRef, 8> escapees;
      for (auto& candidate : inside_region) {
        llvm::StringRef name = candidate.getKey();
        auto found = callers.find(name);
        if (found == callers.end()) continue;
        for (llvm::StringRef caller : found->second) {
          if (establishes_region(caller)) continue;
          if (inside_region.count(caller)) continue;
          escapees.push_back(name);
          break;
        }
      }
      for (llvm::StringRef name : escapees) {
        inside_region.erase(name);
        changed = true;
      }
    }
    if (inside_region.empty()) return;

    for (auto& entry : defined) {
      if (!inside_region.count(entry.first())) continue;
      llvm::SmallVector<mlir::Operation*, 4> region_calls;
      entry.second.walk([&](mlir::LLVM::CallOp call) {
        auto callee = call.getCallee();
        if (!callee) return;
        if (*callee != "nrq_gc_mutator_enter" &&
            *callee != "nrq_gc_mutator_leave")
          return;
        region_calls.push_back(call.getOperation());
      });
      for (mlir::Operation* call : region_calls) call->erase();
    }
  }
};

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

  // Step 4: Expose tail calls. Like memory effects below, the marker describes
  // the program rather than a pass run, so it is added at every level: without
  // it, -O0 cannot turn a returned call into a sibling call and deep recursion
  // exhausts the stack.
  pm.addPass(std::make_unique<MarkTailCallsPass>());

  // Step 5: State which functions actually leave the object file. This is a
  // property of the program too, so it is decided at every optimization level;
  // it is what allows the optimizer to drop unreachable functions instead of
  // emitting them.
  pm.addPass(std::make_unique<InternalizeFunctionsPass>());

  // Step 6: Remove nested mutator regions from functions that can only run
  // inside a region their caller already established. This runs after
  // internalization so the analysis can see which functions are actually
  // reachable only through this module's own calls.
  if (optimize) pm.addPass(std::make_unique<ElideNestedMutatorRegionsPass>());

  // Step 7: Describe the memory each generated function actually touches. This
  // runs last so it sees the final body. It runs at every optimization level:
  // the attribute is a property of the program, not of a pass run.
  pm.addPass(std::make_unique<InferFunctionMemoryEffectsPass>());
}

}  // namespace nirq
