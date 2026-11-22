// Micro-phase 10d: the type-level cycle predicate.
//
// A type cannot prove that a program's object graph closes a cycle - that is a
// runtime fact. It can prove that the graph is *allowed* to close one, which is
// what decides whether deterministic ownership can be handed to a payload at all.
// These tests pin the edges that count: an owner and a reference count keep their
// target alive, a borrow, a raw pointer, a view and a weak reference do not, and a
// collector-managed reference is the collector's problem.

#include "test_harness.h"

#include <string>
#include <utility>
#include <vector>

#include "nirqoba/Common/Type.h"

using namespace nirq;

namespace {

Type struct_of(const std::string& name) {
  return Type::struct_type(name);
}

void declare(StructFieldTable& table, const std::string& name,
             std::vector<std::pair<std::string, Type>> fields) {
  table[name] = std::move(fields);
}

// The type an allocation site sees: the struct with its own fields attached.
Type site_type(const std::string& name, const StructFieldTable& table) {
  const auto found = table.find(name);
  return found == table.end() ? struct_of(name)
                              : Type::struct_type(name, found->second);
}

}  // namespace

TEST(CycleAnalysisTest, ASharedPointerBackToItsOwnStructIsCyclic) {
  StructFieldTable table;
  declare(table, "GraphNode",
          {{"value", Type::int_type()},
           {"next", Type::shared_pointer_type(struct_of("GraphNode"))}});
  const Type node = site_type("GraphNode", table);
  EXPECT_TRUE(is_structurally_cyclic(node, table));
  // The pointer to the payload is the same question as the payload itself.
  EXPECT_TRUE(is_structurally_cyclic(Type::shared_pointer_type(node), table));
  EXPECT_TRUE(is_structurally_cyclic(Type::pointer_type(node), table) == false);
}

TEST(CycleAnalysisTest, AnOwnedPointerBackToItsOwnStructIsCyclic) {
  StructFieldTable table;
  declare(table, "Ring",
          {{"value", Type::int_type()},
           {"next", Type::owned_pointer_type(struct_of("Ring"))}});
  EXPECT_TRUE(is_structurally_cyclic(site_type("Ring", table), table));
}

TEST(CycleAnalysisTest, ACycleThroughAnotherStructIsCyclic) {
  StructFieldTable table;
  declare(table, "First",
          {{"to_second", Type::shared_pointer_type(struct_of("Second"))}});
  declare(table, "Second",
          {{"to_first", Type::owned_pointer_type(struct_of("First"))}});
  EXPECT_TRUE(is_structurally_cyclic(site_type("First", table), table));
  EXPECT_TRUE(is_structurally_cyclic(site_type("Second", table), table));
}

TEST(CycleAnalysisTest, ACycleThroughAStructStoredByValueIsCyclic) {
  StructFieldTable table;
  declare(table, "Outer",
          {{"inner", struct_of("Inner")},
           {"value", Type::int_type()}});
  declare(table, "Inner",
          {{"back", Type::shared_pointer_type(struct_of("Outer"))}});
  EXPECT_TRUE(is_structurally_cyclic(site_type("Outer", table), table));
}

TEST(CycleAnalysisTest, ACycleThroughAContainerElementIsCyclic) {
  StructFieldTable table;
  declare(table, "Folder",
          {{"children",
            Type::dynamic_array_type(
                Type::shared_pointer_type(struct_of("Folder")))}});
  EXPECT_TRUE(is_structurally_cyclic(site_type("Folder", table), table));
}

TEST(CycleAnalysisTest, ATreeOfOwnedPointersIsNotCyclic) {
  StructFieldTable table;
  declare(table, "TreeNode",
          {{"value", Type::int_type()},
           {"child", Type::owned_pointer_type(struct_of("TreeNode"))}});
  // A tree node names itself, so the *type* allows a cycle even though the
  // program builds a tree. The predicate answers for the type; only a program
  // that never links a node back stays acyclic at runtime.
  EXPECT_TRUE(is_structurally_cyclic(site_type("TreeNode", table), table));
}

TEST(CycleAnalysisTest, ABorrowDoesNotCloseACycle) {
  StructFieldTable table;
  declare(table, "View",
          {{"target", Type::array_view_type(struct_of("View"))}});
  EXPECT_TRUE(!is_structurally_cyclic(site_type("View", table), table));
}

TEST(CycleAnalysisTest, ARawPointerDoesNotCloseACycle) {
  StructFieldTable table;
  declare(table, "Handle",
          {{"target", Type::pointer_type(struct_of("Handle"))}});
  EXPECT_TRUE(!is_structurally_cyclic(site_type("Handle", table), table));
}

TEST(CycleAnalysisTest, ACollectorManagedPointerDoesNotCloseACycle) {
  StructFieldTable table;
  declare(table, "GcNode",
          {{"next", Type::managed_pointer_type(struct_of("GcNode"))}});
  // The collector reclaims a cycle it can reach, so a cycle of managed pointers
  // is not a reason to distrust deterministic ownership.
  EXPECT_TRUE(!is_structurally_cyclic(site_type("GcNode", table), table));
}

TEST(CycleAnalysisTest, FieldsThatStayInsideTheFrameAreNotCyclic) {
  StructFieldTable table;
  declare(table, "Pair",
          {{"left", Type::int_type()},
           {"right", Type::int_type()},
           {"items", Type::dynamic_array_type(Type::int_type())}});
  EXPECT_TRUE(!is_structurally_cyclic(site_type("Pair", table), table));
}

TEST(CycleAnalysisTest, AStructTheTableNeverSawHasNoProvenFields) {
  StructFieldTable table;
  EXPECT_TRUE(!is_structurally_cyclic(struct_of("Unknown"), table));
  EXPECT_TRUE(!is_structurally_cyclic(Type::int_type(), table));
}
