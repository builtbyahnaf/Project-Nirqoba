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
  // Every use stays in the loop, so the region belongs to the loop rather than to
  // the frame.
  ASSERT_TRUE(site->loop_scoped);
  ASSERT_TRUE(site->scope_begin < site->scope_end);
}

TEST(LifetimeAnalysisTest, AUseAfterTheLoopKeepsTheRegionOnTheFrame) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int function main() {\n"
      "  var total = 0;\n"
      "  var index = 0;\n"
      "  var count = 8192;\n"
      "  var kept = memory.allocate<int>(count);\n"
      "  kept[0] = 7;\n"
      "  while (index < 3) {\n"
      "    var value = memory.allocate<int>(count);\n"
      "    value[0] = index;\n"
      "    total = total + value[0];\n"
      "    index = index + 1;\n"
      "  }\n"
      "  print(total + kept[0]);\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());
  const tir::Function* function = find_function(*module, "main");
  ASSERT_TRUE(function != nullptr);

  const tir::LifetimeAnalysis analysis =
      tir::LifetimeAnalysis::analyze(*function);
  ASSERT_EQ(analysis.size(), static_cast<std::size_t>(2));
  int scoped_to_loop = 0;
  int anchored_to_frame = 0;
  for (const auto& entry : analysis.sites()) {
    if (entry.second.loop_scoped)
      ++scoped_to_loop;
    else
      ++anchored_to_frame;
  }
  // Reading `kept` after the loop is what keeps it on the frame: a region that
  // died at the loop's exit would free storage the print still reads.
  EXPECT_EQ(scoped_to_loop, 1);
  EXPECT_EQ(anchored_to_frame, 1);
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
  // A pointer is a word, and a pointer to storage of the same kind is what a
  // linked list or a tree is made of: the chunk owns every node, so bulk release
  // is the whole answer. The pointee's shape is what decides.
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::pointer_type(Type::int_type())));
  ASSERT_TRUE(tir::is_arena_eligible_payload(
      Type::struct_type("Holder", {{"slot", Type::pointer_type(Type::int_type())}})));
  // `struct Node { int value; owned ptr Node next; }` as the type system stores
  // it: the field names a struct that names itself, and the classifier has to
  // assume the node in progress is eligible instead of recursing forever.
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::struct_type(
      "Node", {{"value", Type::int_type()},
               {"next", Type::owned_pointer_type(Type::struct_type("Node"))}})));
  // Owned text and a dynamic container are hybrid: they own storage, and the
  // arena records a field teardown when it allocates, so the chunk's release
  // reclaims the field's buffer before it reclaims the chunk.
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::owned_string_type()));
  ASSERT_TRUE(tir::is_arena_eligible_payload(
      Type::dynamic_array_type(Type::int_type())));
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::struct_type(
      "AstNode", {{"name", Type::owned_string_type()},
                  {"next", Type::pointer_type(Type::struct_type("AstNode"))}})));
  // An owned array and a file resource are the same bargain: the registry names
  // the buffer's release, and the file's handle close, so a payload holding them
  // is released field by field before the chunk goes back.
  ASSERT_TRUE(tir::is_arena_eligible_payload(
      Type::owned_array_type(Type::int_type())));
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::struct_type(
      "Columns", {{"rows", Type::owned_array_type(Type::int_type())}})));
  ASSERT_TRUE(tir::is_arena_eligible_payload(Type::struct_type(
      "Handle", {{"file", Type::owned_pointer_type(Type::u8_type(), "file")}})));
  // A bare `string` is a view of someone else's buffer, so it stays out.
  ASSERT_FALSE(tir::is_arena_eligible_payload(Type::string_type()));
  ASSERT_FALSE(tir::is_arena_eligible_payload(
      Type::shared_pointer_type(Type::int_type())));
  ASSERT_FALSE(tir::is_arena_eligible_payload(
      Type::managed_pointer_type(Type::int_type())));
  // A domain with no teardown kind stays out until the registry grows one.
  ASSERT_FALSE(tir::is_arena_eligible_payload(Type::struct_type(
      "Socket",
      {{"handle", Type::owned_pointer_type(Type::u8_type(), "socket")}})));
  ASSERT_FALSE(tir::is_arena_eligible_payload(Type::struct_type(
      "Nodes", {{"next", Type::shared_pointer_type(Type::int_type())}})));
}

// Phase 9c: one callee, two kinds of call site.
//
// The bounded caller keeps the arena it created; the escaping caller passes null
// and the callee falls back. The plan therefore has to record both sides of the
// same callee, and neither caller's decision may erase the other's.
TEST(LifetimeAnalysisTest, BoundedAndEscapingCallSitesShareOnePromotedCallee) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int* function alloc_and_return() {\n"
      "  var node = memory.allocate<int>(8);\n"
      "  node[0] = 42;\n"
      "  return node;\n"
      "}\n"
      "void function keep(int* pointer) {\n"
      "  print(pointer[0]);\n"
      "  return;\n"
      "}\n"
      "int function main() {\n"
      "  var bounded = alloc_and_return();\n"
      "  print(bounded[0]);\n"
      "  var escaped = alloc_and_return();\n"
      "  keep(escaped);\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());

  const tir::CallSiteArenaPlan plan = tir::analyze_call_site_arenas(*module);
  // One bounded site is enough: the callee gains the handle even though the other
  // site lets the storage out, and it is only partially promoted.
  ASSERT_TRUE(plan.promoted("alloc_and_return"));
  ASSERT_TRUE(plan.partial("alloc_and_return"));
  const auto sites = plan.call_sites.find("main");
  ASSERT_TRUE(sites != plan.call_sites.end());
  ASSERT_EQ(sites->second.size(), static_cast<std::size_t>(1));
  ASSERT_EQ(sites->second[0].callee, std::string("alloc_and_return"));
  // The escaped site is recorded so the report can name the null handle it passes.
  const auto escaped = plan.escaping_call_results.find("main");
  ASSERT_TRUE(escaped != plan.escaping_call_results.end());
  ASSERT_EQ(escaped->second.size(), static_cast<std::size_t>(1));
  ASSERT_TRUE(escaped->second.count(sites->second[0].result) == 0);
}

// With no bounded caller at all there is no handle to pass, so the signature stays
// as it was: a callee only gains a nullable argument when some call site supplies
// one, and nothing here does.
TEST(LifetimeAnalysisTest, EscapingOnlyCallerLeavesTheSignatureAlone) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int* function alloc_and_return() {\n"
      "  var node = memory.allocate<int>(8);\n"
      "  node[0] = 42;\n"
      "  return node;\n"
      "}\n"
      "int* function pass_through() {\n"
      "  return alloc_and_return();\n"
      "}\n"
      "int function main() {\n"
      "  var m = pass_through();\n"
      "  print(m[0]);\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());

  const tir::CallSiteArenaPlan plan = tir::analyze_call_site_arenas(*module);
  ASSERT_FALSE(plan.promoted("alloc_and_return"));
  ASSERT_FALSE(plan.partial("alloc_and_return"));
  ASSERT_TRUE(plan.call_sites.empty());
  ASSERT_TRUE(plan.escaping_call_results.empty());
}

// Phase 9d: a bounded call site inside a loop asks for a loop-scoped arena.
//
// Every use of the returned storage is in the loop body, so the arena can be
// created on the loop's entry edge and released on its exit instead of living
// until the frame returns.
TEST(LifetimeAnalysisTest, BoundedCallSiteInsideALoopIsLoopScoped) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int* function alloc_and_return() {\n"
      "  var node = memory.allocate<int>(8);\n"
      "  node[0] = 42;\n"
      "  return node;\n"
      "}\n"
      "int function main() {\n"
      "  var i = 0;\n"
      "  while (i < 3) {\n"
      "    var n = alloc_and_return();\n"
      "    print(n[0]);\n"
      "    i = i + 1;\n"
      "  }\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());

  const tir::CallSiteArenaPlan plan = tir::analyze_call_site_arenas(*module);
  ASSERT_TRUE(plan.promoted("alloc_and_return"));
  const auto sites = plan.call_sites.find("main");
  ASSERT_TRUE(sites != plan.call_sites.end());
  ASSERT_EQ(sites->second.size(), static_cast<std::size_t>(1));
  // The span runs from the loop header to the back edge that closes it.
  ASSERT_TRUE(sites->second[0].loop_scoped);
  ASSERT_TRUE(sites->second[0].scope_begin < sites->second[0].scope_end);
}

// The same call outside a loop keeps the frame's arena: there is no exit edge to
// release at, so the storage lives as long as the activation does.
TEST(LifetimeAnalysisTest, BoundedCallSiteOutsideALoopIsFrameScoped) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int* function alloc_and_return() {\n"
      "  var node = memory.allocate<int>(8);\n"
      "  node[0] = 42;\n"
      "  return node;\n"
      "}\n"
      "int function main() {\n"
      "  var n = alloc_and_return();\n"
      "  print(n[0]);\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());

  const tir::CallSiteArenaPlan plan = tir::analyze_call_site_arenas(*module);
  const auto sites = plan.call_sites.find("main");
  ASSERT_TRUE(sites != plan.call_sites.end());
  ASSERT_EQ(sites->second.size(), static_cast<std::size_t>(1));
  ASSERT_FALSE(sites->second[0].loop_scoped);
}

// Phase 9e: a direct recursive call forwards the activation's own handle.
//
// The self-call's storage is the storage the function returns, so the chain it
// builds belongs to whichever arena the outermost caller created. Passing null
// there would put every level on the general allocator; passing the handle this
// activation was given keeps the whole chain in one chunk.
TEST(LifetimeAnalysisTest, SelfRecursiveCallThatReturnsForwards) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int* function build_list(int depth) {\n"
      "  var node = memory.allocate<int>(8);\n"
      "  node[0] = depth;\n"
      "  if (depth > 0) {\n"
      "    return build_list(depth - 1);\n"
      "  }\n"
      "  return node;\n"
      "}\n"
      "int function main() {\n"
      "  var root = build_list(5);\n"
      "  print(root[0]);\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());

  const tir::CallSiteArenaPlan plan = tir::analyze_call_site_arenas(*module);
  ASSERT_TRUE(plan.promoted("build_list"));
  const auto forwarded = plan.forwarded_call_results.find("build_list");
  ASSERT_TRUE(forwarded != plan.forwarded_call_results.end());
  ASSERT_EQ(forwarded->second.size(), static_cast<std::size_t>(1));
  ASSERT_TRUE(plan.forwards("build_list", *forwarded->second.begin()));
  // A forwarding site is not an escaped one: it never asks for a null handle.
  const auto escaped = plan.escaping_call_results.find("build_list");
  ASSERT_TRUE(escaped == plan.escaping_call_results.end());
}

// Forwarding needs a handle to forward. With every caller letting the storage out
// there is no arena, so the self-call keeps passing null and the signature stays
// as it was.
TEST(LifetimeAnalysisTest, SelfRecursiveForwardingNeedsAHandle) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "int* function build_list(int depth) {\n"
      "  var node = memory.allocate<int>(8);\n"
      "  node[0] = depth;\n"
      "  if (depth > 0) {\n"
      "    return build_list(depth - 1);\n"
      "  }\n"
      "  return node;\n"
      "}\n"
      "void function keep(int* pointer) {\n"
      "  print(pointer[0]);\n"
      "  return;\n"
      "}\n"
      "int function main() {\n"
      "  var root = build_list(3);\n"
      "  keep(root);\n"
      "  return 0;\n"
      "}",
      diag);
  ASSERT_FALSE(diag.has_errors());

  const tir::CallSiteArenaPlan plan = tir::analyze_call_site_arenas(*module);
  ASSERT_FALSE(plan.promoted("build_list"));
  ASSERT_TRUE(plan.forwarded_call_results.empty());
  ASSERT_TRUE(plan.escaping_call_results.empty());
}

// Micro-phase 10d: the type-level fact travels with the classification.
//
// A payload's shape is a fact about the program's struct catalog, not about the
// operation stream the shape analysis walks: the analysis sees a pointer, and only
// the catalog knows what the pointee owns. Both answers below come from the same
// site, and the flag is exactly as present as the catalog it was asked with.
TEST(LifetimeAnalysisTest, APayloadThatCanCloseACycleIsFlaggedAtItsSite) {
  DiagnosticEngine diag;
  auto module = build_tir(
      "struct GraphNode {\n"
      "  int value;\n"
      "  shared ptr GraphNode next;\n"
      "}\n"
      "int function main() {\n"
      "  var kept = memory.dynamic_array<GraphNode*>();\n"
      "  var node = memory.allocate<GraphNode>(32);\n"
      "  *node = GraphNode { value: 7, next: nullptr };\n"
      "  memory.array_append(kept, node);\n"
      "  return 0;\n"
      "}\n",
      diag);
  ASSERT_TRUE(module != nullptr);
  const tir::Function* main_fn = find_function(*module, "main");
  ASSERT_TRUE(main_fn != nullptr);
  const int id = allocation_result(*main_fn);
  ASSERT_TRUE(id >= 0);

  // Without a catalog the site is classified exactly as before, and the type-level
  // question is simply unanswered.
  const tir::LifetimeAnalysis blind = tir::LifetimeAnalysis::analyze(*main_fn);
  ASSERT_TRUE(blind.site(id) != nullptr);
  EXPECT_TRUE(!blind.site(id)->cyclic_payload);

  StructFieldTable table;
  table["GraphNode"] = {
      {"value", Type::int_type()},
      {"next", Type::shared_pointer_type(Type::struct_type("GraphNode"))}};
  const tir::LifetimeAnalysis aware =
      tir::LifetimeAnalysis::analyze(*main_fn, &table);
  ASSERT_TRUE(aware.site(id) != nullptr);
  EXPECT_TRUE(aware.site(id)->cyclic_payload);
  // The escape fact itself does not depend on the catalog.
  EXPECT_TRUE(aware.site(id)->reason == blind.site(id)->reason);
  EXPECT_TRUE(aware.site(id)->lifetime == tir::AllocationLifetime::Escaping);
}

// The catalog the classifier reads is the one the checker publishes, so the
// allocation gate, the report and this analysis cannot disagree about a type.
TEST(LifetimeAnalysisTest, TheCheckerPublishesTheStructCatalogAndItsCyclicTypes) {
  DiagnosticEngine diag;
  SourceFile src("catalog.nrqba",
                 "struct GraphNode {\n"
                 "  int value;\n"
                 "  shared ptr GraphNode next;\n"
                 "}\n"
                 "struct TreeNode {\n"
                 "  int value;\n"
                 "  owned ptr TreeNode child;\n"
                 "}\n"
                 "int function main() { return 0; }\n");
  Lexer lexer(src, diag);
  Parser parser(lexer.tokenize(), diag);
  auto program = parser.parse_program();
  ASSERT_TRUE(program != nullptr);
  TypeChecker checker(diag);
  ASSERT_TRUE(checker.check(*program));

  EXPECT_TRUE(checker.struct_field_table().count("GraphNode") == 1);
  EXPECT_TRUE(checker.struct_field_table().count("TreeNode") == 1);
  // A reference count can close a loop; a chain of unique owners cannot.
  EXPECT_TRUE(checker.cyclic_structs().count("GraphNode") == 1);
  EXPECT_TRUE(checker.cyclic_structs().count("TreeNode") == 0);
}
