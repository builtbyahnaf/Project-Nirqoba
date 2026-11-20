// Sigma: publication that defers discovery.
//
// A window of allocation is applied to its page from the counters the allocator
// already kept, and the object-start bits are written the first time something
// resolves into the page. These tests pin both halves of that contract: a page
// that nothing reaches is never read (a dropped graph costs no object traffic),
// and a page that is reached becomes fully discoverable again, children
// included.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nirqoba/runtime/gc.h"

namespace {

struct Node {
  Node* next;
  uint64_t value;
};

void trace_node(void* object, size_t object_bytes, nrq_gc_visit_fn visit,
                void* context) {
  (void)object_bytes;
  auto* node = static_cast<Node*>(object);
  visit(node->next, context);
}

}  // namespace

TEST(GcLazyPublicationTest, ADroppedChainIsCollectedWithoutReadingItsObjects) {
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_gc_stats before = nrq_gc_get_stats();

  // Long enough to span many pages, and linked so every object is itself a
  // traceable record rather than a leaf.
  constexpr int kNodes = 40000;
  nrq_gc_mutator_enter();
  Node* head = nullptr;
  for (int index = 0; index < kNodes; ++index) {
    auto* node = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
    ASSERT_NE(node, nullptr);
    node->next = head;
    node->value = static_cast<uint64_t>(index);
    head = node;
  }
  nrq_gc_mutator_leave();
  ASSERT_NE(head, nullptr);

  // Nothing roots the chain. Statistics are exact before the collection, which
  // is what the window counters are for.
  const nrq_gc_stats during = nrq_gc_get_stats();
  ASSERT_EQ(during.live_objects, before.live_objects + kNodes);

  nrq_gc_collect();
  const nrq_gc_stats after = nrq_gc_get_stats();
  ASSERT_EQ(after.live_objects, before.live_objects);
  ASSERT_TRUE(after.collected_bytes > before.collected_bytes);
  nrq_gc_enable();
}

TEST(GcLazyPublicationTest, AReachableChainResolvesThroughUnwrittenStartBits) {
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_gc_stats before = nrq_gc_get_stats();

  constexpr int kNodes = 20000;
  nrq_gc_mutator_enter();
  Node* head = nullptr;
  for (int index = 0; index < kNodes; ++index) {
    auto* node = static_cast<Node*>(nrq_gc_allocate(sizeof(Node), trace_node));
    ASSERT_NE(node, nullptr);
    node->next = head;
    node->value = static_cast<uint64_t>(index);
    head = node;
  }
  nrq_gc_mutator_leave();

  // The pin is the only root. Marking has to resolve every child that lives on a
  // page whose start bits were never written, so the whole chain survives and
  // stays traversable afterwards.
  ASSERT_TRUE(nrq_gc_pin(head));
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), before.live_objects + kNodes);

  int visited = 0;
  for (Node* node = head; node != nullptr; node = node->next) ++visited;
  ASSERT_EQ(visited, kNodes);

  nrq_gc_unpin(head);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), before.live_objects);
  nrq_gc_enable();
}

TEST(GcLazyPublicationTest, APageSurvivesRepeatedPublicationWindows) {
  // Thousands of refills land on a handful of pages, so one page accumulates
  // several runs. A run recorded but never materialised must not hide the next
  // one, and the counters must add up across windows.
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_gc_stats before = nrq_gc_get_stats();

  constexpr int kWaves = 4;
  constexpr int kNodes = 5000;
  for (int wave = 0; wave < kWaves; ++wave) {
    std::vector<void*> objects;
    objects.reserve(kNodes);
    nrq_gc_mutator_enter();
    for (int index = 0; index < kNodes; ++index) {
      void* object = nrq_gc_allocate(sizeof(Node), trace_node);
      ASSERT_NE(object, nullptr);
      auto* node = static_cast<Node*>(object);
      node->next = nullptr;
      node->value = static_cast<uint64_t>(index);
      objects.push_back(object);
    }
    nrq_gc_mutator_leave();
    ASSERT_EQ(nrq_gc_get_live_objects(),
              before.live_objects + static_cast<uint64_t>(kNodes));
    nrq_gc_collect();
    ASSERT_EQ(nrq_gc_get_live_objects(), before.live_objects);
  }
  nrq_gc_enable();
}
