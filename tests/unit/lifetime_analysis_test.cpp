// Phase Psi-1: the allocation lifetime classifier.
//
// The classifier answers one question per explicit allocation site: can the
// pointer leave the activation that created it? Everything automated strategy
// selection does afterwards depends on that answer, so these tests pin the
// answer itself - including the reasons, because "escaping" without a reason
// cannot be reviewed.

#include "test_harness.h"

#include <cstdint>
#include <string>
#include <vector>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Lexer.h"
#include "nirqoba/Frontend/Parser.h"
#include "nirqoba/Frontend/SourceFile.h"
#include "nirqoba/Sema/TypeChecker.h"
#include "nirqoba/TIR/Builder.h"
#include "nirqoba/TIR/LifetimeAnalysis.h"

using namespace nirq;

namespace {

std::unique_ptr<tir::Module> build_tir(const std::string& code,
                                       DiagnosticEngine& diag) {
  SourceFile src("lifetime_analysis.nrqba", code);
  Lexer lexer(src, diag);
  Parser parser(lexer.tokenize(), diag);
  auto program = parser.parse_program();
  EXPECT_TRUE(program != nullptr);
  TypeChecker checker(diag);
  EXPECT_TRUE(checker.check(*program));
  TirBuilder builder(diag);
  return builder.lower(*program);
}

const tir::Function* find_function(const tir::Module& module,
                                   const std::string& name) {
  for (const auto& function : module.functions())
    if (function->name() == name) return function.get();
  return nullptr;
}

// The single allocation site of a function, or -1 when it has none.
int allocation_result(const tir::Function& function) {
  for (const tir::Op& op : function.operations())
    if (tir::is_allocation_site(op)) return op.result.id;
  return -1;
}

std::string describe(const tir::AllocationSite& site) {
  return std::string(tir::lifetime_name(site.lifetime)) + "/" +
         std::string(tir::escape_reason_name(site.reason));
}

}  // namespace

TEST(LifetimeAnalysisTest, StorageUsedInsideOneFunctionIsBounded) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int function main() {\n"
      "  var count = 4;\n"
      "  var value = memory.allocate<int>(count * 8);\n"
      "  value[0] = 7;\n"
      "  return value[0];\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* function = find_function(*module, "main");
  ASSERT_TRUE(function != nullptr);

  const tir::LifetimeAnalysis analysis =
      tir::LifetimeAnalysis::analyze(*function);
  const int site_id = allocation_result(*function);
  ASSERT_TRUE(site_id >= 0);
  const tir::AllocationSite* site = analysis.site(site_id);
  ASSERT_TRUE(site != nullptr);
  ASSERT_EQ(describe(*site), std::string("bounded/none"));
  ASSERT_FALSE(site->manually_released);
  ASSERT_TRUE(site->used_locally);
  // The result, the binding slot, the values loaded back and the element
  // pointer all name the same storage.
  ASSERT_TRUE(site->aliases.size() >= 4);
  ASSERT_TRUE(site->aliases.count(site_id) != 0);
}

TEST(LifetimeAnalysisTest, ReturningThePointerEscapes) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int* function make(int count) {\n"
      "  var value = memory.allocate<int>(count * 8);\n"
      "  value[0] = 1;\n"
      "  return value;\n"
      "}\n"
      "int function main() { return 0; }",
      diag);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* function = find_function(*module, "make");
  ASSERT_TRUE(function != nullptr);

  const tir::LifetimeAnalysis analysis =
      tir::LifetimeAnalysis::analyze(*function);
  const tir::AllocationSite* site = analysis.site(allocation_result(*function));
  ASSERT_TRUE(site != nullptr);
  // The return moves the pointer first; the move does not hide the escape.
  ASSERT_EQ(describe(*site), std::string("escaping/returned"));
}

TEST(LifetimeAnalysisTest, PassingThePointerToACalleeEscapes) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "void function sink(int* pointer) {\n"
      "  print(pointer[0]);\n"
      "  return;\n"
      "}\n"
      "int function main() {\n"
      "  var count = 4;\n"
      "  var value = memory.allocate<int>(count * 8);\n"
      "  value[0] = 2;\n"
      "  sink(value);\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* function = find_function(*module, "main");
  ASSERT_TRUE(function != nullptr);

  const tir::LifetimeAnalysis analysis =
      tir::LifetimeAnalysis::analyze(*function);
  const tir::AllocationSite* site = analysis.site(allocation_result(*function));
  ASSERT_TRUE(site != nullptr);
  // Nothing in TIR promises the callee keeps no copy, so the storage may outlive
  // the frame and the collector has to own it.
  ASSERT_EQ(describe(*site), std::string("escaping/passed-to-callee"));
}

TEST(LifetimeAnalysisTest, AHandWrittenReleaseIsRecordedNotAnEscape) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int function main() {\n"
      "  var count = 4;\n"
      "  var value = memory.allocate<int>(count * 8);\n"
      "  value[0] = 3;\n"
      "  print(value[0]);\n"
      "  memory.deallocate(value);\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* function = find_function(*module, "main");
  ASSERT_TRUE(function != nullptr);

  const tir::LifetimeAnalysis analysis =
      tir::LifetimeAnalysis::analyze(*function);
  const tir::AllocationSite* site = analysis.site(allocation_result(*function));
  ASSERT_TRUE(site != nullptr);
  ASSERT_EQ(describe(*site), std::string("bounded/none"));
  ASSERT_TRUE(site->manually_released);
  ASSERT_TRUE(site->used_locally);
}

TEST(LifetimeAnalysisTest, StorageNobodyTouchesIsNotALocalWorkingSet) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int function take(int size) {\n"
      "  var buffer = memory.allocate<byte>(size);\n"
      "  return size;\n"
      "}\n"
      "int function main() { return 0; }",
      diag);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* function = find_function(*module, "take");
  ASSERT_TRUE(function != nullptr);

  const tir::LifetimeAnalysis analysis =
      tir::LifetimeAnalysis::analyze(*function);
  const tir::AllocationSite* site = analysis.site(allocation_result(*function));
  ASSERT_TRUE(site != nullptr);
  ASSERT_EQ(describe(*site), std::string("bounded/none"));
  // Bounded, but never dereferenced: routing it would save nothing.
  ASSERT_FALSE(site->used_locally);
}

TEST(LifetimeAnalysisTest, ARepeatedSiteInsideALoopIsPoolEligible) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int function main() {\n"
      "  var total = 0;\n"
      "  var index = 0;\n"
      "  while (index < 3) {\n"
      "    var value = memory.allocate<int>(8192);\n"
      "    value[0] = index;\n"
      "    total = total + value[0];\n"
      "    index = index + 1;\n"
      "  }\n"
      "  return total;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* function = find_function(*module, "main");
  ASSERT_TRUE(function != nullptr);

  const tir::LifetimeAnalysis analysis =
      tir::LifetimeAnalysis::analyze(*function);
  const tir::AllocationSite* site = analysis.site(allocation_result(*function));
  ASSERT_TRUE(site != nullptr);
  ASSERT_EQ(describe(*site), std::string("bounded/none"));
  // One site, but it runs once per iteration: the pooling shape.
  ASSERT_TRUE(site->in_loop);
  ASSERT_TRUE(site->constant_payload);
  ASSERT_EQ(site->payload_bytes, static_cast<std::uint64_t>(8192));
  ASSERT_TRUE(site->pool_eligible);
}

TEST(LifetimeAnalysisTest, FlatSitesPoolOnlyFromTheThreshold) {
  auto analyze_sites = [](int count) {
    std::string code = "int function main() {\n  var total = 0;\n";
    for (int index = 0; index < count; ++index) {
      code += "  var slot" + std::to_string(index) +
              " = memory.allocate<int>(8192);\n";
      code += "  slot" + std::to_string(index) + "[0] = " +
              std::to_string(index) + ";\n";
      code += "  total = total + slot" + std::to_string(index) + "[0];\n";
    }
    code += "  return total;\n}\n";
    DiagnosticEngine diag;
    auto module = build_tir(code, diag);
    EXPECT_FALSE(diag.has_errors());
    const tir::Function* function = module ? find_function(*module, "main") : nullptr;
    if (function == nullptr) return std::vector<tir::AllocationSite>{};
    const tir::LifetimeAnalysis analysis =
        tir::LifetimeAnalysis::analyze(*function);
    std::vector<tir::AllocationSite> sites;
    for (const tir::Op& op : function->operations())
      if (tir::is_allocation_site(op)) sites.push_back(*analysis.site(op.result.id));
    return sites;
  };

  // Below the threshold each site keeps the cheaper answer it already had.
  const auto below = analyze_sites(3);
  ASSERT_EQ(below.size(), static_cast<std::size_t>(3));
  for (const tir::AllocationSite& site : below) {
    ASSERT_FALSE(site.in_loop);
    ASSERT_EQ(site.identical_sites, static_cast<std::size_t>(3));
    ASSERT_FALSE(site.pool_eligible);
  }

  // At the threshold the group is worth one shared chunk.
  const auto at_threshold = analyze_sites(static_cast<int>(tir::kPoolSiteThreshold));
  ASSERT_EQ(at_threshold.size(), tir::kPoolSiteThreshold);
  for (const tir::AllocationSite& site : at_threshold) {
    ASSERT_EQ(site.identical_sites, tir::kPoolSiteThreshold);
    ASSERT_TRUE(site.pool_eligible);
  }
}

TEST(LifetimeAnalysisTest, EscapingRepeatsAreNotPoolEligible) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "void function sink(int* pointer) {\n"
      "  print(pointer[0]);\n"
      "  return;\n"
      "}\n"
      "int function main() {\n"
      "  var index = 0;\n"
      "  while (index < 3) {\n"
      "    var value = memory.allocate<int>(8192);\n"
      "    value[0] = index;\n"
      "    sink(value);\n"
      "    index = index + 1;\n"
      "  }\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* function = find_function(*module, "main");
  ASSERT_TRUE(function != nullptr);

  const tir::LifetimeAnalysis analysis =
      tir::LifetimeAnalysis::analyze(*function);
  const tir::AllocationSite* site = analysis.site(allocation_result(*function));
  ASSERT_TRUE(site != nullptr);
  // Repeated and constant, but the pointer leaves: a pool the frame destroys
  // would free storage that outlives the frame, so the site keeps the allocator.
  ASSERT_TRUE(site->in_loop);
  ASSERT_EQ(describe(*site), std::string("escaping/passed-to-callee"));
  ASSERT_FALSE(site->pool_eligible);
}

TEST(LifetimeAnalysisTest, ArenaPayloadsArePlainData) {
  // Scalars and aggregates of scalars can live in arena storage: nothing has to
  // be traced and nothing has to be destroyed.
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::int_type()));
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::u8_type()));
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::f64_type()));
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::array_type(Type::int_type(), 4)));
  ASSERT_TRUE(tir::is_arena_eligible_payload(
      Type::struct_type("Pair", {{"left", Type::int_type()},
                                 {"right", Type::u64_type()}})));
  // Storage with an owner or a collector reference cannot: an arena frees in
  // bulk and the collector cannot trace arena memory.
  ASSERT_FALSE(tir::is_arena_eligible_payload(Type::pointer_type(Type::int_type())));
  ASSERT_FALSE(tir::is_arena_eligible_payload(Type::string_type()));
  ASSERT_FALSE(tir::is_arena_eligible_payload(Type::owned_string_type()));
  ASSERT_FALSE(tir::is_arena_eligible_payload(
      Type::dynamic_array_type(Type::int_type())));
  ASSERT_FALSE(tir::is_arena_eligible_payload(
      Type::struct_type("Holder", {{"slot", Type::pointer_type(Type::int_type())}})));
}
