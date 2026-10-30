// NrqToLLVMPass.cpp — lowers the nrq dialect to the LLVM dialect.
//
// This pass is the Phase 1 "compile" step in the MLIR pipeline. Its job is to
// turn every nrq.print_string op into:
//
//   1. An llvm.mlir.global constant holding the string bytes (NUL-terminated,
//      though the runtime also receives the byte length).
//   2. A call to the runtime symbol `nrq_print_string(ptr, i64)`.
//
// Algorithm:
//   The pass walks all func.func bodies in the module. When it finds a
//   nrq.print_string op it:
//     a. Materialises a uniqued global constant for the literal text.
//        Uniqueness is by content: two identical string literals produce one
//        global, reducing the symbol table size.
//     b. Emits an llvm.getelementptr to get a ptr into the global.
//     c. Emits the byte-length as an i64 constant.
//     d. Emits llvm.call @nrq_print_string(%ptr, %len).
//     e. Erases the original op.
//
// The nrq_print_string symbol is declared as an llvm.func with external
// linkage once per module; MLIR deduplicates additional declarations.
//
// Why not use MLIR's ConversionPattern machinery?
//   ConversionPattern requires a TypeConverter and a full dialect conversion
//   framework. Phase 1's nrq dialect has one op and no custom types, so a
//   simple op walk is both correct and much easier to read. Phase 2 or 3 can
//   upgrade to the pattern framework when the dialect grows.

#include <memory>
#include <string>
#include <unordered_map>

#include "mlir/Dialect/Func/IR/FuncDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "nirqoba/Lowering/NrqDialect.h"
#include "nirqoba/Lowering/NrqOps.h"

namespace nirq {
namespace {

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

// Ensures `@nrq_print_string` is declared in the module as:
//   llvm.func @nrq_print_string(!llvm.ptr, i64) -> i32
// Returns the symbol name so callers can emit a call.
static mlir::LLVM::LLVMFuncOp
ensure_runtime_decl(mlir::ModuleOp module, mlir::OpBuilder& builder) {
  const char* kSymbol = "nrq_print_string";

  // If already declared, return it.
  if (auto fn = module.lookupSymbol<mlir::LLVM::LLVMFuncOp>(kSymbol)) {
    return fn;
  }

  mlir::MLIRContext* ctx = module.getContext();
  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(module.getBody());

  auto ptrTy = mlir::LLVM::LLVMPointerType::get(ctx);
  auto i64Ty = builder.getI64Type();
  auto i32Ty = builder.getI32Type();
  auto fnTy  = mlir::LLVM::LLVMFunctionType::get(i32Ty, {ptrTy, i64Ty});

  return builder.create<mlir::LLVM::LLVMFuncOp>(
    module.getLoc(), kSymbol, fnTy,
    mlir::LLVM::Linkage::External);
}

// Creates (or returns) a module-level llvm.mlir.global constant for `text`.
// Globals are uniqued by content using a module-level map passed in by the
// caller; two identical literals share one global.
static mlir::LLVM::GlobalOp
get_or_create_string_global(mlir::ModuleOp module,
                             mlir::OpBuilder& builder,
                             const std::string& text,
                             std::unordered_map<std::string, mlir::LLVM::GlobalOp>& cache) {
  auto it = cache.find(text);
  if (it != cache.end()) return it->second;

  mlir::MLIRContext* ctx = module.getContext();
  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(module.getBody());

  // The global stores the UTF-8 bytes including a NUL terminator.
  const std::string stored = text + '\0';
  const std::size_t num_bytes = stored.size();

  auto arrayTy = mlir::LLVM::LLVMArrayType::get(builder.getI8Type(),
                                                  static_cast<unsigned>(num_bytes));

  // Unique the name by a counter to avoid collisions.
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

// ----------------------------------------------------------------------------
// Pass implementation
// ----------------------------------------------------------------------------

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
    bool failed = false;

    // Ensure the runtime declaration exists before walking so any newly
    // created call sites see a defined callee.
    mlir::LLVM::LLVMFuncOp runtime_fn = ensure_runtime_decl(module, builder);

    // Collect all nrq.print_string ops first to avoid mutation during walk.
    llvm::SmallVector<nrq::PrintStringOp, 8> print_ops;
    module.walk([&](nrq::PrintStringOp op) {
      print_ops.push_back(op);
    });

    for (nrq::PrintStringOp op : print_ops) {
      const std::string text = op.getValue().str();
      builder.setInsertionPoint(op);

      mlir::Location loc = op.getLoc();

      // --- 1. Materialise or reuse the string global.
      mlir::LLVM::GlobalOp global = get_or_create_string_global(
          module, builder, text, string_cache);

      // --- 2. GEP to get a pointer to the first byte.
      auto ptrTy = mlir::LLVM::LLVMPointerType::get(ctx);
      auto i64Ty = builder.getI64Type();
      auto i32Ty = builder.getI32Type();

      // addressof gives us a ptr to the global array.
      mlir::Value addr = builder.create<mlir::LLVM::AddressOfOp>(
          loc, ptrTy, global.getSymName());

      // GEP [N x i8]* %addr, 0, 0 -> ptr to first byte.
      mlir::Value zero = builder.create<mlir::LLVM::ConstantOp>(
          loc, i64Ty, builder.getI64IntegerAttr(0));

      mlir::Value gep = builder.create<mlir::LLVM::GEPOp>(
          loc,
          ptrTy,
          global.getType(),
          addr,
          mlir::ValueRange{zero, zero},
          /*inbounds=*/true);

      // --- 3. Length as i64 (not counting the NUL terminator).
      mlir::Value len_val = builder.create<mlir::LLVM::ConstantOp>(
          loc, i64Ty,
          builder.getI64IntegerAttr(static_cast<int64_t>(text.size())));

      // --- 4. Call nrq_print_string(ptr, len).
      builder.create<mlir::LLVM::CallOp>(
          loc,
          mlir::TypeRange{i32Ty},
          mlir::FlatSymbolRefAttr::get(ctx, runtime_fn.getSymName()),
          mlir::ValueRange{gep, len_val});

      // --- 5. Erase the original nrq.print_string op.
      op.erase();
    }

    if (failed) {
      signalPassFailure();
    }
  }
};

}  // namespace

// Factory function declared in PassPipeline.cpp.
std::unique_ptr<mlir::Pass> createNrqToLLVMPass() {
  return std::make_unique<NrqToLLVMPass>();
}

}  // namespace nirq
