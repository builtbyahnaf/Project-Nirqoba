// AOR Phase 7: the unified collector across every representation.
//
// The representation tests pin what each header costs and how a page is reused.
// These tests pin what Phase 7 integrates: one collection that has to trace,
// promote, remember and reclaim objects whose headers differ, in graphs that mix
// them.
//
// Every case measures the live-object count relative to the count it started
// with, and releases its roots through an owner instead of a statement a failed
// assertion can skip. A test that throws therefore cannot leave a root behind and
// change what a later test sees, which is what makes these cases independent of
// the order the suite runs them in.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "nirqoba/runtime/alloc_hint.h"
#include "nirqoba/runtime/gc.h"

namespace {

const size_t kFieldOffset[] = {0};

const nrq_object_layout* record_layout(size_t payload_bytes) {
  return nrq_gc_layout_intern(payload_bytes, 0, nullptr, kFieldOffset, 1);
}

// Writes a managed field through the collector's store barrier, the same entry
// point generated code uses for a managed reference store.
void store_field(void* field, void* value) {
  nrq_gc_root_store(reinterpret_cast<void**>(field), value);
}

// A registered root slot that unregisters itself. Roots outlive a thrown
// assertion here, which is exactly when a leaked root would corrupt the next
// test's heap.
class RootSlot {
 public:
  RootSlot() : storage_(new void*(nullptr)) {
    nrq_gc_root_register(storage_.get());
  }
  ~RootSlot() { nrq_gc_root_unregister(storage_.get()); }
  RootSlot(const RootSlot&) = delete;
  RootSlot& operator=(const RootSlot&) = delete;

  void* get() const { return *storage_; }
  // Address of the registered slot, for entry points that publish into one.
  void** slot() { return storage_.get(); }
  void set(void* object) { nrq_gc_root_store(storage_.get(), object); }
  void clear() { nrq_gc_root_store(storage_.get(), nullptr); }

 private:
  // Own storage rather than a member: a root slot's address is its identity, so
  // a stack address this case happens to reuse for a slot cannot be mistaken for
  // a root another case registered at that address.
  std::unique_ptr<void*> storage_;
};

// A clean baseline at the start of a case, and a collection at the end so the
// objects the case left unreachable are actually gone.
struct CleanHeap {
  CleanHeap() {
    nrq_gc_disable();
    nrq_gc_collect();
    nrq_gc_collect();
  }
  ~CleanHeap() {
    nrq_gc_collect();
    nrq_gc_collect();
    nrq_gc_enable();
  }
};

}  // namespace

TEST(GcPhase7Test, AMinorKeepsAnOldObjectsYoungReferentInEveryRepresentation) {
  CleanHeap heap;
  const uint64_t base = nrq_gc_get_live_objects();

  const nrq_object_layout* small = record_layout(16);
  const nrq_object_layout* big = record_layout(512);
  ASSERT_NE(small, nullptr);
  ASSERT_NE(big, nullptr);

  RootSlot h0;
  RootSlot h8;
  RootSlot h16;
  h0.set(nrq_gc_allocate_layout(small));
  h8.set(nrq_gc_allocate_layout(big));
  h16.set(nrq_gc_allocate_array_typed(2, 16, kFieldOffset, 1));
  ASSERT_NE(h0.get(), nullptr);
  ASSERT_NE(h8.get(), nullptr);
  ASSERT_NE(h16.get(), nullptr);
  ASSERT_EQ(nrq_gc_object_representation(h0.get()),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_representation(h8.get()),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  ASSERT_EQ(nrq_gc_object_representation(h16.get()),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));

  // A full cycle promotes all three holders. The store barrier is armed from
  // here on because an old generation exists.
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{3});
  ASSERT_TRUE(nrq_gc_store_barrier_active() != 0);

  // The targets are young and reachable only through an old object's field.
  void* young_h0 = nrq_gc_allocate_layout(small);
  void* young_h8 = nrq_gc_allocate_layout(small);
  void* young_h16 = nrq_gc_allocate_layout(small);
  ASSERT_NE(young_h0, nullptr);
  ASSERT_NE(young_h8, nullptr);
  ASSERT_NE(young_h16, nullptr);
  store_field(h0.get(), young_h0);
  store_field(h8.get(), young_h8);
  store_field(h16.get(), young_h16);

  nrq_gc_collect_minor();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{6});
  ASSERT_TRUE(nrq_gc_pointer_is_managed(young_h0));
  ASSERT_TRUE(nrq_gc_pointer_is_managed(young_h8));
  ASSERT_TRUE(nrq_gc_pointer_is_managed(young_h16));
  // A minor collection leaves the referent young and the holder old.
  ASSERT_TRUE((nrq_gc_object_state_bits(h0.get()) & NRQ_GC_STATE_YOUNG) == 0);
  ASSERT_TRUE((nrq_gc_object_state_bits(young_h0) & NRQ_GC_STATE_YOUNG) != 0);

  // Clearing the edge does not drop the remembered entry: the collector recorded
  // that an old object referenced this young object, and it cannot know whether
  // another old object still does without scanning every remembered container.
  // The targets therefore stay reachable for the minor generation, which is the
  // documented conservative behaviour of the remembered set.
  store_field(h0.get(), nullptr);
  store_field(h8.get(), nullptr);
  store_field(h16.get(), nullptr);
  nrq_gc_collect_minor();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{6});
  ASSERT_TRUE(nrq_gc_pointer_is_managed(young_h0));

  // A full collection marks from the roots only, so it reclaims them.
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{3});
  ASSERT_FALSE(nrq_gc_pointer_is_managed(young_h0));
  ASSERT_FALSE(nrq_gc_pointer_is_managed(young_h8));
  ASSERT_FALSE(nrq_gc_pointer_is_managed(young_h16));

  h0.clear();
  h8.clear();
  h16.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
}

TEST(GcPhase7Test, ACycleThatCrossesRepresentationsIsReclaimed) {
  CleanHeap heap;
  const uint64_t base = nrq_gc_get_live_objects();

  RootSlot head;
  void* first = nrq_gc_allocate_layout(record_layout(16));    // H0
  void* second = nrq_gc_allocate_layout(record_layout(512));  // H8
  void* third = nrq_gc_allocate_array_typed(2, 16, kFieldOffset, 1);  // H16
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(third, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(first),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_representation(second),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  ASSERT_EQ(nrq_gc_object_representation(third),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));

  // first -> second -> third -> first closes the cycle across all three headers.
  store_field(first, second);
  store_field(second, third);
  store_field(third, first);
  head.set(first);

  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{3});

  head.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
  ASSERT_FALSE(nrq_gc_pointer_is_managed(first));
  ASSERT_FALSE(nrq_gc_pointer_is_managed(second));
  ASSERT_FALSE(nrq_gc_pointer_is_managed(third));
}

TEST(GcPhase7Test, ADeeperGraphThanTheMarkWorklistHoldsIsFullyMarked) {
  CleanHeap heap;
  const uint64_t base = nrq_gc_get_live_objects();

  // The inline worklist holds 256 objects; a chain this long lives almost
  // entirely in its overflow vector, which is the path that has to keep working
  // for arbitrarily deep graphs.
  constexpr size_t kNodes = 100000;
  const nrq_object_layout* layout = record_layout(16);
  ASSERT_NE(layout, nullptr);

  std::vector<void*> nodes;
  nodes.reserve(kNodes);
  for (size_t index = 0; index < kNodes; ++index) {
    void* node = nrq_gc_allocate_layout(layout);
    ASSERT_NE(node, nullptr);
    if (!nodes.empty()) store_field(nodes.back(), node);
    nodes.push_back(node);
  }

  RootSlot head;
  head.set(nodes.front());
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, static_cast<uint64_t>(kNodes));

  head.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
}

TEST(GcPhase7Test, ALargeObjectKeepsItsExactBoundaries) {
  CleanHeap heap;
  const uint64_t base = nrq_gc_get_live_objects();

  constexpr size_t kLargeBytes = 200 * 1024;
  RootSlot large;
  RootSlot small;
  large.set(nrq_gc_allocate(kLargeBytes, nullptr));
  small.set(nrq_gc_allocate(24, nullptr));
  ASSERT_NE(large.get(), nullptr);
  ASSERT_NE(small.get(), nullptr);
  ASSERT_EQ(nrq_gc_object_representation(large.get()),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_EQ(nrq_gc_object_header_size(large.get()), size_t{16});

  // One cycle reclaims the small object while the large one survives, so the
  // large object is discovered through the exact index rather than a bitmap.
  small.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{1});
  ASSERT_TRUE(nrq_gc_pointer_is_managed(large.get()));

  large.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
}

TEST(GcPhase7Test, APinnedObjectSurvivesCyclesWithoutARoot) {
  CleanHeap heap;
  const uint64_t base = nrq_gc_get_live_objects();

  const nrq_object_layout* layout = record_layout(32);
  ASSERT_NE(layout, nullptr);
  void* object = nrq_gc_allocate_layout(layout);
  void* child = nrq_gc_allocate_layout(record_layout(16));
  ASSERT_NE(object, nullptr);
  ASSERT_NE(child, nullptr);

  // The pin is the only thing keeping both allocations reachable. The child is
  // reachable through the pinned object's declared reference field.
  void* pinned = nrq_gc_pin(object);
  ASSERT_EQ(pinned, object);
  store_field(object, child);

  nrq_gc_collect();
  nrq_gc_collect_minor();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{2});
  ASSERT_TRUE(nrq_gc_pointer_is_managed(object));
  ASSERT_TRUE(nrq_gc_pointer_is_managed(child));

  nrq_gc_unpin(object);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
}

TEST(GcPhase7Test, NestedArraysStayReachableThroughTheOuterElement) {
  CleanHeap heap;
  const uint64_t base = nrq_gc_get_live_objects();

  RootSlot outer;
  outer.set(nrq_gc_allocate_array_typed(2, sizeof(void*), kFieldOffset, 1));
  ASSERT_NE(outer.get(), nullptr);

  std::vector<void*> inner;
  for (int index = 0; index < 2; ++index) {
    void* array = nrq_gc_allocate_array_typed(3, 16, kFieldOffset, 1);
    ASSERT_NE(array, nullptr);
    inner.push_back(array);
  }
  // outer[0] = inner[0], outer[1] = inner[1]. Only the outer array is a root.
  auto* outer_bytes = static_cast<unsigned char*>(outer.get());
  store_field(outer_bytes + 0 * sizeof(void*), inner[0]);
  store_field(outer_bytes + 1 * sizeof(void*), inner[1]);

  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{3});
  ASSERT_TRUE(nrq_gc_pointer_is_managed(inner[0]));
  ASSERT_TRUE(nrq_gc_pointer_is_managed(inner[1]));

  outer.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
}

TEST(GcPhase7Test, SoaGeometryKeepsItsIdentityAndTracesItsColumn) {
  CleanHeap heap;
  const uint64_t base = nrq_gc_get_live_objects();

  // Column 0 is a contiguous pointer column at offset 0 with stride 8; the rows
  // are twelve bytes, so the scalar column follows it. The geometry lookup is
  // memoised per thread, which must return the same descriptor for the same
  // table and a different one for a different table.
  const size_t pointers[] = {0, 8};
  const size_t other_pointers[] = {0, 12};

  RootSlot allocation;
  void* first = nrq_gc_allocate_soa_rooted(allocation.slot(), 4,
                                           12, pointers, 2);
  ASSERT_NE(first, nullptr);
  void* second = nrq_gc_allocate_soa_rooted(allocation.slot(), 4,
                                            12, pointers, 2);
  ASSERT_NE(second, nullptr);
  void* different = nrq_gc_allocate_soa_rooted(
      allocation.slot(), 4, 12, other_pointers, 2);
  ASSERT_NE(different, nullptr);
  ASSERT_EQ(nrq_gc_object_layout(first), nrq_gc_object_layout(second));
  ASSERT_TRUE(nrq_gc_object_layout(first) != nrq_gc_object_layout(different));

  // Row 2 of the pointer column is payload byte 16. A child stored there is the
  // only reference the marker has to follow.
  void* child = nrq_gc_allocate_layout(record_layout(16));
  ASSERT_NE(child, nullptr);
  std::memcpy(static_cast<unsigned char*>(first) + 16, &child, sizeof(child));

  RootSlot holder;
  holder.set(first);
  allocation.clear();

  nrq_gc_collect();
  ASSERT_TRUE(nrq_gc_pointer_is_managed(first));
  ASSERT_TRUE(nrq_gc_pointer_is_managed(child));

  holder.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
}

TEST(GcPhase7Test, APlannedRecordAllocatesTheRepresentationItAskedFor) {
  CleanHeap heap;
  const uint64_t base = nrq_gc_get_live_objects();

  RootSlot root;
  void* site_cache = nullptr;
  const uint32_t flags =
      NRQ_ALLOC_PLAN_HAS_REFERENCES |
      NRQ_ALLOC_PLAN_REPR_BITS(NRQ_ALLOC_REPR_HEADERLESS);

  void* first = nrq_gc_allocate_planned(root.slot(), 16,
                                       kFieldOffset, 1, &site_cache, flags);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(site_cache, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(first),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_header_size(first), size_t{0});

  // The cached descriptor is reused rather than re-resolved, and the second
  // allocation lands in the same layout as the first.
  void* second = nrq_gc_allocate_planned(root.slot(), 16,
                                        kFieldOffset, 1, &site_cache, flags);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(nrq_gc_object_layout(first), nrq_gc_object_layout(second));

  // A plan the runtime cannot honour falls back to its own decision instead of
  // allocating a representation the shape cannot use.
  void* rejected_cache = nullptr;
  const uint32_t impossible =
      NRQ_ALLOC_PLAN_REJECTED |
      NRQ_ALLOC_PLAN_REPR_BITS(NRQ_ALLOC_REPR_HEADERLESS);
  void* fallback = nrq_gc_allocate_planned(root.slot(), 16,
                                          kFieldOffset, 1, &rejected_cache,
                                          impossible);
  ASSERT_NE(fallback, nullptr);
  ASSERT_EQ(nrq_gc_object_representation(fallback),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));

  root.clear();
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
}
