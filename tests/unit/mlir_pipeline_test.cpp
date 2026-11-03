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
