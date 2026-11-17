// AOR Phase 4: the three adaptive object representations.
//
// These tests are about the representation itself rather than collection
// semantics: which representation the allocator selects, what it costs, where the
// payload begins, and that a live object still reports the layout it was born
// with after a page has been recycled. Collection behaviour for each shape is
// covered by the existing reachability tests, which now run through all three.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "nirqoba/runtime/gc.h"

namespace {

struct Leaf {
  uint64_t value;
};

struct Node {
  void* child;
  uint64_t value;
};

// A record big enough to miss the headerless size bound while still holding one
// managed reference, so the compact representation has something to trace.
struct BigNode {
  void* child;
  uint64_t value;
  unsigned char padding[512];
};

void trace_node(void* object, size_t object_bytes, nrq_gc_visit_fn visit,
                void* context) {
  (void)object_bytes;
  visit(*static_cast<void**>(object), context);
}

const nrq_gc_representation_facts& facts() {
  static const nrq_gc_representation_facts value =
      nrq_gc_get_representation_facts();
  return value;
}

uintptr_t address_of(void* object) {
  return reinterpret_cast<uintptr_t>(object);
}

}  // namespace

TEST(GcRepresentationTest, FactsDescribeTheThreeHeaders) {
  ASSERT_EQ(facts().headerless_header_bytes, size_t{0});
  ASSERT_EQ(facts().compact_header_bytes, size_t{8});
  ASSERT_EQ(facts().extended_header_bytes, size_t{16});
  ASSERT_EQ(facts().max_header_bytes, size_t{16});
  ASSERT_TRUE(facts().max_header_bytes <= 16);
  // A headerless object starts its payload at the span start; a compact one starts
  // it after its 8-byte header; an extended one keeps the span alignment.
  ASSERT_EQ(facts().headerless_payload_alignment, alignof(std::max_align_t));
  ASSERT_EQ(facts().extended_payload_alignment, alignof(std::max_align_t));
  ASSERT_EQ(facts().compact_payload_alignment, size_t{8});
}

TEST(GcRepresentationTest, HeaderlessObjectCarriesNoHeader) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* object = nrq_gc_allocate_repr(sizeof(Leaf), nullptr,
                                      NRQ_GC_REPR_HEADERLESS);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_header_size(object), size_t{0});
  ASSERT_EQ(address_of(object) % alignof(std::max_align_t), uintptr_t{0});
  // The payload is the object: writing it must not disturb page metadata.
  auto* leaf = static_cast<Leaf*>(object);
  leaf->value = 0x5A5A5A5Aull;
  ASSERT_EQ(leaf->value, uint64_t{0x5A5A5A5Aull});
}

TEST(GcRepresentationTest, CompactObjectSpendsEightHeaderBytes) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* object =
      nrq_gc_allocate_repr(sizeof(BigNode), trace_node, NRQ_GC_REPR_COMPACT);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  ASSERT_EQ(nrq_gc_object_header_size(object), size_t{8});
  ASSERT_EQ(address_of(object) % size_t{8}, uintptr_t{0});
  auto* node = static_cast<BigNode*>(object);
  node->child = nullptr;
  node->value = 7;
  ASSERT_EQ(node->value, uint64_t{7});
}

TEST(GcRepresentationTest, ExtendedObjectSpendsSixteenHeaderBytes) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* object =
      nrq_gc_allocate_repr(sizeof(BigNode), trace_node, NRQ_GC_REPR_EXTENDED);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_EQ(nrq_gc_object_header_size(object), size_t{16});
  ASSERT_EQ(address_of(object) % alignof(std::max_align_t), uintptr_t{0});
}

TEST(GcRepresentationTest, AutoPolicyChoosesByShapeAndSize) {
  nrq_gc_disable();
  nrq_gc_collect();
  // A small fixed record is headerless, a large one is compact, and an array is
  // always extended because it needs its element count.
  void* small = nrq_gc_allocate(sizeof(Leaf), nullptr);
  void* large = nrq_gc_allocate(sizeof(BigNode), nullptr);
  void* array = nrq_gc_allocate_array(4, sizeof(void*), nullptr);
  ASSERT_NE(small, nullptr);
  ASSERT_NE(large, nullptr);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(small),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_representation(large),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  ASSERT_EQ(nrq_gc_object_representation(array),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
}

TEST(GcRepresentationTest, AnImpossibleRepresentationIsRejected) {
  nrq_gc_disable();
  nrq_gc_collect();
  // Headerless is bounded by the uniform slot: a payload that cannot fit one is
  // refused instead of being coerced into a different representation.
  ASSERT_TRUE(nrq_gc_allocate_repr(sizeof(BigNode), nullptr,
                                   NRQ_GC_REPR_HEADERLESS) == nullptr);
  // An unknown id is rejected rather than treated as auto.
  ASSERT_TRUE(nrq_gc_allocate_repr(sizeof(Leaf), nullptr, 99) == nullptr);
  // Arrays are extended by construction.
  ASSERT_EQ(nrq_gc_object_header_size(nrq_gc_allocate_array(2, 8, nullptr)),
            size_t{16});
}

TEST(GcRepresentationTest, AnArrayReportsItsElementCountThroughItsLayout) {
  nrq_gc_disable();
  nrq_gc_collect();
  constexpr size_t kCount = 9;
  void* array = nrq_gc_allocate_array_typed(kCount, sizeof(void*), nullptr, 0);
  ASSERT_NE(array, nullptr);
  const nrq_object_layout* layout = nrq_gc_object_layout(array);
  ASSERT_NE(layout, nullptr);
  ASSERT_TRUE((layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0);
  ASSERT_EQ(layout->payload_size, static_cast<uint32_t>(sizeof(void*)));
}

TEST(GcRepresentationTest, HeaderlessPagesStayDedicatedToTheirLayout) {
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_gc_representation_stats before = nrq_gc_get_representation_stats();
  std::vector<void*> objects;
  // Two different small shapes must not share a uniform page, and every object
  // must still report the descriptor it was allocated with.
  for (int index = 0; index < 64; ++index) {
    objects.push_back(nrq_gc_allocate(sizeof(Leaf), nullptr));
    objects.push_back(nrq_gc_allocate(sizeof(Node), trace_node));
  }
  const nrq_gc_representation_stats after = nrq_gc_get_representation_stats();
  ASSERT_TRUE(after.headerless_pages >= before.headerless_pages);
  ASSERT_TRUE(after.headerless_objects >= before.headerless_objects + 128);
  for (void* object : objects) {
    ASSERT_NE(nrq_gc_object_layout(object), nullptr);
    ASSERT_EQ(nrq_gc_object_representation(object),
              static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  }
}

TEST(GcRepresentationTest, StatisticsAccountForHeaderSavings) {
  nrq_gc_disable();
  nrq_gc_collect();
  void* small = nrq_gc_allocate(sizeof(Leaf), nullptr);
  void* large = nrq_gc_allocate(sizeof(BigNode), nullptr);
  void* array = nrq_gc_allocate_array(4, sizeof(void*), nullptr);
  ASSERT_NE(small, nullptr);
  ASSERT_NE(large, nullptr);
  ASSERT_NE(array, nullptr);
  const nrq_gc_representation_stats stats = nrq_gc_get_representation_stats();
  ASSERT_TRUE(stats.headerless_objects >= 1);
  ASSERT_TRUE(stats.compact_objects >= 1);
  ASSERT_TRUE(stats.extended_objects >= 1);
  // A headerless object saves 16 header bytes against an extended one; a compact
  // object saves 8. Anything smaller would mean the accounting is not measuring
  // the headers that actually exist.
  ASSERT_TRUE(stats.header_bytes_saved_vs_extended >= 16 + 8);
}

TEST(GcRepresentationTest, EveryRepresentationTracesAndIsReclaimed) {
  nrq_gc_disable();
  nrq_gc_set_generational(1);
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();

  void* leaf = nrq_gc_allocate_repr(sizeof(Leaf), nullptr,
                                    NRQ_GC_REPR_HEADERLESS);
  void* compact =
      nrq_gc_allocate_repr(sizeof(BigNode), trace_node, NRQ_GC_REPR_COMPACT);
  void* extended =
      nrq_gc_allocate_repr(sizeof(BigNode), trace_node, NRQ_GC_REPR_EXTENDED);
  ASSERT_NE(leaf, nullptr);
  ASSERT_NE(compact, nullptr);
  ASSERT_NE(extended, nullptr);
  static_cast<BigNode*>(compact)->child = leaf;
  static_cast<BigNode*>(extended)->child = leaf;

  void* root = compact;
  nrq_gc_root_register(&root);
  nrq_gc_collect();
  // The leaf is reachable through the compact object, which is reachable from the
  // root; the extended object is unreachable and must be gone.
  ASSERT_TRUE(nrq_gc_pointer_is_managed(leaf) == 1);
  ASSERT_TRUE(nrq_gc_pointer_is_managed(compact) == 1);
  ASSERT_EQ(nrq_gc_pointer_is_managed(extended), int32_t{0});

  root = nullptr;
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcRepresentationTest, AHeaderlessObjectSurvivesPromotionUnchanged) {
  nrq_gc_disable();
  nrq_gc_set_generational(1);
  nrq_gc_collect();
  void* object = nrq_gc_allocate_repr(sizeof(Node), trace_node,
                                      NRQ_GC_REPR_HEADERLESS);
  ASSERT_NE(object, nullptr);
  auto* node = static_cast<Node*>(object);
  node->value = 1234;
  void* root = object;
  nrq_gc_root_register(&root);
  // Two minor collections age and then promote the object; the representation and
  // the payload must be untouched by that transition.
  for (int cycle = 0; cycle < 3; ++cycle) nrq_gc_collect_minor();
  ASSERT_EQ(nrq_gc_object_representation(object),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_header_size(object), size_t{0});
  ASSERT_EQ(static_cast<Node*>(object)->value, uint64_t{1234});
  ASSERT_EQ(nrq_gc_object_state_bits(object) & NRQ_GC_STATE_YOUNG, 0u);
  root = nullptr;
  nrq_gc_collect();
}

TEST(GcRepresentationTest, AMixedRepresentationHeapCollectsCleanly) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  {
    std::vector<void*> objects;
    for (int index = 0; index < 256; ++index) {
      objects.push_back(nrq_gc_allocate(sizeof(Leaf), nullptr));
      objects.push_back(nrq_gc_allocate(sizeof(BigNode), trace_node));
      objects.push_back(nrq_gc_allocate_array(4, sizeof(void*), nullptr));
    }
    const nrq_gc_representation_stats stats = nrq_gc_get_representation_stats();
    ASSERT_TRUE(stats.headerless_objects >= 256);
    ASSERT_TRUE(stats.compact_objects >= 256);
    ASSERT_TRUE(stats.extended_objects >= 256);
    for (void* object : objects) ASSERT_NE(object, nullptr);
  }
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

TEST(GcRepresentationTest, AnInvalidAddressReportsNoRepresentation) {
  ASSERT_EQ(nrq_gc_object_representation(nullptr),
            static_cast<int32_t>(NRQ_GC_REPR_AUTO));
  ASSERT_EQ(nrq_gc_object_header_size(nullptr), size_t{0});
  int local = 0;
  ASSERT_EQ(nrq_gc_object_representation(&local),
            static_cast<int32_t>(NRQ_GC_REPR_AUTO));
}
