// Phase 1 ownership representation tests.
//
// These tests exercise the semantic ownership categories and the TIR contract
// that carries them: transfer, borrowing, lifetimes and destruction obligations.
// They build real TIR from source and then corrupt the representation on purpose,
// so every verifier rule has both a positive and a negative test.
#include "test_harness.h"

#include <string>
#include <vector>

#include "nirqoba/Common/Type.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Lexer.h"
#include "nirqoba/Frontend/Parser.h"
#include "nirqoba/Frontend/SourceFile.h"
#include "nirqoba/Sema/TypeChecker.h"
#include "nirqoba/TIR/Builder.h"
#include "nirqoba/TIR/Printing.h"
#include "nirqoba/TIR/Verifier.h"

using namespace nirq;
using namespace std;

namespace {

unique_ptr<tir::Module> lower_source(const string& name, const string& source,
                                     DiagnosticEngine& diag) {
  SourceFile file(name, source);
  Lexer lexer(file, diag);
  Parser parser(lexer.tokenize(), diag);
  auto program = parser.parse_program();
  if (program == nullptr || diag.has_errors()) return nullptr;
  TypeChecker checker(diag);
  if (!checker.check(*program) || diag.has_errors()) return nullptr;
  TirBuilder builder(diag);
  return builder.lower(*program);
}

const tir::Function* find_function(const tir::Module& module,
                                   const string& name) {
  for (const auto& function : module.functions())
    if (function->name() == name) return function.get();
  return nullptr;
}

tir::Function* find_function(tir::Module& module, const string& name) {
  for (auto& function : module.functions())
    if (function->name() == name) return function.get();
  return nullptr;
}

int count_kind(const tir::Function& function, tir::OpKind kind) {
  int count = 0;
  for (const tir::Op& op : function.operations())
    if (op.kind == kind) ++count;
  return count;
}

const tir::Op* first_kind(const tir::Function& function, tir::OpKind kind) {
  for (const tir::Op& op : function.operations())
    if (op.kind == kind) return &op;
  return nullptr;
}

bool diagnostic_contains(const DiagnosticEngine& diag, const string& needle) {
  for (const Diagnostic& diagnostic : diag.diagnostics())
    if (diagnostic.message.find(needle) != string::npos) return true;
  return false;
}

bool has_note(const DiagnosticEngine& diag, const string& needle) {
  for (const Diagnostic& diagnostic : diag.diagnostics())
    if (diagnostic.severity == Severity::Note &&
        diagnostic.message.find(needle) != string::npos)
      return true;
  return false;
}

const tir::Op* find_drop(const tir::Function& function) {
  return first_kind(function, tir::OpKind::Drop);
}

}  // namespace

TEST(OwnershipTest, CategoriesClassifyOwningBorrowingAndTransfer) {
  ASSERT_TRUE(is_owning_ownership(ValueOwnership::Owned));
  ASSERT_TRUE(is_owning_ownership(ValueOwnership::Shared));
  ASSERT_TRUE(is_owning_ownership(ValueOwnership::ArenaBound));
  ASSERT_FALSE(is_owning_ownership(ValueOwnership::GcManaged));
  ASSERT_FALSE(is_owning_ownership(ValueOwnership::Borrowed));

  ASSERT_TRUE(is_borrowing_ownership(ValueOwnership::Borrowed));
  ASSERT_TRUE(is_borrowing_ownership(ValueOwnership::MutableBorrowed));
  ASSERT_TRUE(is_borrowing_ownership(ValueOwnership::Weak));
  ASSERT_FALSE(is_borrowing_ownership(ValueOwnership::Owned));

  ASSERT_TRUE(is_transferable_ownership(ValueOwnership::Owned));
  ASSERT_TRUE(is_transferable_ownership(ValueOwnership::Shared));
  ASSERT_FALSE(is_transferable_ownership(ValueOwnership::GcManaged));
  ASSERT_FALSE(is_transferable_ownership(ValueOwnership::Raw));

  ASSERT_TRUE(requires_destruction(ValueOwnership::Owned));
  ASSERT_TRUE(requires_destruction(ValueOwnership::Shared));
  ASSERT_FALSE(requires_destruction(ValueOwnership::GcManaged));
  ASSERT_FALSE(requires_destruction(ValueOwnership::Borrowed));

  ASSERT_EQ(string(ownership_name(ValueOwnership::MutableBorrowed)),
            string("mutable_borrowed"));
  ASSERT_EQ(string(ownership_name(ValueOwnership::ArenaBound)),
            string("arena_bound"));
}

TEST(OwnershipTest, ReturnTransferEmitsExplicitMoveAndTransfersObligation) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_return.nrqba",
      "byte* function make() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return buffer;\n"
      "}\n"
      "int function main() {\n"
      "  let result = make();\n"
      "  memory.deallocate(result);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  ASSERT_TRUE(verify_tir_module(*module, diag));

  const tir::Function* make = find_function(*module, "make");
  ASSERT_TRUE(make != nullptr);
  const tir::Op* move = first_kind(*make, tir::OpKind::Move);
  ASSERT_TRUE(move != nullptr);
  ASSERT_TRUE(move->transfer_kind == tir::TransferKind::Return);
  ASSERT_EQ(move->result.id != move->operand.id, true);
  ASSERT_EQ(move->result.type == move->operand.type, true);
  ASSERT_EQ(move->result.ownership, ValueOwnership::Owned);

  // The declaration cleanup in `make` was suppressed because the value moved to
  // the caller, so the obligation is recorded as transferred rather than
  // dropped, and `make` emits no release call.
  ASSERT_EQ(count_kind(*make, tir::OpKind::Drop), 0);
  ASSERT_EQ(make->obligations().size(), 1u);
  ASSERT_TRUE(make->obligations().front().transferred);
  ASSERT_FALSE(make->obligations().front().discharged);

  ASSERT_TRUE(print_tir(*module).find("tir.move %") != string::npos);
  ASSERT_TRUE(print_tir(*module).find("kind=return") != string::npos);
}

TEST(OwnershipTest, ArgumentTransferEmitsMoveWithArgumentReason) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_argument.nrqba",
      "void function consume(byte* value) {\n"
      "  memory.deallocate(value);\n"
      "}\n"
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  consume(buffer);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  const tir::Op* move = first_kind(*main_fn, tir::OpKind::Move);
  ASSERT_TRUE(move != nullptr);
  ASSERT_TRUE(move->transfer_kind == tir::TransferKind::Argument);
  ASSERT_TRUE(verify_tir_module(*module, diag));
}

TEST(OwnershipTest, AddressOfEmitsBorrowLifetimeAndScopeEnd) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_borrow.nrqba",
      "int function main() {\n"
      "  var value = 5;\n"
      "  let pointer = &value;\n"
      "  print(*pointer);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  ASSERT_TRUE(verify_tir_module(*module, diag));

  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  const tir::Op* borrow = first_kind(*main_fn, tir::OpKind::Borrow);
  ASSERT_TRUE(borrow != nullptr);
  ASSERT_TRUE(borrow->mutable_borrow);
  ASSERT_EQ(borrow->operand.ownership, ValueOwnership::MutableBorrowed);
  ASSERT_EQ(count_kind(*main_fn, tir::OpKind::EndBorrow), 1);

  const auto& lifetimes = main_fn->lifetimes();
  ASSERT_EQ(lifetimes.size(), 1u);
  ASSERT_TRUE(lifetimes.front().kind == tir::LifetimeKind::Lexical);
  ASSERT_EQ(lifetimes.front().borrow.id, borrow->operand.id);
  ASSERT_EQ(lifetimes.front().source.id, borrow->lhs.id);
  ASSERT_TRUE(lifetimes.front().mutable_access);
  ASSERT_TRUE(print_tir(*module).find("tir.borrow") != string::npos);
  ASSERT_TRUE(print_tir(*module).find("tir.end_borrow") != string::npos);
}

TEST(OwnershipTest, BorrowedParameterCarriesCallerLifetime) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_parameter_lifetime.nrqba",
      "int function read(borrow int* value) {\n"
      "  return *value;\n"
      "}\n"
      "int function main() {\n"
      "  var value = 7;\n"
      "  return read(&value);\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  ASSERT_TRUE(verify_tir_module(*module, diag));

  const tir::Function* read = find_function(*module, "read");
  ASSERT_TRUE(read != nullptr);
  ASSERT_EQ(read->parameters().size(), 1u);
  ASSERT_EQ(read->parameters().front().ownership, ValueOwnership::Borrowed);
  ASSERT_EQ(read->lifetimes().size(), 1u);
  ASSERT_TRUE(read->lifetimes().front().kind == tir::LifetimeKind::Parameter);
  ASSERT_EQ(read->lifetimes().front().source_parameter, string("value"));
}

TEST(OwnershipTest, LexicalCleanupRegistersAndDischargesObligation) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_drop.nrqba",
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  ASSERT_TRUE(verify_tir_module(*module, diag));

  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  const tir::Op* drop = find_drop(*main_fn);
  ASSERT_TRUE(drop != nullptr);
  ASSERT_EQ(drop->string_value, string("memory.deallocate"));
  ASSERT_EQ(drop->operand.ownership, ValueOwnership::Owned);

  ASSERT_EQ(main_fn->obligations().size(), 1u);
  const tir::DestructionObligation& obligation = main_fn->obligations().front();
  ASSERT_EQ(obligation.id, drop->obligation_id);
  ASSERT_EQ(obligation.release, string("memory.deallocate"));
  ASSERT_TRUE(obligation.discharged);
  ASSERT_FALSE(obligation.transferred);
  ASSERT_TRUE(print_tir(*module).find("tir.drop") != string::npos);
}

TEST(OwnershipTest, VerifierRejectsMoveFromBorrowedValue) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_bad_move.nrqba",
      "byte* function make() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return buffer;\n"
      "}\n"
      "int function main() {\n"
      "  let result = make();\n"
      "  memory.deallocate(result);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* make = find_function(*module, "make");
  ASSERT_TRUE(make != nullptr);
  for (tir::Op& op : make->operations()) {
    if (op.kind == tir::OpKind::Move) {
      op.operand.ownership = ValueOwnership::Borrowed;
      break;
    }
  }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag, "move source must be owned"));
}

TEST(OwnershipTest, VerifierRejectsUseAfterMove) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_use_after_move.nrqba",
      "byte* function make() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return buffer;\n"
      "}\n"
      "int function main() {\n"
      "  let result = make();\n"
      "  memory.deallocate(result);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* make = find_function(*module, "make");
  ASSERT_TRUE(make != nullptr);
  tir::Value moved_source;
  for (const tir::Op& op : make->operations())
    if (op.kind == tir::OpKind::Move) {
      moved_source = op.operand;
      break;
    }
  ASSERT_TRUE(moved_source.is_valid());

  tir::Op use;
  use.kind = tir::OpKind::Call;
  use.callee = "memory.deallocate";
  use.arguments.push_back(moved_source);
  use.location = moved_source.type.is_valid() ? SourceLocation{} : SourceLocation{};
  make->operations().insert(make->operations().end() - 1, use);
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag, "used after it was moved"));
  ASSERT_TRUE(has_note(verify_diag, "transferred to a new owner"));
}

TEST(OwnershipTest, VerifierRejectsBorrowMutabilityMismatch) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_bad_borrow.nrqba",
      "int function main() {\n"
      "  var value = 5;\n"
      "  let pointer = &value;\n"
      "  print(*pointer);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::Borrow) {
      op.mutable_borrow = !op.mutable_borrow;
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag, "borrow mutability disagrees"));
}

TEST(OwnershipTest, VerifierRejectsBorrowWithoutLifetimeRecord) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_missing_lifetime.nrqba",
      "int function main() {\n"
      "  var value = 5;\n"
      "  let pointer = &value;\n"
      "  print(*pointer);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  main_fn->lifetimes().clear();
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag, "no matching lifetime record"));
}

TEST(OwnershipTest, VerifierRejectsUnknownDropObligation) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_bad_drop.nrqba",
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::Drop) {
      op.obligation_id = 999;
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag, "unknown obligation"));
}

TEST(OwnershipTest, VerifierRejectsDropOfBorrowedValue) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_drop_borrow.nrqba",
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::Drop) {
      op.operand.ownership = ValueOwnership::Borrowed;
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag, "drop operand must be owning"));
}

TEST(OwnershipTest, VerifierRejectsDoubleDrop) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_double_drop.nrqba",
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (const tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::Drop) {
      main_fn->operations().insert(main_fn->operations().end() - 1, op);
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag, "discharged more than once"));
}

TEST(OwnershipTest, BorrowRecordsSelectedFieldPath) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_borrow_path.nrqba",
      "struct P {\n"
      "  int x;\n"
      "  int y;\n"
      "}\n"
      "int function main() {\n"
      "  var p = P { x: 17, y: 4 };\n"
      "  let x = &p.x;\n"
      "  p.y = 5;\n"
      "  return *x + p.y;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  ASSERT_TRUE(verify_tir_module(*module, diag));
  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  const tir::Op* borrow = first_kind(*main_fn, tir::OpKind::Borrow);
  ASSERT_TRUE(borrow != nullptr);
  ASSERT_EQ(borrow->borrow_path, string("p.x"));
  ASSERT_EQ(main_fn->lifetimes().size(), 1u);
  ASSERT_EQ(main_fn->lifetimes().front().field_path, string("p.x"));
  ASSERT_TRUE(print_tir(*module).find("path=\"p.x\"") != string::npos);
}

TEST(OwnershipTest, VerifierRejectsBorrowWithoutAddressOfOrigin) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_borrow_origin.nrqba",
      "int function main() {\n"
      "  var value = 5;\n"
      "  let pointer = &value;\n"
      "  print(*pointer);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::Borrow) {
      // Point the borrow at an ordinary value instead of the address_of result.
      op.operand = op.lhs;
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "does not originate from an address_of"));
}

TEST(OwnershipTest, VerifierRejectsDropWithoutMatchingRelease) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_drop_release.nrqba",
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::Drop) {
      op.string_value = "memory.shared_release";
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "does not match obligation release"));
}

TEST(OwnershipTest, PrinterShowsOwnershipLifetimesAndObligations) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_print.nrqba",
      "int function main() {\n"
      "  var value = 5;\n"
      "  let pointer = &value;\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  print(*pointer);\n"
      "  print(buffer[0]);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  const string printed = print_tir(*module);
  ASSERT_TRUE(printed.find("// lifetime lexical mutable") != string::npos);
  ASSERT_TRUE(printed.find("// obligation 0") != string::npos);
  ASSERT_TRUE(printed.find("state=discharged") != string::npos);
}

TEST(OwnershipTest, VerifierRejectsReturnMoveWithoutReturnConsumer) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_false_return.nrqba",
      "int function main() {\n"
      "  let allocation = memory.allocate<byte>(8);\n"
      "  let owner = allocation;\n"
      "  memory.deallocate(owner);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::Move) {
      op.transfer_kind = tir::TransferKind::Return;
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "return transfer does not feed a return"));
}

TEST(OwnershipTest, VerifierRejectsArgumentMoveWithoutCallConsumer) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_false_argument.nrqba",
      "byte* function make() {\n"
      "  let allocation = memory.allocate<byte>(8);\n"
      "  return allocation;\n"
      "}\n"
      "int function main() {\n"
      "  let value = make();\n"
      "  memory.deallocate(value);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* make = find_function(*module, "make");
  ASSERT_TRUE(make != nullptr);
  for (tir::Op& op : make->operations())
    if (op.kind == tir::OpKind::Move) {
      op.transfer_kind = tir::TransferKind::Argument;
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "argument transfer does not feed a call"));
}

TEST(OwnershipTest, VerifierRejectsObligationOwnerThatIsNotStorage) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_obligation_owner.nrqba",
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (const tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::ConstantInt) {
      // Point the obligation at an ordinary value instead of a storage slot.
      main_fn->obligations().front().owner = op.result;
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "owner is not a storage location"));
}

TEST(OwnershipTest, VerifierRejectsReservedReturnOwnership) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_reserved_return.nrqba",
      "int function main() {\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  main_fn->set_return_ownership(ValueOwnership::Weak);
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "returns a reserved ownership category"));
}

TEST(OwnershipTest, SharedOwnershipStaysTransferable) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_shared_move.nrqba",
      "int function main() {\n"
      "  let first = memory.shared_allocate<byte>(8);\n"
      "  let second = first;\n"
      "  memory.shared_release(second);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  bool shared_move = false;
  for (const tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::Move &&
        op.operand.ownership == ValueOwnership::Shared)
      shared_move = true;
  ASSERT_TRUE(shared_move);
  ASSERT_TRUE(verify_tir_module(*module, diag));
}

TEST(OwnershipTest, VerifierRejectsReturnedBorrowWithoutSourceParameter) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_bad_return_lifetime.nrqba",
      "borrow value int* function alias(borrow int* value) {\n"
      "  return value;\n"
      "}\n"
      "int function main() {\n"
      "  var number = 3;\n"
      "  let pointer = alias(&number);\n"
      "  return *pointer;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* alias = find_function(*module, "alias");
  ASSERT_TRUE(alias != nullptr);
  bool found_return = false;
  for (tir::LifetimeRecord& lifetime : alias->lifetimes())
    if (lifetime.kind == tir::LifetimeKind::Return) {
      lifetime.source_parameter = "missing";
      found_return = true;
    }
  ASSERT_TRUE(found_return);
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "return lifetime must name a borrowed parameter"));
}

TEST(OwnershipTest, ReownedSlotGetsAFreshDestructionObligation) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_reowned_slot.nrqba",
      "int function main() {\n"
      "  var buffer = memory.allocate<byte>(8);\n"
      "  let first = buffer;\n"
      "  buffer = memory.allocate<byte>(8);\n"
      "  memory.deallocate(first);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  ASSERT_EQ(main_fn->obligations().size(), 3u);
  std::size_t drops = 0;
  for (const tir::Op& op : main_fn->operations())
    if (op.kind == tir::OpKind::Drop) ++drops;
  // The moved-out binding is released by the explicit call; only the re-owned
  // slot gets an automatic Drop.
  ASSERT_EQ(drops, 1u);
  ASSERT_TRUE(verify_tir_module(*module, diag));
}

TEST(OwnershipTest, VerifierRejectsUndischargedObligation) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_undischarged.nrqba",
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  ASSERT_FALSE(main_fn->obligations().empty());
  main_fn->obligations().front().discharged = false;
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "is never discharged or transferred"));
}

TEST(OwnershipTest, VerifierRejectsUseAfterDestruction) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_use_after_drop.nrqba",
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  auto& operations = main_fn->operations();
  for (std::size_t index = 0; index < operations.size(); ++index) {
    if (operations[index].kind != tir::OpKind::Drop) continue;
    tir::Op use;
    use.kind = tir::OpKind::Print;
    use.location = operations[index].location;
    use.operand = operations[index].operand;
    operations.insert(operations.begin() + index + 1, use);
    break;
  }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "used after its destruction"));
}

TEST(OwnershipTest, VerifierRejectsDuplicateReleaseAfterDrop) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_duplicate_release.nrqba",
      "int function main() {\n"
      "  let buffer = memory.allocate<byte>(8);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  auto& operations = main_fn->operations();
  for (std::size_t index = 0; index < operations.size(); ++index) {
    if (operations[index].kind != tir::OpKind::Drop) continue;
    tir::Op duplicate;
    duplicate.kind = tir::OpKind::Call;
    duplicate.location = operations[index].location;
    duplicate.callee = operations[index].string_value;
    duplicate.arguments.push_back(operations[index].operand);
    operations.insert(operations.begin() + index + 1, duplicate);
    break;
  }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "second 'memory.deallocate' release"));
}

TEST(OwnershipTest, BorrowFlowRejectsOwnerDestroyedOnOnlyOneBranch) {
  DiagnosticEngine diagnostics;
  tir::Module module;
  auto function = std::make_unique<tir::Function>("branch_drop", Type::int_type());
  tir::Function& fn = *function;
  const Type pointer = Type::pointer_type(Type::int_type());
  const Type address = Type::pointer_type(pointer);

  tir::Value slot = fn.allocate_value(pointer);
  tir::Op alloca;
  alloca.kind = tir::OpKind::Alloca;
  alloca.slot_name = "owner";
  alloca.result = slot;
  fn.add_operation(alloca);

  tir::Value owner = fn.allocate_value(pointer, ValueOwnership::Owned);
  tir::Op load;
  load.kind = tir::OpKind::Load;
  load.lhs = slot;
  load.result = owner;
  fn.add_operation(load);

  tir::Value condition = fn.allocate_value(Type::bool_type());
  tir::Op constant;
  constant.kind = tir::OpKind::ConstantInt;
  constant.integer_value = 1;
  constant.result = condition;
  fn.add_operation(constant);

  tir::Op branch;
  branch.kind = tir::OpKind::BranchCond;
  branch.operand = condition;
  branch.true_label = 0;
  branch.false_label = 1;
  fn.add_operation(branch);

  tir::Op then_label;
  then_label.kind = tir::OpKind::Label;
  then_label.label_id = 0;
  fn.add_operation(then_label);

  fn.add_obligation(slot, pointer, "memory.deallocate", {}, SourceLocation{});
  fn.obligations().back().discharged = true;
  tir::Op drop;
  drop.kind = tir::OpKind::Drop;
  drop.operand = owner;
  drop.obligation_id = fn.obligations().back().id;
  drop.string_value = "memory.deallocate";
  fn.add_operation(drop);

  tir::Op then_to_merge;
  then_to_merge.kind = tir::OpKind::Branch;
  then_to_merge.label_id = 2;
  fn.add_operation(then_to_merge);

  tir::Op else_label;
  else_label.kind = tir::OpKind::Label;
  else_label.label_id = 1;
  fn.add_operation(else_label);
  tir::Op else_to_merge;
  else_to_merge.kind = tir::OpKind::Branch;
  else_to_merge.label_id = 2;
  fn.add_operation(else_to_merge);

  tir::Op merge_label;
  merge_label.kind = tir::OpKind::Label;
  merge_label.label_id = 2;
  fn.add_operation(merge_label);

  tir::Value address_value = fn.allocate_value(address, ValueOwnership::Borrowed);
  tir::Op address_of;
  address_of.kind = tir::OpKind::AddressOf;
  address_of.operand = slot;
  address_of.result = address_value;
  fn.add_operation(address_of);

  tir::Op borrow;
  borrow.kind = tir::OpKind::Borrow;
  borrow.operand = address_value;
  borrow.lhs = slot;
  fn.add_operation(borrow);

  tir::LifetimeRecord lifetime;
  lifetime.borrow = address_value;
  lifetime.source = slot;
  lifetime.kind = tir::LifetimeKind::Lexical;
  fn.add_lifetime(lifetime);

  tir::Op end;
  end.kind = tir::OpKind::EndBorrow;
  end.operand = address_value;
  fn.add_operation(end);

  tir::Value zero = fn.allocate_value(Type::int_type());
  tir::Op zero_op;
  zero_op.kind = tir::OpKind::ConstantInt;
  zero_op.result = zero;
  fn.add_operation(zero_op);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  ret.operand = zero;
  fn.add_operation(ret);

  module.add_function(std::move(function));
  ASSERT_FALSE(verify_tir_module(module, diagnostics));
  ASSERT_TRUE(diagnostic_contains(diagnostics,
                                  "borrows storage after its owner was moved "
                                  "or destroyed"));
}

TEST(OwnershipTest, BorrowFlowRejectsConflictingLiveBorrow) {
  DiagnosticEngine diagnostics;
  tir::Module module;
  auto function = std::make_unique<tir::Function>("conflict", Type::int_type());
  tir::Function& fn = *function;
  const Type address = Type::pointer_type(Type::int_type());

  tir::Value slot = fn.allocate_value(Type::int_type());
  tir::Op alloca;
  alloca.kind = tir::OpKind::Alloca;
  alloca.slot_name = "owner";
  alloca.result = slot;
  fn.add_operation(alloca);

  const auto make_borrow = [&](ValueOwnership ownership, bool mutable_access) {
    tir::Value address_value = fn.allocate_value(address, ownership);
    tir::Op address_of;
    address_of.kind = tir::OpKind::AddressOf;
    address_of.operand = slot;
    address_of.result = address_value;
    fn.add_operation(address_of);
    tir::Op borrow;
    borrow.kind = tir::OpKind::Borrow;
    borrow.operand = address_value;
    borrow.lhs = slot;
    borrow.mutable_borrow = mutable_access;
    fn.add_operation(borrow);
    tir::LifetimeRecord lifetime;
    lifetime.borrow = address_value;
    lifetime.source = slot;
    lifetime.kind = tir::LifetimeKind::Lexical;
    lifetime.mutable_access = mutable_access;
    fn.add_lifetime(lifetime);
    return address_value;
  };

  const tir::Value immutable = make_borrow(ValueOwnership::Borrowed, false);
  make_borrow(ValueOwnership::MutableBorrowed, true);

  // The first pointer is still used after the second borrow, so its restriction
  // is live at the conflict point.
  tir::Op print;
  print.kind = tir::OpKind::Print;
  print.operand = immutable;
  fn.add_operation(print);

  tir::Value zero = fn.allocate_value(Type::int_type());
  tir::Op zero_op;
  zero_op.kind = tir::OpKind::ConstantInt;
  zero_op.result = zero;
  fn.add_operation(zero_op);
  tir::Op ret;
  ret.kind = tir::OpKind::Return;
  ret.operand = zero;
  fn.add_operation(ret);

  module.add_function(std::move(function));
  ASSERT_FALSE(verify_tir_module(module, diagnostics));
  ASSERT_TRUE(diagnostic_contains(diagnostics,
                                  "conflicting borrow of the same owner"));
  ASSERT_TRUE(has_note(diagnostics, "conflicting borrow is active here"));
}

TEST(OwnershipTest, BorrowFlowRejectsInexactLifetimeRecord) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_lifetime_path.nrqba",
      "struct P { int x; int y; }\n"
      "int function main() {\n"
      "  var p = P { x: 1, y: 2 };\n"
      "  let x = &p.x;\n"
      "  return *x;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (tir::LifetimeRecord& lifetime : main_fn->lifetimes())
    if (lifetime.kind == tir::LifetimeKind::Lexical)
      lifetime.field_path = "p.y";
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(
      verify_diag, "borrow lifetime record does not describe"));
}

TEST(OwnershipTest, BorrowFlowRejectsReturnOfWrongBorrowedParameter) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_wrong_borrow_parameter.nrqba",
      "borrow second int* function select(borrow int* first, borrow int* second) {\n"
      "  return second;\n"
      "}\n"
      "int function main() {\n"
      "  var a = 1;\n"
      "  var b = 2;\n"
      "  let pointer = select(&a, &b);\n"
      "  return *pointer;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  tir::Function* select = find_function(*module, "select");
  ASSERT_TRUE(select != nullptr);
  select->set_return_borrow_parameter("first");
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "returns a borrow of parameter 'second' but "
                                  "declares 'first'"));
}

TEST(OwnershipTest, BorrowFlowRejectsDuplicateLexicalLifetime) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_duplicate_lifetime.nrqba",
      "int function main() {\n"
      "  var value = 5;\n"
      "  let pointer = &value;\n"
      "  return *pointer;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  for (const tir::LifetimeRecord& lifetime : main_fn->lifetimes())
    if (lifetime.kind == tir::LifetimeKind::Lexical) {
      main_fn->add_lifetime(lifetime);
      break;
    }
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "more than one lexical lifetime record"));
}

TEST(OwnershipTest, BorrowFlowRejectsMutableReturnFromImmutableBorrow) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_mutable_return.nrqba",
      "borrow mut source byte* function writable(borrow mut byte* source) {\n"
      "  return source;\n"
      "}\n"
      "int function main() {\n"
      "  var owner = memory.allocate<byte>(1);\n"
      "  var view = writable(owner);\n"
      "  return view[0] as int;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  tir::Function* writable = find_function(*module, "writable");
  ASSERT_TRUE(writable != nullptr);
  writable->set_return_ownership(ValueOwnership::Borrowed);
  DiagnosticEngine verify_diag;
  ASSERT_FALSE(verify_tir_module(*module, verify_diag));
  ASSERT_TRUE(diagnostic_contains(verify_diag,
                                  "immutable borrowed return cannot alias a "
                                  "mutable borrow"));
}

TEST(OwnershipTest, LoopBorrowDoesNotMarkScalarMoved) {
  DiagnosticEngine diag;
  auto module = lower_source(
      "ownership_loop_borrow.nrqba",
      "int function main() {\n"
      "  var value = 0;\n"
      "  var index = 0;\n"
      "  while (index < 2) {\n"
      "    let pointer = &value;\n"
      "    print(*pointer);\n"
      "    index = index + 1;\n"
      "  }\n"
      "  print(value);\n"
      "  return 42;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  ASSERT_FALSE(diag.has_errors());
  ASSERT_TRUE(verify_tir_module(*module, diag));
}
