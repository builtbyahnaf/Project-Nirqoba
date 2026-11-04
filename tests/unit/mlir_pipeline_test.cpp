#include "test_harness.h"

#include <memory>
#include <string>

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"

#include "nirqoba/Lowering/PassPipeline.h"
#include "nirqoba/Lowering/RegisterDialects.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Lexer.h"
#include "nirqoba/Frontend/Parser.h"
#include "nirqoba/Frontend/SourceFile.h"
#include "nirqoba/Lowering/TirToMlir.h"
#include "nirqoba/Sema/TypeChecker.h"
#include "nirqoba/TIR/Builder.h"
#include "nirqoba/TIR/Verifier.h"

using namespace std;

namespace {

// A deliberately small module that exercises every cleanup decision:
//
//   * two identical value-producing operations that CSE must merge;
//   * duplicate constants that CSE must merge;
//   * one trivially dead multiplication whose constant must be removed by
//     canonicalization;
//   * two calls with unknown side effects that must both survive;
//   * one nrq runtime operation that must be lowered to a call, not deleted;
//   * one unreachable block that remove-unreachable-blocks must erase.
//
// The function takes an argument so the multiplications cannot be constant
// folded; the test is about cleanup, not folding.
constexpr const char* kCleanupProbe = R"mlir(
module {
  llvm.func @side_effect()
  llvm.func @cleanup_probe(%x : i64) -> i64 {
    %two_a = llvm.mlir.constant(2 : i64) : i64
    %two_b = llvm.mlir.constant(2 : i64) : i64
    %three = llvm.mlir.constant(3 : i64) : i64
    %first = llvm.mul %x, %two_a : i64
    %duplicate = llvm.mul %x, %two_b : i64
    %dead = llvm.mul %x, %three : i64
    %sum = llvm.add %first, %duplicate : i64
    nrq.print_int %sum
    llvm.call @side_effect() : () -> ()
    llvm.call @side_effect() : () -> ()
    llvm.return %sum : i64
  ^unreachable:
    llvm.return %two_a : i64
  }
}
)mlir";

// SCCP must propagate a constant through a branch argument so the conditional
// edge becomes provably constant. This is deliberately stronger than a direct
// `icmp` of two constants: the value crosses a CFG edge as a block argument, so
// the probe exercises propagation and reachability rather than only folding.
constexpr const char* kConstantProbe = R"mlir(
module {
  llvm.func @constant_probe() -> i64 {
    %one = llvm.mlir.constant(1 : i64) : i64
    llvm.br ^next(%one : i64)
  ^next(%argument : i64):
    %is_one = llvm.icmp "eq" %argument, %one : i64
    llvm.cond_br %is_one, ^yes, ^no
  ^yes:
    %six = llvm.mlir.constant(6 : i64) : i64
    %seven = llvm.add %one, %six : i64
    llvm.return %seven : i64
  ^no:
    %nine = llvm.mlir.constant(9 : i64) : i64
    llvm.return %nine : i64
  }
}
)mlir";

// A body for each memory-effect outcome the inference pass must distinguish:
//
//   * `pure` performs arithmetic only, so it may not touch any memory;
//   * `reader` loads through its pointer argument, so it reads argument memory
//     and nothing else;
//   * `spilled` spills that argument into a stack slot and reads it back, the
//     shape the toolchain generates for every local; the slot must not erase
//     the argument provenance, and the slot itself is inaccessible memory;
//   * `opaque_caller` calls a function with no known effects, so it must keep
//     the default (no attribute) rather than promise anything.
constexpr const char* kMemoryEffectProbe = R"mlir(
module {
  llvm.func @opaque()
  llvm.func @pure(%x : i64) -> i64 {
    %two = llvm.mlir.constant(2 : i64) : i64
    %product = llvm.mul %x, %two : i64
    llvm.return %product : i64
  }
  llvm.func @reader(%p : !llvm.ptr) -> i64 {
    %value = llvm.load %p : !llvm.ptr -> i64
    llvm.return %value : i64
  }
  llvm.func @spilled(%p : !llvm.ptr) -> i64 {
    %one = llvm.mlir.constant(1 : i64) : i64
    %slot = llvm.alloca %one x !llvm.ptr : (i64) -> !llvm.ptr
    llvm.store %p, %slot : !llvm.ptr, !llvm.ptr
    %reloaded = llvm.load %slot : !llvm.ptr -> !llvm.ptr
    %value = llvm.load %reloaded : !llvm.ptr -> i64
    llvm.return %value : i64
  }
  llvm.func @opaque_caller() {
    llvm.call @opaque() : () -> ()
    llvm.return
  }
}
)mlir";

struct Fixture {
  mlir::DialectRegistry registry;
  unique_ptr<mlir::MLIRContext> context;
  mlir::OwningOpRef<mlir::ModuleOp> module;

  Fixture() {
    // Dialects must be registered before the context is built because the
    // context copies the registry at construction time.
    nirq::register_all_dialects(registry);
    context = make_unique<mlir::MLIRContext>(registry);
    context->loadAllAvailableDialects();
    module = mlir::parseSourceString<mlir::ModuleOp>(kCleanupProbe, context.get());
  }
};

int count_operations(mlir::ModuleOp module, llvm::StringRef name) {
  int count = 0;
  module.walk([&](mlir::Operation* operation) {
    if (operation->getName().getStringRef() == name) ++count;
  });
  return count;
}

struct ConstantFixture {
  mlir::DialectRegistry registry;
  unique_ptr<mlir::MLIRContext> context;
  mlir::OwningOpRef<mlir::ModuleOp> module;

  ConstantFixture() {
    nirq::register_all_dialects(registry);
    context = make_unique<mlir::MLIRContext>(registry);
    context->loadAllAvailableDialects();
    module = mlir::parseSourceString<mlir::ModuleOp>(kConstantProbe,
                                                     context.get());
  }
};

struct MemoryEffectFixture {
  mlir::DialectRegistry registry;
  unique_ptr<mlir::MLIRContext> context;
  mlir::OwningOpRef<mlir::ModuleOp> module;

  MemoryEffectFixture() {
    nirq::register_all_dialects(registry);
    context = make_unique<mlir::MLIRContext>(registry);
    context->loadAllAvailableDialects();
    module = mlir::parseSourceString<mlir::ModuleOp>(kMemoryEffectProbe,
                                                     context.get());
  }

  mlir::LLVM::LLVMFuncOp function(llvm::StringRef name) {
    return module->lookupSymbol<mlir::LLVM::LLVMFuncOp>(name);
  }
};

using mlir::LLVM::ModRefInfo;

// A source program that forces every kind of stack slot the lowering can
// create: scalar and aggregate locals, a managed allocation whose reference
// offsets must be described in memory, and a profiler capture. Every one of
// them used to be allocated where it is used, which inside the loop means once
// per iteration.
constexpr const char* kStackSlotSource = R"nrqba(
struct Node {
    int value;
    managed<Node> next;
}

int function main() {
    var total = 0;
    var i = 0;
    while (i < 4) {
        var node = array.allocate<Node>(1);
        node[0].value = i;
        var profile = memory.profiler.capture();
        total += node[0].value;
        i += 1;
    }
    print(total);
    return 0;
}
)nrqba";

mlir::OwningOpRef<mlir::ModuleOp> lower_source_to_mlir(
    const std::string& source, mlir::MLIRContext& context,
    nirq::DiagnosticEngine& diagnostics) {
  nirq::SourceFile file("lowering_probe.nrqba", source);
  nirq::Lexer lexer(file, diagnostics);
  nirq::Parser parser(lexer.tokenize(), diagnostics);
  auto program = parser.parse_program();
  if (!program || diagnostics.has_errors()) return nullptr;
  nirq::TypeChecker checker(diagnostics);
  if (!checker.check(*program)) return nullptr;
  nirq::TirBuilder builder(diagnostics);
  auto tir_module = builder.lower(*program);
  if (!tir_module) return nullptr;
  if (!nirq::verify_tir_module(*tir_module, diagnostics)) return nullptr;
  return nirq::translate_tir_to_mlir(*tir_module, context, diagnostics);
}

}  // namespace

TEST(MlirPipelineTest, CleanupMergesPureOperationsAndPreservesEffects) {
  Fixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.module));
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*fixture.module)));

  mlir::PassManager pass_manager(fixture.context.get());
  nirq::build_lowering_pipeline(pass_manager, /*optimize=*/true);
  ASSERT_TRUE(mlir::succeeded(pass_manager.run(*fixture.module)));

  // The pass manager verifies after every pass. This second check documents
  // that the cleaned module is also valid as a whole.
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*fixture.module)));

  // CSE merges the two identical multiplications; canonicalization removes the
  // dead multiplication. One multiplication remains in the reachable body.
  ASSERT_EQ(count_operations(*fixture.module, "llvm.mul"), 1);

  // Only the constant used by the surviving multiplication is needed.
  ASSERT_EQ(count_operations(*fixture.module, "llvm.mlir.constant"), 1);

  // The unreachable block and its terminator are gone; only the reachable
  // return remains.
  ASSERT_EQ(count_operations(*fixture.module, "llvm.return"), 1);

  // Both unknown-effect calls and the lowered nrq.print_int call survive. This
  // is the semantic-safety half of the cleanup contract: no runtime call was
  // dropped or duplicated.
  ASSERT_EQ(count_operations(*fixture.module, "llvm.call"), 3);
}

TEST(MlirPipelineTest, OptimizedPipelinePropagatesConstantsAndFoldsBranches) {
  ConstantFixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.module));
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*fixture.module)));

  mlir::PassManager pass_manager(fixture.context.get());
  nirq::build_lowering_pipeline(pass_manager, /*optimize=*/true);
  ASSERT_TRUE(mlir::succeeded(pass_manager.run(*fixture.module)));
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*fixture.module)));

  // The branch argument is known to be 1, so the comparison is constant and
  // the `^no` edge and block are unreachable. The surviving addition folds to
  // the single constant 7.
  ASSERT_EQ(count_operations(*fixture.module, "llvm.icmp"), 0);
  ASSERT_EQ(count_operations(*fixture.module, "llvm.add"), 0);
  ASSERT_EQ(count_operations(*fixture.module, "llvm.return"), 1);

  bool found_seven = false;
  bool found_nine = false;
  fixture.module->walk([&](mlir::Operation* operation) {
    auto constant = mlir::dyn_cast<mlir::LLVM::ConstantOp>(operation);
    if (!constant) return;
    auto integer = mlir::dyn_cast<mlir::IntegerAttr>(constant.getValue());
    if (!integer || integer.getInt() != 7) return;
    found_seven = true;
  });
  fixture.module->walk([&](mlir::Operation* operation) {
    auto constant = mlir::dyn_cast<mlir::LLVM::ConstantOp>(operation);
    if (!constant) return;
    auto integer = mlir::dyn_cast<mlir::IntegerAttr>(constant.getValue());
    if (!integer || integer.getInt() != 9) return;
    found_nine = true;
  });
  ASSERT_TRUE(found_seven);
  ASSERT_FALSE(found_nine);
}

TEST(MlirPipelineTest, O0PipelineKeepsConstantPropagationProbe) {
  ConstantFixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.module));

  mlir::PassManager pass_manager(fixture.context.get());
  nirq::build_lowering_pipeline(pass_manager, /*optimize=*/false);
  ASSERT_TRUE(mlir::succeeded(pass_manager.run(*fixture.module)));

  ASSERT_EQ(count_operations(*fixture.module, "llvm.icmp"), 1);
  ASSERT_EQ(count_operations(*fixture.module, "llvm.add"), 1);
  ASSERT_EQ(count_operations(*fixture.module, "llvm.return"), 2);
}

TEST(MlirPipelineTest, O0PipelineKeepsDirectLoweringShape) {
  Fixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.module));

  mlir::PassManager pass_manager(fixture.context.get());
  nirq::build_lowering_pipeline(pass_manager, /*optimize=*/false);
  ASSERT_TRUE(mlir::succeeded(pass_manager.run(*fixture.module)));

  // No canonicalization or CSE: both duplicate multiplications, the dead
  // multiplication, all three constants, and the unreachable return remain.
  ASSERT_EQ(count_operations(*fixture.module, "llvm.mul"), 3);
  ASSERT_EQ(count_operations(*fixture.module, "llvm.mlir.constant"), 3);
  ASSERT_EQ(count_operations(*fixture.module, "llvm.return"), 2);

  // Even at -O0 the mandatory nrq lowering produces the runtime call, so the
  // side-effect count must equal the optimized case.
  ASSERT_EQ(count_operations(*fixture.module, "llvm.call"), 3);
}

TEST(MlirPipelineTest, InfersFunctionMemoryEffectsFromProvenance) {
  MemoryEffectFixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.module));
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*fixture.module)));

  mlir::PassManager pass_manager(fixture.context.get());
  nirq::build_lowering_pipeline(pass_manager, /*optimize=*/false);
  ASSERT_TRUE(mlir::succeeded(pass_manager.run(*fixture.module)));

  auto pure = fixture.function("pure");
  ASSERT_TRUE(static_cast<bool>(pure));
  auto pure_effects = pure.getMemoryEffectsAttr();
  ASSERT_TRUE(static_cast<bool>(pure_effects));
  ASSERT_EQ(pure_effects.getOther(), ModRefInfo::NoModRef);
  ASSERT_EQ(pure_effects.getArgMem(), ModRefInfo::NoModRef);
  ASSERT_EQ(pure_effects.getInaccessibleMem(), ModRefInfo::NoModRef);

  auto reader = fixture.function("reader");
  ASSERT_TRUE(static_cast<bool>(reader));
  auto reader_effects = reader.getMemoryEffectsAttr();
  ASSERT_TRUE(static_cast<bool>(reader_effects));
  ASSERT_EQ(reader_effects.getArgMem(), ModRefInfo::Ref);
  ASSERT_EQ(reader_effects.getOther(), ModRefInfo::NoModRef);

  // Spilling the argument into a stack slot must not lose its provenance: the
  // load through the reloaded pointer still reads argument memory, and the slot
  // itself is inaccessible memory the caller cannot name.
  auto spilled = fixture.function("spilled");
  ASSERT_TRUE(static_cast<bool>(spilled));
  auto spilled_effects = spilled.getMemoryEffectsAttr();
  ASSERT_TRUE(static_cast<bool>(spilled_effects));
  ASSERT_EQ(spilled_effects.getArgMem(), ModRefInfo::Ref);
  ASSERT_EQ(spilled_effects.getOther(), ModRefInfo::NoModRef);
  ASSERT_EQ(spilled_effects.getInaccessibleMem(), ModRefInfo::ModRef);

  // A callee with unknown effects poisons the summary, so the caller keeps the
  // conservative default instead of claiming it touches nothing.
  auto caller = fixture.function("opaque_caller");
  ASSERT_TRUE(static_cast<bool>(caller));
  ASSERT_FALSE(static_cast<bool>(caller.getMemoryEffectsAttr()));
}

TEST(MlirPipelineTest, AllocatesEveryStackSlotInTheEntryBlock) {
  mlir::DialectRegistry registry;
  nirq::register_all_dialects(registry);
  mlir::MLIRContext context(registry);
  context.loadAllAvailableDialects();
  nirq::DiagnosticEngine diagnostics;

  auto module = lower_source_to_mlir(kStackSlotSource, context, diagnostics);
  ASSERT_FALSE(diagnostics.has_errors());
  ASSERT_TRUE(static_cast<bool>(module));
  ASSERT_TRUE(mlir::succeeded(mlir::verify(*module)));

  // An `llvm.alloca` outside the entry block is re-executed every time its
  // block runs, so a slot created in a loop body grows the stack until the
  // process dies. It is also unreachable for mem2reg, which only promotes entry
  // block slots, so the local would keep going through memory at every level.
  unsigned allocas = 0;
  unsigned outside_entry = 0;
  module->walk([&](mlir::func::FuncOp function) {
    mlir::Block& entry = function.getBody().front();
    function.walk([&](mlir::LLVM::AllocaOp alloca) {
      ++allocas;
      if (alloca->getBlock() != &entry) ++outside_entry;
    });
  });

  ASSERT_TRUE(allocas > 0);
  ASSERT_EQ(outside_entry, 0u);
}
