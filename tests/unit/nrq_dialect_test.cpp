#include "test_harness.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "llvm/Support/raw_ostream.h"

#include "nirqoba/Lowering/NrqDialect.h"
#include "nirqoba/Lowering/NrqOps.h"
#include "nirqoba/Lowering/RegisterDialects.h"

using namespace nirq;

TEST(NrqDialectTest, RegisterAndCreateOp) {
  mlir::DialectRegistry registry;
  register_all_dialects(registry);
  mlir::MLIRContext context(registry);
  context.loadAllAvailableDialects();

  mlir::OpBuilder builder(&context);
  mlir::Location loc = builder.getUnknownLoc();

  auto print_op = builder.create<nrq::PrintStringOp>(
      loc, builder.getStringAttr("Hello from Dialect Test"));

  std::string output;
  llvm::raw_string_ostream os(output);
  print_op.print(os);

  ASSERT_TRUE(output.find("nrq.print_string \"Hello from Dialect Test\"") != std::string::npos);

  print_op.erase();
}
