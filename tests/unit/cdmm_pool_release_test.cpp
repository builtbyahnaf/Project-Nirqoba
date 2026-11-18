// CDMM Phase 6-alpha: the pool release path.
//
// Release and reset were rewritten around a per-chunk free list and a sorted
// chunk index. These tests pin the behaviour that rewrite must preserve: no
// duplicate free entries after a partial reset, exactly-once reuse, generation
// validation including the wraparound rule, and refusal of invalid releases.

#include "test_harness.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "nirqoba/runtime/cdmm.h"

namespace {

int release_destructor_calls = 0;

void release_destructor(void* object, void* context) {
  (void)object;
  (void)context;
  ++release_destructor_calls;
}

}  // namespace

TEST(CdmmPoolReleaseTest, ScatteredReleaseAcrossChunksThenResetReusesEverySlot) {
  void* pool = nrq_cdmm_pool_create(16, 8, 8, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  std::vector<void*> slots;
  for (int index = 0; index < 40; ++index) {
    void* slot = nrq_cdmm_pool_allocate(pool);
    ASSERT_NE(slot, nullptr);
    slots.push_back(slot);
  }
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{40});
  for (size_t index = 0; index < slots.size(); index += 3)
    ASSERT_EQ(nrq_cdmm_pool_release(pool, slots[index]), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{40 - 14});
  ASSERT_EQ(nrq_cdmm_pool_reset(pool), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});

  // Every slot must be handed out exactly once again: a duplicated free-list
  // entry would show up as a repeated address here.
  std::set<void*> reused;
  for (int index = 0; index < 40; ++index) {
    void* slot = nrq_cdmm_pool_allocate(pool);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(reused.insert(slot).second);
  }
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{40});
  for (void* slot : reused) ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolReleaseTest, ResetAfterPartialReleaseDoesNotDuplicateFreeSlots) {
  void* pool = nrq_cdmm_pool_create(24, 8, 16, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  std::vector<void*> slots;
  for (int index = 0; index < 64; ++index) {
    slots.push_back(nrq_cdmm_pool_allocate(pool));
    ASSERT_NE(slots.back(), nullptr);
  }
  // Release one slot in each chunk, then reset the rest.
  for (size_t index = 0; index < slots.size(); index += 16)
    ASSERT_EQ(nrq_cdmm_pool_release(pool, slots[index]), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_reset(pool), int32_t{1});
  std::set<void*> reused;
  for (int index = 0; index < 64; ++index) {
    void* slot = nrq_cdmm_pool_allocate(pool);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(reused.insert(slot).second);
  }
  // Double release after a reset still fails: each slot is live exactly once.
  for (void* slot : reused) {
    ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
    ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{0});
  }
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolReleaseTest, ResetDestroysOnlyTheLiveSlots) {
  release_destructor_calls = 0;
  void* pool = nrq_cdmm_pool_create(16, 8, 8, 0, release_destructor, nullptr);
  ASSERT_NE(pool, nullptr);
  std::vector<void*> slots;
  for (int index = 0; index < 10; ++index) {
    slots.push_back(nrq_cdmm_pool_allocate(pool));
    ASSERT_NE(slots.back(), nullptr);
  }
  for (int index = 0; index < 10; index += 2)
    ASSERT_EQ(nrq_cdmm_pool_release(pool, slots[index]), int32_t{1});
  ASSERT_EQ(release_destructor_calls, 5);
  ASSERT_EQ(nrq_cdmm_pool_reset(pool), int32_t{1});
  ASSERT_EQ(release_destructor_calls, 10);
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolReleaseTest, ReleaseAfterResetIsRejected) {
  void* pool = nrq_cdmm_pool_create(16, 8, 8, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_reset(pool), int32_t{1});
  // The address is free again, so releasing it without allocating it first is a
  // double release, not a silent success.
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolReleaseTest, AddressFromAnotherPoolIsRejected) {
  void* first = nrq_cdmm_pool_create(16, 8, 8, 0, nullptr, nullptr);
  void* second = nrq_cdmm_pool_create(16, 8, 8, 0, nullptr, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  void* first_slot = nrq_cdmm_pool_allocate(first);
  void* second_slot = nrq_cdmm_pool_allocate(second);
  ASSERT_EQ(nrq_cdmm_pool_release(first, second_slot), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_release(second, first_slot), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_live(first), uint64_t{1});
  ASSERT_EQ(nrq_cdmm_pool_live(second), uint64_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(first, first_slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(second, second_slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(first), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(second), int32_t{1});
}

#if defined(NRQ_RUNTIME_DEBUG_ENABLED) && NRQ_RUNTIME_DEBUG_ENABLED
TEST(CdmmPoolReleaseTest, GenerationWrapSkipsZero) {
  void* pool = nrq_cdmm_pool_create(16, 8, 8, NRQ_CDMM_POOL_GENERATIONS, nullptr,
                                    nullptr);
  ASSERT_NE(pool, nullptr);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(slot, nullptr);
  ASSERT_EQ(nrq_cdmm_pool_seed_generation(pool, slot, 0xFFFFFFFFu), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  void* reused = nrq_cdmm_pool_allocate(pool);
  ASSERT_EQ(reused, slot);
  // Zero is reserved for "no allocation", so the counter wraps to one.
  ASSERT_EQ(nrq_cdmm_pool_generation(pool, reused), uint64_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release_checked(pool, reused, 0xFFFFFFFFu),
            int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_release_checked(pool, reused, 1), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolReleaseTest, GenerationComparisonUsesTheLowThirtyTwoBits) {
  void* pool = nrq_cdmm_pool_create(16, 8, 8, NRQ_CDMM_POOL_GENERATIONS, nullptr,
                                    nullptr);
  ASSERT_NE(pool, nullptr);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_EQ(nrq_cdmm_pool_seed_generation(pool, slot, 3), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  void* reused = nrq_cdmm_pool_allocate(pool);
  ASSERT_EQ(reused, slot);
  ASSERT_EQ(nrq_cdmm_pool_generation(pool, reused), uint64_t{4});
  // The stored generation is 32 bits wide; a 64-bit value that truncates to the
  // live generation is accepted, which is the documented comparison.
  ASSERT_EQ(nrq_cdmm_pool_release_checked(pool, reused, 4 + (uint64_t{1} << 32)),
            int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}
#endif

TEST(CdmmPoolReleaseTest, LiveCountAndFactsStayConsistentAcrossChurn) {
  void* pool = nrq_cdmm_pool_create(32, 8, 8, NRQ_CDMM_POOL_GENERATIONS, nullptr,
                                    nullptr);
  ASSERT_NE(pool, nullptr);
  std::vector<void*> slots;
  for (int round = 0; round < 50; ++round) {
    slots.clear();
    for (int index = 0; index < 12; ++index) {
      void* slot = nrq_cdmm_pool_allocate(pool);
      ASSERT_NE(slot, nullptr);
      slots.push_back(slot);
    }
    ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{12});
    for (size_t index = 0; index < slots.size(); index += 2)
      ASSERT_EQ(nrq_cdmm_pool_release(pool, slots[index]), int32_t{1});
    ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{12 - slots.size() / 2});
    for (size_t index = 1; index < slots.size(); index += 2)
      ASSERT_EQ(nrq_cdmm_pool_release(pool, slots[index]), int32_t{1});
    ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  }
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}
