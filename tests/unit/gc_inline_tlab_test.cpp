// Tests for Upgrade Upsilon (inline TLAB fast path) and Upgrade Phi
// (compact single-element arrays). Covers H0/H8/H16 representations, TLAB
// refill & exhaustion, recycled-page zeroing, alignment, collection triggers,
// TLAB retirement on mutator leave, debug event gate parity, and concurrency.

#include "test_harness.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "nirqoba/runtime/alloc_hint.h"
#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/gc_debug.h"
#include "nirqoba/runtime/gc_inline.h"

namespace {

constexpr uint32_t kArrayCompactPlan =
    NRQ_ALLOC_PLAN_ARRAY | NRQ_ALLOC_PLAN_REPEATED |
    NRQ_ALLOC_PLAN_REPR_BITS(NRQ_ALLOC_REPR_COMPACT);
constexpr uint32_t kArrayExtendedPlan =
    NRQ_ALLOC_PLAN_ARRAY | NRQ_ALLOC_PLAN_REPEATED |
    NRQ_ALLOC_PLAN_REPR_BITS(NRQ_ALLOC_REPR_EXTENDED);

size_t align_span(size_t header_bytes, size_t payload_bytes) {
  const size_t raw = header_bytes + payload_bytes;
  return (raw + NRQ_GC_INLINE_SPAN_ALIGNMENT - 1) &
         ~size_t{NRQ_GC_INLINE_SPAN_ALIGNMENT - 1};
}

struct TreeNode24 {
  void* left;
  void* right;
  uint64_t value;
};
static_assert(sizeof(TreeNode24) == 24);

void* allocate_h8_with_fallback(void** root_slot, size_t payload_bytes,
                                const size_t* ref_offsets, size_t ref_count,
                                void** site_cache, uint32_t& site_id,
                                size_t& fast_hits, size_t& slow_hits) {
  const size_t span = align_span(8, payload_bytes);
  if (site_id != 0) {
    if (void* fast = nrq_gc_inline_try_allocate_h8(site_id, payload_bytes, span,
                                                   root_slot)) {
      ++fast_hits;
      return fast;
    }
  }
  ++slow_hits;
  void* slow = nrq_gc_allocate_array_planned(root_slot, 1, payload_bytes,
                                             ref_offsets, ref_count, site_cache,
                                             kArrayCompactPlan);
  if (slow && site_id == 0 && *site_cache) {
    site_id = nrq_gc_layout_id(
        static_cast<const nrq_object_layout*>(*site_cache));
  }
  return slow;
}

void* allocate_h16_with_fallback(void** root_slot, uint32_t count,
                                 size_t element_size,
                                 const size_t* ref_offsets, size_t ref_count,
                                 void** site_cache, uint32_t& site_id,
                                 size_t& fast_hits, size_t& slow_hits) {
  const size_t payload_bytes = static_cast<size_t>(count) * element_size;
  const size_t span = align_span(16, payload_bytes);
  if (site_id != 0) {
    if (void* fast = nrq_gc_inline_try_allocate_h16(
            site_id, count, payload_bytes, span, root_slot)) {
      ++fast_hits;
      return fast;
    }
  }
  ++slow_hits;
  void* slow = nrq_gc_allocate_array_planned(root_slot, count, element_size,
                                             ref_offsets, ref_count, site_cache,
                                             kArrayExtendedPlan);
  if (slow && site_id == 0 && *site_cache) {
    site_id = nrq_gc_layout_id(
        static_cast<const nrq_object_layout*>(*site_cache));
  }
  return slow;
}

}  // namespace

TEST(GcInlineTlabTest, SupportsH0H8H16InlineRepresentations) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();

  nrq_gc_mutator_enter();
  void* root_h0 = nullptr;
  void* root_h8 = nullptr;
  void* root_h16 = nullptr;
  nrq_gc_root_register(&root_h0);
  nrq_gc_root_register(&root_h8);
  nrq_gc_root_register(&root_h16);

  // H0 fixed record (64 bytes <= 256).
  void* h0_cache = nullptr;
  void* first_h0 =
      nrq_gc_allocate_planned(&root_h0, 64, nullptr, 0, &h0_cache, 0);
  ASSERT_NE(first_h0, nullptr);
  ASSERT_NE(h0_cache, nullptr);
  const size_t h0_slot = align_span(0, 64);
  void* fast_h0 =
      nrq_gc_inline_try_allocate_h0(h0_cache, 64, h0_slot, &root_h0);
  ASSERT_NE(fast_h0, nullptr);
  ASSERT_EQ(root_h0, fast_h0);
  // While still inside the TLAB, pending objects are invisible to the collector.
  ASSERT_EQ(nrq_gc_object_representation(fast_h0),
            static_cast<int32_t>(NRQ_GC_REPR_AUTO));
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_object_representation(fast_h0),
            static_cast<int32_t>(NRQ_GC_REPR_HEADERLESS));
  ASSERT_EQ(nrq_gc_object_header_size(fast_h0), size_t{0});

  // H8 compact 1-element array with managed references.
  const size_t tree_refs[2] = {0, sizeof(void*)};
  void* h8_cache = nullptr;
  uint32_t h8_id = 0;
  size_t h8_fast = 0;
  size_t h8_slow = 0;
  void* first_h8 = allocate_h8_with_fallback(
      &root_h8, sizeof(TreeNode24), tree_refs, 2, &h8_cache, h8_id, h8_fast,
      h8_slow);
  ASSERT_NE(first_h8, nullptr);
  ASSERT_NE(h8_id, uint32_t{0});
  void* fast_h8 = allocate_h8_with_fallback(
      &root_h8, sizeof(TreeNode24), tree_refs, 2, &h8_cache, h8_id, h8_fast,
      h8_slow);
  ASSERT_NE(fast_h8, nullptr);
  ASSERT_EQ(h8_fast, size_t{1});
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_object_representation(fast_h8),
            static_cast<int32_t>(NRQ_GC_REPR_COMPACT));
  ASSERT_EQ(nrq_gc_object_header_size(fast_h8), size_t{8});
  const nrq_object_layout* h8_layout = nrq_gc_object_layout(fast_h8);
  ASSERT_NE(h8_layout, nullptr);
  ASSERT_TRUE((h8_layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0);

  // H16 extended multi-element array (cannot share TLAB cursor with H8 simultaneously;
  // verify it works once H16 TLAB is active).
  const size_t elem_refs[1] = {0};
  void* h16_cache = nullptr;
  uint32_t h16_id = 0;
  size_t h16_fast = 0;
  size_t h16_slow = 0;
  void* first_h16 = allocate_h16_with_fallback(
      &root_h16, 4, sizeof(void*) * 2, elem_refs, 1, &h16_cache, h16_id,
      h16_fast, h16_slow);
  ASSERT_NE(first_h16, nullptr);
  ASSERT_NE(h16_id, uint32_t{0});
  // Flush any prior slab tail and verify H16 takes the inline TLAB.
  nrq_gc_collect();
  void* second_h16 = allocate_h16_with_fallback(
      &root_h16, 4, sizeof(void*) * 2, elem_refs, 1, &h16_cache, h16_id,
      h16_fast, h16_slow);
  ASSERT_NE(second_h16, nullptr);
  void* fast_h16 = allocate_h16_with_fallback(
      &root_h16, 4, sizeof(void*) * 2, elem_refs, 1, &h16_cache, h16_id,
      h16_fast, h16_slow);
  ASSERT_NE(fast_h16, nullptr);
  ASSERT_EQ(h16_fast, size_t{1});
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_object_representation(fast_h16),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_EQ(nrq_gc_object_header_size(fast_h16), size_t{16});

  // Wire a child inside H8 and H16, drop stack roots for children, and ensure
  // tracing keeps all rooted graphs intact across minor and major collections.
  auto* node = static_cast<TreeNode24*>(fast_h8);
  node->left = fast_h0;
  node->value = 0x123456789abcdef0ull;
  auto** h16_words = static_cast<void**>(fast_h16);
  h16_words[0] = fast_h8;
  root_h0 = nullptr;
  root_h8 = nullptr;

  nrq_gc_collect_minor();
  nrq_gc_collect();

  ASSERT_EQ(nrq_gc_object_representation(fast_h16),
            static_cast<int32_t>(NRQ_GC_REPR_EXTENDED));
  ASSERT_EQ(static_cast<TreeNode24*>(fast_h8)->value, 0x123456789abcdef0ull);
  ASSERT_EQ(static_cast<TreeNode24*>(fast_h8)->left, fast_h0);

  nrq_gc_root_unregister(&root_h16);
  nrq_gc_root_unregister(&root_h8);
  nrq_gc_root_unregister(&root_h0);
  nrq_gc_mutator_leave();
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcInlineTlabTest, RefillsExhaustedTlabAndZeroesRecycledPayloads) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_mutator_enter();

  void* root = nullptr;
  void* temp = nullptr;
  nrq_gc_root_register(&root);
  nrq_gc_root_register(&temp);

  const size_t tree_refs[2] = {0, sizeof(void*)};
  void* site_cache = nullptr;
  uint32_t site_id = 0;
  size_t fast_hits = 0;
  size_t slow_hits = 0;

  const uint64_t refills_before = nrq_gc_tlab_refill_count();
  // 5000 x 32B = 160 KB spans > 2 full 64 KiB TLABs.
  for (size_t i = 0; i < 5000; ++i) {
    void* obj = allocate_h8_with_fallback(&temp, sizeof(TreeNode24), tree_refs, 2,
                                          &site_cache, site_id, fast_hits,
                                          slow_hits);
    ASSERT_NE(obj, nullptr);
    auto* node = static_cast<TreeNode24*>(obj);
    // Dirty payload bytes with non-zero patterns so recycled space must zero.
    ASSERT_EQ(node->left, nullptr);
    ASSERT_EQ(node->right, nullptr);
    ASSERT_EQ(node->value, uint64_t{0});
    node->value = 0xdeadbeefcafebabeull ^ i;
    if (i % 128 == 0) {
      node->left = root;
      root = obj;
    }
  }
  temp = nullptr;
  ASSERT_TRUE(fast_hits > 4900);
  ASSERT_TRUE(nrq_gc_tlab_refill_count() > refills_before + 1);

  // Collect dead dirty objects so subsequent refills reuse dirty reclaimed runs.
  nrq_gc_collect();
  nrq_gc_collect();

  for (size_t i = 0; i < 4500; ++i) {
    void* obj = allocate_h8_with_fallback(&temp, sizeof(TreeNode24), tree_refs, 2,
                                          &site_cache, site_id, fast_hits,
                                          slow_hits);
    ASSERT_NE(obj, nullptr);
    auto* node = static_cast<TreeNode24*>(obj);
    ASSERT_EQ(node->left, nullptr);
    ASSERT_EQ(node->right, nullptr);
    ASSERT_EQ(node->value, uint64_t{0});
  }

  nrq_gc_root_unregister(&temp);
  nrq_gc_root_unregister(&root);
  nrq_gc_mutator_leave();
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcInlineTlabTest, PreservesAlignmentAndExactAccountingAcrossPayloadSizes) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_mutator_enter();

  void* root = nullptr;
  nrq_gc_root_register(&root);

  const uint64_t base_bytes = nrq_gc_get_allocated_bytes();
  const uint64_t base_live = nrq_gc_get_live_objects();
  uint64_t expected_added_bytes = 0;
  uint64_t expected_added_objects = 0;

  const size_t sizes[] = {1, 5, 13, 17, 24, 25, 33, 64};
  for (const size_t payload_bytes : sizes) {
    void* cache = nullptr;
    uint32_t site_id = 0;
    size_t fast_hits = 0;
    size_t slow_hits = 0;
    for (int rep = 0; rep < 8; ++rep) {
      void* obj = allocate_h8_with_fallback(
          &root, payload_bytes, nullptr, 0, &cache, site_id, fast_hits,
          slow_hits);
      ASSERT_NE(obj, nullptr);
      ASSERT_EQ(reinterpret_cast<uintptr_t>(obj) % NRQ_GC_INLINE_SPAN_ALIGNMENT,
                uintptr_t{0});
      const auto* bytes = static_cast<const uint8_t*>(obj);
      for (size_t b = 0; b < payload_bytes; ++b) ASSERT_EQ(bytes[b], uint8_t{0});
      expected_added_bytes += payload_bytes;
      ++expected_added_objects;
    }
    ASSERT_TRUE(fast_hits > 0);
  }

  // Pending fast-path allocations must already be visible in GC stats.
  ASSERT_EQ(nrq_gc_get_allocated_bytes(), base_bytes + expected_added_bytes);
  ASSERT_EQ(nrq_gc_get_live_objects(), base_live + expected_added_objects);

  nrq_gc_root_unregister(&root);
  nrq_gc_mutator_leave();
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_enable();
}

TEST(GcInlineTlabTest, TriggersCollectionRetirementAndDebugAllocationGate) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();

  bool failed = false;
  std::thread worker([&]() {
    nrq_gc_mutator_enter();
    void* root = nullptr;
    void* temp = nullptr;
    nrq_gc_root_register(&root);
    nrq_gc_root_register(&temp);

    const size_t tree_refs[2] = {0, sizeof(void*)};
    void* site_cache = nullptr;
    uint32_t site_id = 0;
    size_t fast_hits = 0;
    size_t slow_hits = 0;

    // Prime H8 TLAB and site ID.
    root = allocate_h8_with_fallback(&root, sizeof(TreeNode24), tree_refs, 2,
                                     &site_cache, site_id, fast_hits, slow_hits);
    if (!root || site_id == 0) failed = true;

    // Enable debug allocation events: inline path must step aside so every
    // allocation emits an allocation debug event.
    nrq_gc_debug_set_event_mask(NRQ_GC_EVENT_ALLOCATION);
    if (nrq_gc_inline_try_allocate_h8(site_id, sizeof(TreeNode24),
                                      align_span(8, sizeof(TreeNode24)),
                                      &temp) != nullptr) {
      failed = true;
    }
    nrq_gc_debug_set_event_mask(0);
    nrq_gc_event drained[64];
    while (nrq_gc_debug_drain_events(drained, 64) != 0) {}

    // Set a tight threshold and enable automatic collection: allocations must
    // trigger automatic collection through the gate/accounting-batch protocol.
    const uint64_t collections_before = nrq_gc_get_collection_count();
    nrq_gc_set_threshold(32 * 1024);
    nrq_gc_enable();
    for (size_t i = 0; i < 4000 && !failed; ++i) {
      temp = allocate_h8_with_fallback(&temp, sizeof(TreeNode24), tree_refs, 2,
                                       &site_cache, site_id, fast_hits,
                                       slow_hits);
      if (!temp) failed = true;
    }
    if (nrq_gc_get_collection_count() <= collections_before) failed = true;

    // Leaving mutator must retire TLAB and block subsequent inline allocation.
    nrq_gc_root_unregister(&temp);
    nrq_gc_root_unregister(&root);
    nrq_gc_mutator_leave();
    if (nrq_gc_fast_tls.tlab_kind !=
        static_cast<uint32_t>(NRQ_GC_INLINE_KIND_NONE)) {
      failed = true;
    }
    if (nrq_gc_inline_try_allocate_h8(site_id, sizeof(TreeNode24),
                                      align_span(8, sizeof(TreeNode24)),
                                      nullptr) != nullptr) {
      failed = true;
    }
  });
  worker.join();
  ASSERT_FALSE(failed);

  nrq_gc_set_adaptive(1);
  nrq_gc_collect();
  nrq_gc_collect();
}

TEST(GcInlineTlabTest, ConcurrentThreadsUseIndependentInlineTlabsAcrossGc) {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();
  nrq_gc_set_threshold(64 * 1024);
  nrq_gc_enable();

  constexpr size_t kThreads = 4;
  constexpr size_t kIterations = 2500;
  std::atomic<bool> failed{false};
  std::atomic<size_t> total_fast_hits{0};
  std::atomic<size_t> finished_workers{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (size_t tid = 0; tid < kThreads; ++tid) {
    workers.emplace_back([&, tid]() {
      nrq_gc_mutator_enter();
      void* head = nullptr;
      void* tmp = nullptr;
      nrq_gc_root_register(&head);
      nrq_gc_root_register(&tmp);

      const size_t tree_refs[2] = {0, sizeof(void*)};
      void* cache = nullptr;
      uint32_t site_id = 0;
      size_t fast_hits = 0;
      size_t slow_hits = 0;

      for (size_t i = 0; i < kIterations; ++i) {
        tmp = allocate_h8_with_fallback(&tmp, sizeof(TreeNode24), tree_refs, 2,
                                        &cache, site_id, fast_hits, slow_hits);
        if (!tmp) {
          failed.store(true);
          break;
        }
        auto* node = static_cast<TreeNode24*>(tmp);
        if (node->value != 0 || node->left != nullptr) {
          failed.store(true);
          break;
        }
        node->value = (tid << 32) | i;
        if ((i % 32) == 0) {
          nrq_gc_root_store(&node->left, head);
          nrq_gc_root_store(&head, tmp);
        }
        if ((i % 250) == 0) nrq_gc_safepoint();
      }

      // Walk retained chain to verify every retained object stayed intact.
      size_t chain_len = 0;
      for (void* cur = head; cur != nullptr;) {
        auto* node = static_cast<TreeNode24*>(cur);
        if ((node->value >> 32) != tid) {
          failed.store(true);
          break;
        }
        ++chain_len;
        cur = node->left;
      }
      if (chain_len != (kIterations + 31) / 32) failed.store(true);

      total_fast_hits.fetch_add(fast_hits);
      nrq_gc_root_unregister(&tmp);
      nrq_gc_root_unregister(&head);
      nrq_gc_mutator_leave();
      finished_workers.fetch_add(1, std::memory_order_release);
    });
  }

  while (finished_workers.load(std::memory_order_acquire) < kThreads) {
    nrq_gc_safepoint();
    std::this_thread::yield();
  }
  for (std::thread& worker : workers) worker.join();
  ASSERT_FALSE(failed.load());
  ASSERT_TRUE(total_fast_hits.load() > kThreads * (kIterations / 2));

  nrq_gc_set_adaptive(1);
  nrq_gc_collect();
  nrq_gc_collect();
}
