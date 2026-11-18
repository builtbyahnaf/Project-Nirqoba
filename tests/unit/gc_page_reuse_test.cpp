// AOR Phase 8: page reuse and payload initialisation.
//
// The collector promises every managed payload is zero-initialised. Phase 8 made
// that promise cheaper by not writing zeros where the storage already holds them,
// which turns the guarantee into something a page's history decides: a page the OS
// has just committed is zero, and a page that held objects before is not. These
// cases pin both halves, because the second one is where a skipped fill would hand
// a program another object's bytes.
//
// The same file pins the other two changes whose failure modes are silent: the run
// list that records reclaimed space must not drop space a page has already freed,
// and the phase timing must report the work a cycle did rather than a constant.

#include "test_harness.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "nirqoba/runtime/gc.h"

namespace {

const size_t kFieldOffset[] = {0};

const nrq_object_layout* record_layout(size_t payload_bytes) {
  return nrq_gc_layout_intern(payload_bytes, 0, nullptr, kFieldOffset, 1);
}

// A registered root region over an externally owned array of managed pointers.
// The address is captured at registration and released with that same address: a
// region whose vector is resized before it is released would otherwise leave the
// original registration behind, and the collector would go on scanning storage the
// vector no longer owns. For the same reason a case that wants to make its objects
// garbage nulls the entries instead of freeing the region.
class RootRegion {
 public:
  explicit RootRegion(std::vector<void*>& region)
      : count_(region.size()),
        base_(region.empty() ? nullptr
                             : reinterpret_cast<void**>(region.data())) {
    if (base_)
      nrq_gc_root_register_array(base_, count_, sizeof(void*), kFieldOffset, 1);
  }
  ~RootRegion() {
    if (base_) nrq_gc_root_unregister(base_);
  }
  RootRegion(const RootRegion&) = delete;
  RootRegion& operator=(const RootRegion&) = delete;

 private:
  size_t count_;
  void** base_;
};

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

bool payload_is_zero(const void* payload, size_t bytes) {
  const auto* cursor = static_cast<const unsigned char*>(payload);
  for (size_t index = 0; index < bytes; ++index)
    if (cursor[index] != 0) return false;
  return true;
}

size_t first_nonzero(const void* payload, size_t bytes) {
  const auto* cursor = static_cast<const unsigned char*>(payload);
  for (size_t index = 0; index < bytes; ++index)
    if (cursor[index] != 0) return index;
  return SIZE_MAX;
}

}  // namespace

// The ordinary case: storage a page has never handed out is zero, so the payload
// a program receives must be zero too - the fill being skipped is not permission
// for the bytes to be anything.
TEST(GcPageReuseTest, AnUntouchedPageHandsOutZeroedPayloads) {
  CleanHeap heap;
  const size_t payloads[] = {16, 64, 256};
  for (size_t payload : payloads) {
    const nrq_object_layout* layout = record_layout(payload);
    ASSERT_NE(layout, nullptr);
    std::vector<void*> objects(512, nullptr);
    RootRegion region(objects);
    for (void*& object : objects) {
      object = nrq_gc_allocate_layout(layout);
      ASSERT_NE(object, nullptr);
      ASSERT_EQ(first_nonzero(object, payload), SIZE_MAX);
    }
  }
}

// The case the optimization must not get wrong: storage that held another object,
// was reclaimed, and is handed out again. Whatever the previous tenant wrote is
// still there, so the payload has to be filled again.
TEST(GcPageReuseTest, ReclaimedSpaceIsZeroedAgain) {
  CleanHeap heap;
  const nrq_object_layout* layout = record_layout(64);
  ASSERT_NE(layout, nullptr);

  {
    std::vector<void*> objects(4096, nullptr);
    RootRegion region(objects);
    for (void*& object : objects) {
      object = nrq_gc_allocate_layout(layout);
      ASSERT_NE(object, nullptr);
      std::memset(object, 0xAB, 64);
    }
    // Retain a few objects so their pages stay live and the space the rest
    // occupied is recorded as reusable runs instead of the whole page being
    // recycled.
    std::vector<void*> kept;
    for (size_t index = 0; index < objects.size(); index += 64)
      kept.push_back(objects[index]);
    RootRegion kept_region(kept);
    // Drop every root by emptying the registered region, not by freeing it.
    std::fill(objects.begin(), objects.end(), nullptr);
    nrq_gc_collect();
    ASSERT_TRUE(nrq_gc_page_count() > 0);
  }
  // The retained objects are gone now, so the next wave allocates into space the
  // previous one used.
  nrq_gc_collect();

  std::vector<void*> reused(2048, nullptr);
  RootRegion region(reused);
  for (size_t index = 0; index < reused.size(); ++index) {
    reused[index] = nrq_gc_allocate_layout(layout);
    ASSERT_NE(reused[index], nullptr);
    ASSERT_EQ(first_nonzero(reused[index], 64), SIZE_MAX);
  }
}

// A page that loses every other object needs one recorded run per dead object. The
// list was capped, and everything above the cap was discarded while the page went
// on being unable to serve a request from it. Nothing may be discarded now.
TEST(GcPageReuseTest, AShatteredPageRecordsAllItsFreeSpace) {
  CleanHeap heap;
  const nrq_object_layout* layout = record_layout(16);
  ASSERT_NE(layout, nullptr);

  const uint64_t dropped_before = nrq_gc_dropped_free_run_bytes();
  size_t runs_after_reclaim = 0;
  {
    std::vector<void*> objects(6000, nullptr);
    RootRegion region(objects);
    for (void*& object : objects) {
      object = nrq_gc_allocate_layout(layout);
      ASSERT_NE(object, nullptr);
    }
    ASSERT_EQ(nrq_gc_object_representation(objects.front()),
              static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));

    // Keep every other object, interleaved, which is the pattern that shatters a
    // page rather than merging its holes.
    std::vector<void*> survivors;
    survivors.reserve(objects.size() / 2);
    for (size_t index = 0; index < objects.size(); index += 2)
      survivors.push_back(objects[index]);
    RootRegion survivor_region(survivors);
    std::fill(objects.begin(), objects.end(), nullptr);
    nrq_gc_collect();
    ASSERT_EQ(nrq_gc_dropped_free_run_bytes() - dropped_before, uint64_t{0});
    runs_after_reclaim = nrq_gc_free_run_count();
    // Every dead slot is a run: 3000 of them, less whatever the page merged.
    ASSERT_TRUE(runs_after_reclaim >= 2000);
  }
}

// The second wave is the measure of the first wave's reclamation: the space it
// freed should serve it, so the heap must not grow a page per few hundred
// objects.
TEST(GcPageReuseTest, ASecondWaveReusesTheSpaceItFreed) {
  CleanHeap heap;
  const nrq_object_layout* layout = record_layout(16);
  ASSERT_NE(layout, nullptr);

  size_t pages_after_reclaim = 0;
  size_t pages_after_wave = 0;
  {
    std::vector<void*> objects(8192, nullptr);
    RootRegion region(objects);
    for (void*& object : objects)
      object = nrq_gc_allocate_layout(layout);
    std::vector<void*> survivors;
    survivors.reserve(objects.size() / 2);
    for (size_t index = 0; index < objects.size(); index += 2)
      survivors.push_back(objects[index]);
    RootRegion survivor_region(survivors);
    std::fill(objects.begin(), objects.end(), nullptr);
    nrq_gc_collect();
    pages_after_reclaim = nrq_gc_page_count();
    ASSERT_TRUE(pages_after_reclaim > 0);

    // Reuse the half that died, still holding the survivors.
    std::vector<void*> second(4096, nullptr);
    RootRegion second_region(second);
    for (void*& object : second) object = nrq_gc_allocate_layout(layout);
    pages_after_wave = nrq_gc_page_count();
  }
  ASSERT_TRUE(pages_after_wave <= pages_after_reclaim + 1);
}

// The phase report exists so a marking change can be judged without a whole-cycle
// figure that also contains the sweep. It has to name the work it did.
TEST(GcPageReuseTest, ACycleReportsItsPhases) {
  CleanHeap heap;
  const nrq_object_layout* layout = record_layout(32);
  ASSERT_NE(layout, nullptr);

  std::vector<void*> objects(2048, nullptr);
  RootRegion region(objects);
  for (void*& object : objects) {
    object = nrq_gc_allocate_layout(layout);
    ASSERT_NE(object, nullptr);
  }

  nrq_gc_reset_cycle_timing();
  nrq_gc_collect();
  const nrq_gc_cycle_timing live = nrq_gc_get_cycle_timing();
  ASSERT_TRUE(live.cycles >= 1);
  ASSERT_TRUE(live.mark_objects >= 2048);
  ASSERT_TRUE(live.pause_ns > 0);
  ASSERT_TRUE(live.mark_ns + live.sweep_ns + live.prepare_ns +
                  live.finish_ns <=
              live.pause_ns + 1000);

  // With no root left there is nothing to mark, and the reclaimer may classify
  // each page from its counts without walking it - which is the point of the
  // counts. What the report must show is the work that was actually done: no
  // marks this time, and a cycle that still accounted for its own pause.
  std::fill(objects.begin(), objects.end(), nullptr);
  nrq_gc_reset_cycle_timing();
  nrq_gc_collect();
  const nrq_gc_cycle_timing dead = nrq_gc_get_cycle_timing();
  ASSERT_TRUE(dead.cycles >= 1);
  ASSERT_EQ(dead.mark_objects, uint64_t{0});
  ASSERT_TRUE(dead.pause_ns > 0);
}

// A page dedicated to one large object is where skipping the fill was worth the
// most, so its payload is checked as carefully as a small one's.
TEST(GcPageReuseTest, ALargePageStillReturnsZeroedStorage) {
  CleanHeap heap;
  nrq_gc_reset_zeroing_facts();
  const size_t large = 128 * 1024;
  size_t first_bad = 0;
  {
    std::vector<void*> region(1, nullptr);
    RootRegion root(region);
    region[0] = nrq_gc_allocate(large, nullptr);
    ASSERT_NE(region[0], nullptr);
    first_bad = first_nonzero(region[0], large);
    nrq_gc_collect();
  }
  ASSERT_EQ(first_bad, SIZE_MAX);
  const nrq_gc_zeroing_facts facts = nrq_gc_get_zeroing_facts();
  ASSERT_TRUE(facts.prezeroed_bytes > 0);
  ASSERT_EQ(facts.filled_bytes, uint64_t{0});
}
