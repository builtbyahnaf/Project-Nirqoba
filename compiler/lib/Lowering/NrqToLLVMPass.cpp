// NrqToLLVMPass.cpp — lowers the nrq dialect to the LLVM dialect.

#include <memory>
#include <string>
#include <unordered_map>

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "nirqoba/Lowering/NrqDialect.h"
#include "nirqoba/Lowering/NrqOps.h"

namespace nirq {
namespace {

static mlir::LLVM::LLVMFuncOp ensure_runtime_decl(
    mlir::ModuleOp module, mlir::OpBuilder& builder,
    llvm::StringRef symbol, mlir::Type retTy, llvm::ArrayRef<mlir::Type> argTys) {
  if (auto fn = module.lookupSymbol<mlir::LLVM::LLVMFuncOp>(symbol)) {
    return fn;
  }
  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(module.getBody());
  auto fnTy = mlir::LLVM::LLVMFunctionType::get(retTy, argTys);
  return builder.create<mlir::LLVM::LLVMFuncOp>(
      module.getLoc(), symbol, fnTy, mlir::LLVM::Linkage::External);
}

static mlir::LLVM::GlobalOp get_or_create_string_global(
    mlir::ModuleOp module, mlir::OpBuilder& builder,
    const std::string& text,
    std::unordered_map<std::string, mlir::LLVM::GlobalOp>& cache) {
  auto it = cache.find(text);
  if (it != cache.end()) return it->second;

  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(module.getBody());

  const std::string stored = text + '\0';
  const std::size_t num_bytes = stored.size();

  auto arrayTy = mlir::LLVM::LLVMArrayType::get(builder.getI8Type(),
                                                static_cast<unsigned>(num_bytes));

  static int counter = 0;
  const std::string sym = "__nrq_str_" + std::to_string(counter++);

  auto global = builder.create<mlir::LLVM::GlobalOp>(
      module.getLoc(),
      arrayTy,
      /*isConstant=*/true,
      mlir::LLVM::Linkage::Private,
      sym,
      builder.getStringAttr(stored));

  cache.emplace(text, global);
  return global;
}

class NrqToLLVMPass : public mlir::PassWrapper<NrqToLLVMPass, mlir::OperationPass<mlir::ModuleOp>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NrqToLLVMPass)

  llvm::StringRef getArgument() const final { return "nrq-to-llvm"; }
  llvm::StringRef getDescription() const final {
    return "Lower the nrq dialect to the LLVM dialect";
  }

  void runOnOperation() override {
    mlir::ModuleOp module = getOperation();
    mlir::MLIRContext* ctx = module.getContext();
    mlir::OpBuilder builder(ctx);

    std::unordered_map<std::string, mlir::LLVM::GlobalOp> string_cache;
    auto ptrTy = mlir::LLVM::LLVMPointerType::get(ctx);
    auto i64Ty = builder.getI64Type();
    auto i32Ty = builder.getI32Type();
    auto f64Ty = builder.getF64Type();

    // 1. Lower nrq.print_string
    llvm::SmallVector<nrq::PrintStringOp, 8> str_ops;
    module.walk([&](nrq::PrintStringOp op) { str_ops.push_back(op); });
    if (!str_ops.empty()) {
      auto fn = ensure_runtime_decl(module, builder, "nrq_print_string", i32Ty, {ptrTy, i64Ty});
      for (nrq::PrintStringOp op : str_ops) {
        const std::string text = op.getValue().str();
        builder.setInsertionPoint(op);
        mlir::Location loc = op.getLoc();

        mlir::LLVM::GlobalOp global = get_or_create_string_global(module, builder, text, string_cache);
        mlir::Value addr = builder.create<mlir::LLVM::AddressOfOp>(loc, ptrTy, global.getSymName());
        mlir::Value zero = builder.create<mlir::LLVM::ConstantOp>(loc, i64Ty, builder.getI64IntegerAttr(0));
        mlir::Value gep = builder.create<mlir::LLVM::GEPOp>(
            loc, ptrTy, global.getType(), addr, mlir::ValueRange{zero, zero}, /*inbounds=*/true);
        mlir::Value len_val = builder.create<mlir::LLVM::ConstantOp>(
            loc, i64Ty, builder.getI64IntegerAttr(static_cast<int64_t>(text.size())));

        builder.create<mlir::LLVM::CallOp>(
            loc, mlir::TypeRange{i32Ty},
            mlir::FlatSymbolRefAttr::get(ctx, fn.getSymName()),
            mlir::ValueRange{gep, len_val});
        op.erase();
      }
    }

    // 2. Lower nrq.print_int
    llvm::SmallVector<nrq::PrintIntOp, 8> int_ops;
    module.walk([&](nrq::PrintIntOp op) { int_ops.push_back(op); });
    if (!int_ops.empty()) {
      auto fn = ensure_runtime_decl(module, builder, "nrq_print_int", i32Ty, {i64Ty});
      for (nrq::PrintIntOp op : int_ops) {
        builder.setInsertionPoint(op);
        builder.create<mlir::LLVM::CallOp>(
            op.getLoc(), mlir::TypeRange{i32Ty},
            mlir::FlatSymbolRefAttr::get(ctx, fn.getSymName()),
            mlir::ValueRange{op.getValue()});
        op.erase();
      }
    }

    // 3. Lower nrq.print_uint
    llvm::SmallVector<nrq::PrintUintOp, 8> uint_ops;
    module.walk([&](nrq::PrintUintOp op) { uint_ops.push_back(op); });
    if (!uint_ops.empty()) {
      auto fn = ensure_runtime_decl(module, builder, "nrq_print_uint", i32Ty, {i64Ty});
      for (nrq::PrintUintOp op : uint_ops) {
        builder.setInsertionPoint(op);
        builder.create<mlir::LLVM::CallOp>(
            op.getLoc(), mlir::TypeRange{i32Ty},
            mlir::FlatSymbolRefAttr::get(ctx, fn.getSymName()),
            mlir::ValueRange{op.getValue()});
        op.erase();
      }
    }

    // 4. Lower nrq.print_f64
    llvm::SmallVector<nrq::PrintF64Op, 8> f64_ops;
    module.walk([&](nrq::PrintF64Op op) { f64_ops.push_back(op); });
    if (!f64_ops.empty()) {
      auto fn = ensure_runtime_decl(module, builder, "nrq_print_f64", i32Ty, {f64Ty});
      for (nrq::PrintF64Op op : f64_ops) {
        builder.setInsertionPoint(op);
        builder.create<mlir::LLVM::CallOp>(
            op.getLoc(), mlir::TypeRange{i32Ty},
            mlir::FlatSymbolRefAttr::get(ctx, fn.getSymName()),
            mlir::ValueRange{op.getValue()});
        op.erase();
      }
    }

    // 5. Lower nrq.print_bool
    llvm::SmallVector<nrq::PrintBoolOp, 8> bool_ops;
    module.walk([&](nrq::PrintBoolOp op) { bool_ops.push_back(op); });
    if (!bool_ops.empty()) {
      auto fn = ensure_runtime_decl(module, builder, "nrq_print_bool", i32Ty, {i32Ty});
      for (nrq::PrintBoolOp op : bool_ops) {
        builder.setInsertionPoint(op);
        mlir::Value i32_val = builder.create<mlir::LLVM::ZExtOp>(op.getLoc(), i32Ty, op.getValue());
        builder.create<mlir::LLVM::CallOp>(
            op.getLoc(), mlir::TypeRange{i32Ty},
            mlir::FlatSymbolRefAttr::get(ctx, fn.getSymName()),
            mlir::ValueRange{i32_val});
        op.erase();
      }
    }

    // 6. Lower nrq.print_char
    llvm::SmallVector<nrq::PrintCharOp, 8> char_ops;
    module.walk([&](nrq::PrintCharOp op) { char_ops.push_back(op); });
    if (!char_ops.empty()) {
      auto fn = ensure_runtime_decl(module, builder, "nrq_print_char", i32Ty, {i32Ty});
      for (nrq::PrintCharOp op : char_ops) {
        builder.setInsertionPoint(op);
        builder.create<mlir::LLVM::CallOp>(
            op.getLoc(), mlir::TypeRange{i32Ty},
            mlir::FlatSymbolRefAttr::get(ctx, fn.getSymName()),
            mlir::ValueRange{op.getValue()});
        op.erase();
      }
    }
  }
};

}  // namespace

std::unique_ptr<mlir::Pass> createNrqToLLVMPass() {
  return std::make_unique<NrqToLLVMPass>();
}

}  // namespace nirq
