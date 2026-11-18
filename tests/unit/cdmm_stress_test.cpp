// CDMM Phase 5: churn, reuse and concurrency stress.
//
// These tests run the strategies long enough and from enough threads that a
// bookkeeping mistake (a doubled free-list entry, a stale chunk range, a slot
// handed to two owners) shows up as a failed count rather than as a comment.

#include "test_harness.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/cdmm.h"

namespace {

uint64_t next_seed(uint64_t& seed) {
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return seed;
}

}  // namespace

TEST(CdmmStressTest, ArenaReusesOneChunkAcrossManyFrames) {
  void* arena = nrq_cdmm_arena_create(64 * 1024, 0);
  ASSERT_NE(arena, nullptr);
  const uint64_t committed = nrq_cdmm_arena_committed(arena);
  uint64_t seed = 0x2545f4914f6cdd1dull;
  for (int round = 0; round < 200; ++round) {
    NrqCdmmArenaScope scope{};
    ASSERT_EQ(nrq_cdmm_arena_begin(arena, &scope), int32_t{1});
    for (int index = 0; index < 500; ++index) {
      const size_t bytes = 8 + next_seed(seed) % 96;
      void* block = nrq_cdmm_arena_scope_allocate(&scope, bytes, 8);
      ASSERT_NE(block, nullptr);
      std::memset(block, static_cast<int>(round & 0xff), bytes);
    }
    nrq_cdmm_arena_end(&scope);
    ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
    ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{0});
    ASSERT_EQ(nrq_cdmm_arena_committed(arena), committed);
  }
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
}

TEST(CdmmStressTest, PoolChurnKeepsLivenessExact) {
  void* pool = nrq_cdmm_pool_create(48, 8, 0, NRQ_CDMM_POOL_GENERATIONS, nullptr,
                                    nullptr);
  ASSERT_NE(pool, nullptr);
  constexpr size_t kSlots = 512;
  std::vector<void*> slots(kSlots, nullptr);
  std::vector<size_t> order(kSlots);
  for (size_t index = 0; index < kSlots; ++index) order[index] = index;
  uint64_t seed = 0x9e3779b97f4a7c15ull;
  for (int round = 0; round < 100; ++round) {
    std::set<void*> live;
    for (size_t index = 0; index < kSlots; ++index) {
      slots[index] = nrq_cdmm_pool_allocate(pool);
      ASSERT_NE(slots[index], nullptr);
      ASSERT_TRUE(live.insert(slots[index]).second);
    }
    ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{kSlots});
    for (size_t index = kSlots; index-- > 0;) {
      const size_t other = next_seed(seed) % kSlots;
      std::swap(order[index], order[other]);
    }
    for (size_t index = 0; index < kSlots; ++index) {
      ASSERT_EQ(nrq_cdmm_pool_release(pool, slots[order[index]]), int32_t{1});
    }
    ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  }
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmStressTest, ConcurrentMutatorsShareOneArenaAndOnePool) {
  constexpr size_t kThreads = 4;
  constexpr size_t kPerThread = 2000;
  void* arena = nrq_cdmm_arena_create(256 * 1024, 0);
  void* pool = nrq_cdmm_pool_create(32, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(arena, nullptr);
  ASSERT_NE(pool, nullptr);
  std::vector<std::vector<void*>> arena_blocks(kThreads);
  std::vector<std::vector<void*>> pool_slots(kThreads);
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (size_t worker = 0; worker < kThreads; ++worker) {
    workers.emplace_back([&, worker] {
      arena_blocks[worker].reserve(kPerThread);
      pool_slots[worker].reserve(kPerThread);
      for (size_t index = 0; index < kPerThread; ++index) {
        void* block = nrq_cdmm_arena_allocate(arena, 24, 8);
        void* slot = nrq_cdmm_pool_allocate(pool);
        if (!block || !slot) {
          ++failures;
          return;
        }
        arena_blocks[worker].push_back(block);
        pool_slots[worker].push_back(slot);
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  ASSERT_EQ(failures.load(), 0);
  // No address was handed to two owners in either strategy.
  std::set<void*> unique_arena;
  std::set<void*> unique_pool;
  for (size_t worker = 0; worker < kThreads; ++worker) {
    ASSERT_EQ(arena_blocks[worker].size(), kPerThread);
    ASSERT_EQ(pool_slots[worker].size(), kPerThread);
    for (void* block : arena_blocks[worker])
      ASSERT_TRUE(unique_arena.insert(block).second);
    for (void* slot : pool_slots[worker])
      ASSERT_TRUE(unique_pool.insert(slot).second);
  }
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{kThreads * kPerThread});
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{kThreads * kPerThread * 24});

  // Releasing from several threads at once returns every slot exactly once.
  std::atomic<int> releases{0};
  std::vector<std::thread> releasers;
  for (size_t worker = 0; worker < kThreads; ++worker) {
    releasers.emplace_back([&, worker] {
      for (void* slot : pool_slots[worker]) {
        if (nrq_cdmm_pool_release(pool, slot)) ++releases;
      }
    });
  }
  for (std::thread& worker : releasers) worker.join();
  ASSERT_EQ(releases.load(), static_cast<int>(kThreads * kPerThread));
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmStressTest, DestroyUnregistersEveryAddressRange) {
  for (int round = 0; round < 50; ++round) {
    void* arena = nrq_cdmm_arena_create(4096, 0);
    void* pool = nrq_cdmm_pool_create(16, 8, 0, 0, nullptr, nullptr);
    ASSERT_NE(arena, nullptr);
    ASSERT_NE(pool, nullptr);
    void* arena_block = nrq_cdmm_arena_allocate(arena, 16, 8);
    void* pool_slot = nrq_cdmm_pool_allocate(pool);
    ASSERT_EQ(nrq_cdmm_strategy_of(arena_block), NRQ_CDMM_STRATEGY_ARENA);
    ASSERT_EQ(nrq_cdmm_strategy_of(pool_slot), NRQ_CDMM_STRATEGY_POOL);
    ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
    ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
    // A released chunk's range must not keep answering for memory the process
    // may hand to something else next.
    ASSERT_EQ(nrq_cdmm_strategy_of(arena_block), NRQ_CDMM_STRATEGY_NONE);
    ASSERT_EQ(nrq_cdmm_strategy_of(pool_slot), NRQ_CDMM_STRATEGY_NONE);
  }
}

TEST(CdmmStressTest, StrategiesDoNotAliasEachOthersStorage) {
  std::set<void*> addresses;
  void* arena = nrq_cdmm_arena_create(64 * 1024, 0);
  void* pool = nrq_cdmm_pool_create(64, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(arena, nullptr);
  ASSERT_NE(pool, nullptr);
  std::vector<void*> arena_blocks;
  std::vector<void*> pool_slots;
  for (int index = 0; index < 64; ++index) {
    void* block = nrq_cdmm_arena_allocate(arena, 64, 8);
    void* slot = nrq_cdmm_pool_allocate(pool);
    ASSERT_NE(block, nullptr);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(addresses.insert(block).second);
    ASSERT_TRUE(addresses.insert(slot).second);
    arena_blocks.push_back(block);
    pool_slots.push_back(slot);
  }
  // Reset one strategy: the other keeps its contents and liveness.
  std::memset(pool_slots[0], 0x33, 64);
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(static_cast<unsigned char*>(pool_slots[0])[63],
            static_cast<unsigned char>(0x33));
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{64});
  // Reset rewinds the arena but keeps its first chunk, so the addresses still
  // belong to a live arena and still name arena storage. They are no longer
  // allocated, which `used` reports; the table must not have been dropped.
  ASSERT_EQ(nrq_cdmm_arena_used(arena), uint64_t{0});
  for (void* block : arena_blocks) {
    ASSERT_EQ(nrq_cdmm_strategy_of(block), NRQ_CDMM_STRATEGY_ARENA);
    ASSERT_TRUE(nrq_cdmm_arena_contains(arena, block));
  }
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmStressTest, OrdinaryOwnedAllocationIsUnaffectedByBothStrategies) {
  void* arena = nrq_cdmm_arena_create(64 * 1024, 0);
  void* pool = nrq_cdmm_pool_create(32, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(arena, nullptr);
  ASSERT_NE(pool, nullptr);
  for (int round = 0; round < 200; ++round) {
    void* owned = nrq_memory_allocate(48, 8);
    ASSERT_NE(owned, nullptr);
    std::memset(owned, 0x42, 48);
    ASSERT_NE(nrq_cdmm_arena_allocate(arena, 48, 8), nullptr);
    ASSERT_NE(nrq_cdmm_pool_allocate(pool), nullptr);
    ASSERT_EQ(static_cast<unsigned char*>(owned)[47],
              static_cast<unsigned char>(0x42));
    nrq_memory_deallocate_checked(owned);
  }
  ASSERT_EQ(nrq_cdmm_arena_reset(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_arena_destroy(arena), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}
