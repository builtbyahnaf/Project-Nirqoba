// AOR Phase 4: storage reuse across representations.
//
// A page is dedicated to one representation, so reuse is not free: a run on a
// compact page cannot serve a headerless object, and a thread that alternates two
// small shapes must not evict a half-full headerless slab on every allocation.
// These tests pin both rules and the recycling path that resets a page's kind.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nirqoba/runtime/gc.h"

namespace {

struct Small {
  uint64_t value;
};

struct Medium {
  uint64_t value;
  uint64_t extra;
};

struct Large {
  uint64_t value;
  unsigned char padding[1024];
};

}  // namespace

TEST(GcRepresentationReuseTest, AlternatingSmallShapesDoNotThrashSlabs) {
  // Two shapes that are both eligible for the headerless representation. The
  // first one takes the thread's slab; the second must be served as compact
  // rather than evicting a slab that is still half full.
  nrq_gc_disable();
  nrq_gc_set_threshold(static_cast<size_t>(1) << 30);
  nrq_gc_collect();
  const uint64_t refills_before = nrq_gc_tlab_refill_count();

  nrq_gc_mutator_enter();
  constexpr int kPairs = 4000;
  std::vector<void*> small;
  std::vector<void*> medium;
  small.reserve(kPairs);
  medium.reserve(kPairs);
  for (int index = 0; index < kPairs; ++index) {
    small.push_back(nrq_gc_allocate(sizeof(Small), nullptr));
    medium.push_back(nrq_gc_allocate(sizeof(Medium), nullptr));
  }
  nrq_gc_mutator_leave();

  for (void* object : small) ASSERT_NE(object, nullptr);
  for (void* object : medium) ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(small[0]),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_representation(medium[0]),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));

  // One refill per alternating allocation would be ~8000. A slab plus the compact
  // page TLABs must answer the whole wave with a handful.
  const uint64_t refills = nrq_gc_tlab_refill_count() - refills_before;
  ASSERT_TRUE(refills < static_cast<uint64_t>(kPairs) / 8);

  nrq_gc_collect();
}

TEST(GcRepresentationReuseTest, HeaderlessRunsAreNotSharedAcrossRepresentations) {
  // A reclaimed headerless run has the right byte span for a compact object of a
  // different shape, but the page kind decides how the object is decoded, so it
  // must not be handed out.
  nrq_gc_disable();
  nrq_gc_collect();
  const nrq_gc_representation_stats before = nrq_gc_get_representation_stats();

  for (int index = 0; index < 128; ++index) {
    void* object = nrq_gc_allocate(sizeof(Small), nullptr);
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(nrq_gc_object_representation(object),
              static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  }
  nrq_gc_collect();

  for (int index = 0; index < 128; ++index) {
    void* object = nrq_gc_allocate(sizeof(Large), nullptr);
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(nrq_gc_object_representation(object),
              static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  }
  const nrq_gc_representation_stats after = nrq_gc_get_representation_stats();
  ASSERT_TRUE(after.headerless_pages >= before.headerless_pages + 1);
  ASSERT_TRUE(after.compact_pages >= before.compact_pages + 1);
  nrq_gc_collect();
}

TEST(GcRepresentationReuseTest, RecycledPagesServeTheirNewRepresentation) {
  nrq_gc_disable();
  nrq_gc_collect();
  // Three lives of the same page pool. The kind, the uniform layout and the
  // object-index fallback flag all have to be reset between them; a stale
  // fallback flag used to make the sweep skip the page entirely.
  for (int life = 0; life < 3; ++life) {
    std::vector<void*> objects;
    for (int index = 0; index < 512; ++index) {
      void* object = life == 1 ? nrq_gc_allocate(sizeof(Large), nullptr)
                               : nrq_gc_allocate(sizeof(Small), nullptr);
      ASSERT_NE(object, nullptr);
      objects.push_back(object);
    }
    const int32_t expected = life == 1
                                 ? static_cast<int32_t>(NRQ_GC_REPR_COMPACT)
                                 : static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS);
    for (void* object : objects) {
      ASSERT_EQ(nrq_gc_object_representation(object), expected);
      ASSERT_EQ(nrq_gc_object_header_size(object),
                expected == NRQ_GC_REPR_HEADERLESS ? size_t{0} : size_t{8});
    }
    nrq_gc_collect();
    ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
  }
}

TEST(GcRepresentationReuseTest, RecycledHeaderlessPagesKeepTheirLayoutIdentity) {
  // The same page must not be handed a second would-be uniform layout while an
  // object of the first is still live, and after recycling it must describe the
  // layout it now holds.
  nrq_gc_disable();
  nrq_gc_collect();
  void* first = nrq_gc_allocate(sizeof(Small), nullptr);
  void* second = nrq_gc_allocate(sizeof(Medium), nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  const nrq_object_layout* first_layout = nrq_gc_object_layout(first);
  const nrq_object_layout* second_layout = nrq_gc_object_layout(second);
  ASSERT_NE(first_layout, nullptr);
  ASSERT_NE(second_layout, nullptr);
  ASSERT_TRUE(first_layout != second_layout);
  ASSERT_EQ(first_layout->payload_size, uint32_t{8});
  ASSERT_EQ(second_layout->payload_size, uint32_t{16});
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects(), uint64_t{0});
}
