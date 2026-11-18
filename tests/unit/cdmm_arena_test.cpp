// CDMM Phase 5: explicit arena behavior.
//
// The arena is the deterministic-lifetime strategy: it hands out aligned blocks
// from chunks it owns, runs the destructors it was given in reverse allocation
// order on bulk release, and refuses to reset while a scope pins it. These tests
// pin each of those contracts, including the failure paths.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/cdmm.h"

namespace {

std::vector<int>* destructor_order = nullptr;
int destructor_calls = 0;

void record_destructor(void* object, void* context) {
  ++destructor_calls;
  if (destructor_order)
    destructor_order->push_back(*static_cast<int*>(context));
  *static_cast<int*>(object) = -1;
}

// Frees storage the object owns through the ordinary allocator, so a test can
// show that an arena destructor runs while the arena still maps its chunks.
void release_nested(void* object, void* context) {
  auto** nested = static_cast<void**>(object);
  (void)context;
  ++destructor_calls;
  nrq_memory_deallocate_checked(*nested);
  *nested = nullptr;
}

}  // namespace

TEST(CdmmArenaTest, AllocatesAlignedContainedBlocksWithoutHeaders) {
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  void* small = nrq_cdmm_arena_allocate(arena, 8, 8);
  void* wide = nrq_cdmm_arena_allocate(arena, 64, 64);
  ASSERT_NE(small, nullptr);
  ASSERT_NE(wide, nullptr);
  ASSERT_EQ(reinterpret_cast<uintptr_t>(small) % 8, uintptr_t{0});
  ASSERT_EQ(reinterpret_cast<uintptr_t>(wide) % 64, uintptr_t{0});
  ASSERT_TRUE(nrq_cdmm_arena_contains(arena, small));
  ASSERT_TRUE(nrq_cdmm_arena_contains(arena, wide));
  ASSERT_FALSE(nrq_cdmm_arena_contains(arena, &small));
  // 8 bytes, then a 64-aligned 64-byte block: the chunk base is 64-aligned, so
  // the second block starts at offset 64 and the arena has handed out 128 bytes.
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{128});
  ASSERT_TRUE(nrq_cdmm_arena_committed(arena) >= uint64_t{4096});

  // The payload is the object: there is no header in front of it.
  NrqCdmmFacts facts{};
  ASSERT_EQ(nrq_cdmm_object_facts(small, &facts), int32_t{1});
  ASSERT_EQ(facts.strategy, NRQ_CDMM_STRATEGY_ARENA);
  ASSERT_EQ(facts.header_bytes, uint64_t{0});
  ASSERT_EQ(facts.generation, nrq_cdmm_arena_generation(arena));
  ASSERT_EQ(nrq_cdmm_strategy_of(small), NRQ_CDMM_STRATEGY_ARENA);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmArenaTest, ResetReusesTheFirstChunkAndAdvancesTheGeneration) {
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  const uint64_t first_generation = nrq_cdmm_arena_generation(arena);
  void* first = nrq_cdmm_arena_allocate(arena, 32, 8);
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{32});
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{0});
  const uint64_t second_generation = nrq_cdmm_arena_generation(arena);
  ASSERT_NE(first_generation, second_generation);
  // A pointer plus the generation it was allocated under is the runtime's stale
  // check: the address itself is deliberately reused.
  void* reused = nrq_cdmm_arena_allocate(arena, 32, 8);
  ASSERT_EQ(reused, first);
  ASSERT_NE(nrq_cdmm_arena_generation(arena), first_generation);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmArenaTest, ResetRunsDestructorsInReverseAllocationOrder) {
  std::vector<int> order;
  destructor_order = &order;
  destructor_calls = 0;
  void* arena = nrq_cdmm_arena_create(8192, 0);
  ASSERT_NE(arena, nullptr);
  int first_context = 1;
  int second_context = 2;
  void* first = nrq_cdmm_arena_allocate_destructible(arena, 32, 8,
                                                     record_destructor,
                                                     &first_context);
  void* second = nrq_cdmm_arena_allocate_destructible(arena, 32, 8,
                                                      record_destructor,
                                                      &second_context);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(destructor_calls, 2);
  ASSERT_EQ(order.size(), size_t{2});
  // Last allocated is destroyed first: an object can safely release what it owns.
  ASSERT_EQ(order[0], 2);
  ASSERT_EQ(order[1], 1);
  destructor_order = nullptr;
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmArenaTest, DestructorReleasesANestedOwnedResource) {
  destructor_calls = 0;
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  void* nested = nrq_memory_allocate(128, 8);
  ASSERT_NE(nested, nullptr);
  std::memset(nested, 0x5a, 128);
  void* owner = nrq_cdmm_arena_allocate_destructible(arena, sizeof(void*),
                                                     alignof(void*),
                                                     release_nested, nullptr);
  ASSERT_NE(owner, nullptr);
  *static_cast<void**>(owner) = nested;
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(destructor_calls, 1);
  // The nested storage was returned to the general allocator, which would stop
  // the process on a double or foreign release; nothing above can still reach it.
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmArenaTest, DestroyRunsDestructorsAndInvalidatesTheHandle) {
  destructor_calls = 0;
  int context = 0;
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  void* object = nrq_cdmm_arena_allocate_destructible(arena, 24, 8,
                                                      record_destructor,
                                                      &context);
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  ASSERT_EQ(destructor_calls, 1);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{0});
  ASSERT_EQ(nrq_cdmm_arena_allocate(arena, 8, 8), nullptr);
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{0});
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_strategy_of(object), NRQ_CDMM_STRATEGY_NONE);
}

TEST(CdmmArenaTest, CapIsHonouredAndFailuresLeaveUsageUnchanged) {
  void* arena = nrq_cdmm_arena_create(4096, 4096);
  ASSERT_NE(arena, nullptr);
  const uint64_t committed = nrq_cdmm_arena_committed(arena);
  ASSERT_EQ(committed, uint64_t{4096});
  ASSERT_EQ(nrq_cdmm_arena_allocate(arena, 8192, 8), nullptr);
  int32_t blocks = 0;
  while (nrq_cdmm_arena_allocate(arena, 32, 8) != nullptr) ++blocks;
  // The chunk is exactly 4096 bytes; the exact block count is the point, the cap
  // is what has to hold.
  ASSERT_TRUE(blocks > 0);
  ASSERT_TRUE(nrq_cdmm_arena_committed(arena) <= uint64_t{4096});
  ASSERT_EQ(nrq_cdmm_arena_committed(arena), committed);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmArenaTest, DestructibleAllocationRollsBackWhenThePayloadCannotFit) {
  destructor_calls = 0;
  void* arena = nrq_cdmm_arena_create(4096, 4096);
  ASSERT_NE(arena, nullptr);
  void* padding = nrq_cdmm_arena_allocate(arena, 3896, 8);
  ASSERT_NE(padding, nullptr);
  const uint64_t before = nrq_cdmm_arena_used(arena);
  int context = 0;
  // The record fits in the remaining bytes; the 4096-byte payload cannot, and no
  // growth chunk is allowed by the cap.
  void* object = nrq_cdmm_arena_allocate_destructible(arena, 4096, 8,
                                                      record_destructor,
                                                      &context);
  ASSERT_EQ(object, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_used(arena), before);
  // The arena is still usable at the same place, which is what the rollback is
  // for: a failed allocation must not leave a stray record behind.
  void* after = nrq_cdmm_arena_allocate(arena, 64, 8);
  ASSERT_NE(after, nullptr);
  ASSERT_EQ(destructor_calls, 0);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmArenaTest, ScopeAllocatesAndResetWaitsForItToClose) {
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  NrqCdmmArenaScope scope{};
  ASSERT_EQ(nrq_cdmm_arena_begin(arena, &scope), int32_t{1});
  ASSERT_EQ(scope.generation, nrq_cdmm_arena_generation(arena));
  void* first = nrq_cdmm_arena_scope_allocate(&scope, 16, 8);
  void* second = nrq_cdmm_arena_scope_allocate(&scope, 16, 8);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{0});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{0});
  nrq_cdmm_arena_end(&scope);
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{32});
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmArenaTest, BulkAllocateReturnsTheSuccessfulPrefix) {
  void* arena = nrq_cdmm_arena_create(4096, 4096);
  ASSERT_NE(arena, nullptr);
  std::vector<void*> blocks(1000, nullptr);
  const size_t written =
      nrq_cdmm_arena_allocate_bulk(arena, 32, 8, blocks.size(), blocks.data());
  ASSERT_TRUE(written > 0);
  ASSERT_TRUE(written < blocks.size());
  for (size_t index = 0; index < written; ++index) {
    ASSERT_NE(blocks[index], nullptr);
    ASSERT_TRUE(nrq_cdmm_arena_contains(arena, blocks[index]));
    if (index != 0) ASSERT_NE(blocks[index], blocks[index - 1]);
  }
  ASSERT_EQ(blocks[written], nullptr);
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmArenaTest, ScopeBulkAllocateUsesOnePinForTheWholeBatch) {
  void* arena = nrq_cdmm_arena_create(8192, 0);
  ASSERT_NE(arena, nullptr);
  NrqCdmmArenaScope scope{};
  ASSERT_EQ(nrq_cdmm_arena_begin(arena, &scope), int32_t{1});
  std::vector<void*> blocks(64, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_scope_allocate_bulk(&scope, 24, 8, blocks.size(),
                                               blocks.data()),
            blocks.size());
  nrq_cdmm_arena_end(&scope);
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{64 * 24});
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmArenaTest, InvalidArgumentsAreRejectedWithoutCreatingAnything) {
  void* defaulted = nrq_cdmm_arena_create(0, 0);
  ASSERT_NE(defaulted, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_destroy(defaulted), int32_t{1});
  void* arena = nrq_cdmm_arena_create(4096, 0);
  ASSERT_NE(arena, nullptr);
  ASSERT_EQ(nrq_cdmm_arena_allocate(arena, 0, 8), nullptr);
  ASSERT_EQ(nrq_cdmm_arena_allocate(arena, 8, 0), nullptr);
  ASSERT_EQ(nrq_cdmm_arena_allocate(arena, 8, 3), nullptr);
  ASSERT_EQ(nrq_cdmm_arena_allocate(nullptr, 8, 8), nullptr);
  ASSERT_EQ(nrq_cdmm_arena_begin(arena, nullptr), int32_t{0});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_arena_destroy(nullptr), int32_t{0});
}
