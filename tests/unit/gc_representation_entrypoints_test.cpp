// AOR Phase 4: the representation every public allocation entry point produces.
//
// The auto policy is one thing; a caller that allocates through a descriptor, a
// typed array, a SoA container, or a rooted slot must land in the representation
// its shape calls for, including at the headerless size boundary and for objects
// too large for a page.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nirqoba/runtime/gc.h"

namespace {

struct Record {
  uint64_t value;
};

struct BigRecord {
  uint64_t value;
  unsigned char padding[1024];
};

// A root that does not live in any test's stack frame.
void* rooted_slot = nullptr;

int32_t representation_of(void* object) {
  return nrq_gc_object_representation(object);
}

}  // namespace

TEST(GcRepresentationEntryPointTest, LargeObjectsUseTheExactIndexButStillH16) {
  nrq_gc_disable();
  nrq_gc_collect();
  // A large object is not found through a start bitmap, so its representation
  // must come from its index entry rather than its page walk.
  void* large = nrq_gc_allocate(200000, nullptr);
  ASSERT_NE(large, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(large),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_EQ(nrq_gc_object_header_size(large), size_t{16});
  ASSERT_EQ(nrq_gc_pointer_is_managed(large), int32_t{1});
  nrq_gc_collect();
  if (nrq_gc_pointer_is_managed(large) != 0) nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
  ASSERT_EQ(nrq_gc_pointer_is_managed(large), int32_t{0});
}

TEST(GcRepresentationEntryPointTest, DescriptorAllocationKeepsItsRepresentation) {
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_object_layout* small = nrq_gc_layout_intern(sizeof(Record), 0, nullptr,
                                                        nullptr, 0);
  const nrq_object_layout* big =
      nrq_gc_layout_intern(sizeof(BigRecord), 0, nullptr, nullptr, 0);
  ASSERT_NE(small, nullptr);
  ASSERT_NE(big, nullptr);

  void* small_object = nrq_gc_allocate_layout(small);
  void* big_object = nrq_gc_allocate_layout(big);
  ASSERT_NE(small_object, nullptr);
  ASSERT_NE(big_object, nullptr);
  ASSERT_EQ(representation_of(small_object),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(representation_of(big_object),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  // A descriptor path must store the same descriptor, not a copy.
  ASSERT_TRUE(nrq_gc_object_layout(small_object) == small);
  ASSERT_TRUE(nrq_gc_object_layout(big_object) == big);
  nrq_gc_collect();
}

TEST(GcRepresentationEntryPointTest, ArrayLayoutAndTypedEntryPointsAreExtended) {
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_object_layout* element =
      nrq_gc_layout_intern(sizeof(void*), sizeof(void*), nullptr, nullptr, 0);
  ASSERT_NE(element, nullptr);
  ASSERT_TRUE((element->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0);

  // All three are taken inside one mutator batch: an unpublished object is not
  // discoverable, so the representation can only be read after the batch is
  // published at leave.
  nrq_gc_mutator_enter();
  const size_t offsets[1] = {0};
  void* layout_array = nrq_gc_allocate_array_layout(element, 4);
  void* typed_array =
      nrq_gc_allocate_array_typed(4, sizeof(void*), offsets, 1);
  // The rooted entry point requires a registered slot. The slot is a static so
  // the root does not point at a stack frame that is gone by the next test, and
  // it is unregistered before the batch is published.
  nrq_gc_root_register(&rooted_slot);
  void* rooted_array =
      nrq_gc_allocate_array_typed_rooted(&rooted_slot, 4, sizeof(void*), offsets, 1);
  const bool rooted_slot_matches = rooted_slot == rooted_array;
  nrq_gc_mutator_leave();
  nrq_gc_root_unregister(&rooted_slot);

  for (void* object : {layout_array, typed_array, rooted_array}) {
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(representation_of(object),
              static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
    ASSERT_EQ(nrq_gc_object_header_size(object), size_t{16});
  }
  ASSERT_TRUE(rooted_slot_matches);
  nrq_gc_collect();
}

TEST(GcRepresentationEntryPointTest, PayloadBoundarySwitchesRepresentation) {
  nrq_gc_disable();
  nrq_gc_collect();
  // 256 bytes is the headerless bound; one more byte goes to the compact
  // representation, and an object whose span exceeds a page goes large.
  void* at_bound = nrq_gc_allocate(256, nullptr);
  void* past_bound = nrq_gc_allocate(257, nullptr);
  void* large = nrq_gc_allocate(100000, nullptr);
  ASSERT_NE(at_bound, nullptr);
  ASSERT_NE(past_bound, nullptr);
  ASSERT_NE(large, nullptr);
  ASSERT_EQ(representation_of(at_bound),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_header_size(at_bound), size_t{0});
  ASSERT_EQ(representation_of(past_bound),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  ASSERT_EQ(nrq_gc_object_header_size(past_bound), size_t{8});
  ASSERT_EQ(representation_of(large),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_EQ(nrq_gc_object_header_size(large), size_t{16});
  ASSERT_NE(nrq_gc_object_layout(large), nullptr);
  nrq_gc_collect();
}

TEST(GcRepresentationEntryPointTest, PinningHoldsEveryRepresentation) {
  nrq_gc_disable();
  nrq_gc_collect();
  const uint64_t baseline = nrq_gc_get_live_objects();
  void* headerless = nrq_gc_allocate(sizeof(Record), nullptr);
  void* compact = nrq_gc_allocate(sizeof(BigRecord), nullptr);
  void* extended = nrq_gc_allocate_array(4, sizeof(void*), nullptr);
  void* objects[3] = {headerless, compact, extended};
  for (void* object : objects) {
    ASSERT_NE(object, nullptr);
    ASSERT_TRUE(nrq_gc_pin(object) == object);
  }
  nrq_gc_collect();
  for (void* object : objects)
    ASSERT_EQ(nrq_gc_pointer_is_managed(object), int32_t{1});
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline + 3);
  for (void* object : objects) nrq_gc_unpin(object);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), baseline);
}

