// CDMM Phase 5: explicit fixed-slot pools.
//
// The pool's job is reuse: uniform slots, one liveness bit per slot, a free list
// in the slots themselves, and refusals that keep a double release or a stale
// pointer from corrupting the next allocation.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "nirqoba/runtime/array.h"
#include "nirqoba/runtime/cdmm.h"

namespace {

int pool_destructor_calls = 0;
unsigned char observed_first_byte = 0;

void count_destructor(void* object, void* context) {
  (void)context;
  ++pool_destructor_calls;
  // The destructor runs before the slot is poisoned, so it still sees whatever
  // the allocation last wrote.
  if (object) observed_first_byte = *static_cast<unsigned char*>(object);
}

}  // namespace

TEST(CdmmPoolTest, AllocatesDistinctAlignedSlotsAndReusesReleasedOnes) {
  void* pool = nrq_cdmm_pool_create(64, 16, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  ASSERT_EQ(nrq_cdmm_pool_slot_bytes(pool), uint64_t{64});
  std::vector<void*> slots;
  std::set<void*> unique;
  for (int index = 0; index < 200; ++index) {
    void* slot = nrq_cdmm_pool_allocate(pool);
    ASSERT_NE(slot, nullptr);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(slot) % 16, uintptr_t{0});
    slots.push_back(slot);
    unique.insert(slot);
  }
  ASSERT_EQ(unique.size(), slots.size());
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{200});
  void* released = slots[100];
  ASSERT_EQ(nrq_cdmm_pool_release(pool, released), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{199});
  // The free list is LIFO, so the slot just released is handed back first.
  ASSERT_EQ(nrq_cdmm_pool_allocate(pool), released);
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, DoubleReleaseIsRejected) {
  void* pool = nrq_cdmm_pool_create(32, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(slot, nullptr);
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  // The refused second release did not push the slot twice: one allocation still
  // gets it.
  ASSERT_EQ(nrq_cdmm_pool_allocate(pool), slot);
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, ForeignAndInteriorAddressesAreRejected) {
  void* pool = nrq_cdmm_pool_create(64, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(slot, nullptr);
  int local = 0;
  ASSERT_EQ(nrq_cdmm_pool_release(pool, &local), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, static_cast<unsigned char*>(slot) + 8),
            int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{1});
  ASSERT_TRUE(nrq_cdmm_pool_contains(pool, slot));
  ASSERT_FALSE(nrq_cdmm_pool_contains(pool, &local));
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, GenerationsRejectAReleaseOfAReusedSlot) {
  void* pool = nrq_cdmm_pool_create(32, 8, 0, NRQ_CDMM_POOL_GENERATIONS, nullptr,
                                    nullptr);
  ASSERT_NE(pool, nullptr);
  void* first = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(first, nullptr);
  const uint64_t first_generation = nrq_cdmm_pool_generation(pool, first);
  ASSERT_NE(first_generation, uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, first), int32_t{1});
  void* second = nrq_cdmm_pool_allocate(pool);
  ASSERT_EQ(second, first);
  const uint64_t second_generation = nrq_cdmm_pool_generation(pool, second);
  ASSERT_NE(second_generation, first_generation);
  // A stale reference carries the old generation; the address now belongs to a
  // different allocation, so the release must fail even though it looks live.
  ASSERT_EQ(nrq_cdmm_pool_release_checked(pool, second, first_generation),
            int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release_checked(pool, second, second_generation),
            int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, WithoutGenerationsTheGenerationIsZero) {
  void* pool = nrq_cdmm_pool_create(32, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_EQ(nrq_cdmm_pool_generation(pool, slot), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, DestructorRunsOnReleaseAndReset) {
  pool_destructor_calls = 0;
  void* pool = nrq_cdmm_pool_create(16, 8, 8, 0, count_destructor, nullptr);
  ASSERT_NE(pool, nullptr);
  void* first = nrq_cdmm_pool_allocate(pool);
  void* second = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  std::memset(first, 0x7e, 16);
  std::memset(second, 0x5b, 16);
  ASSERT_EQ(nrq_cdmm_pool_release(pool, first), int32_t{1});
  // One destructor call, and the object byte was still readable inside it.
  ASSERT_EQ(pool_destructor_calls, 1);
  ASSERT_EQ(observed_first_byte, static_cast<unsigned char>(0x7e));
  ASSERT_EQ(nrq_cdmm_pool_reset(pool), int32_t{1});
  ASSERT_EQ(observed_first_byte, static_cast<unsigned char>(0x5b));
  ASSERT_EQ(pool_destructor_calls, 2);
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, ResetReclaimsEverySlotWithoutReleasingTheChunks) {
  void* pool = nrq_cdmm_pool_create(48, 8, 16, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  std::vector<void*> slots;
  for (int index = 0; index < 64; ++index) {
    void* slot = nrq_cdmm_pool_allocate(pool);
    ASSERT_NE(slot, nullptr);
    std::memset(slot, 0x7e, 48);
    slots.push_back(slot);
  }
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{64});
  // The committed size after the pool grew to hold every slot; reset reclaims the
  // slots, not the chunks, so this is what has to survive.
  const uint64_t committed = nrq_cdmm_pool_committed(pool);
  ASSERT_EQ(nrq_cdmm_pool_reset(pool), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_committed(pool), committed);
  void* reused = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(reused, nullptr);
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, reused), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, GrowthAddsAChunkOnlyWhenTheFreeListIsEmpty) {
  void* pool = nrq_cdmm_pool_create(32, 8, 8, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  const uint64_t first_chunk = nrq_cdmm_pool_committed(pool);
  std::vector<void*> slots;
  for (int index = 0; index < 8; ++index) {
    slots.push_back(nrq_cdmm_pool_allocate(pool));
    ASSERT_NE(slots.back(), nullptr);
  }
  ASSERT_EQ(nrq_cdmm_pool_committed(pool), first_chunk);
  void* ninth = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(ninth, nullptr);
  ASSERT_TRUE(nrq_cdmm_pool_committed(pool) > first_chunk);
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{9});
  for (void* slot : slots) ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, ninth), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, FactsReportSlotSizeAndNoHeader) {
  // A full-size chunk, so the chunk header is amortized over thousands of slots
  // and the reported per-object metadata is the liveness bit it really is.
  void* pool = nrq_cdmm_pool_create(24, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(slot, nullptr);
  NrqCdmmFacts facts{};
  ASSERT_EQ(nrq_cdmm_object_facts(slot, &facts), int32_t{1});
  ASSERT_EQ(facts.strategy, NRQ_CDMM_STRATEGY_POOL);
  ASSERT_EQ(facts.header_bytes, uint64_t{0});
  ASSERT_EQ(facts.object_bytes, uint64_t{24});
  // One liveness bit per 24-byte slot is under a byte of metadata.
  ASSERT_TRUE(facts.metadata_bytes < uint64_t{16});
  ASSERT_EQ(nrq_cdmm_strategy_of(slot), NRQ_CDMM_STRATEGY_POOL);
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, ScopeBulkAllocatesAndReleasesWithOnePin) {
  void* pool = nrq_cdmm_pool_create(16, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  NrqCdmmPoolScope scope{};
  ASSERT_EQ(nrq_cdmm_pool_begin(pool, &scope), int32_t{1});
  std::vector<void*> slots(128, nullptr);
  ASSERT_EQ(nrq_cdmm_pool_scope_allocate_bulk(&scope, slots.size(), slots.data()),
            slots.size());
  ASSERT_EQ(nrq_cdmm_pool_reset(pool), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{0});
  // Release half through the same scope.
  ASSERT_EQ(nrq_cdmm_pool_scope_release_bulk(&scope, slots.data(), 64),
            size_t{64});
  nrq_cdmm_pool_end(&scope);
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{64});
  ASSERT_EQ(nrq_cdmm_pool_release_bulk(pool, slots.data() + 64, 64), size_t{64});
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

TEST(CdmmPoolTest, DestroyRunsDestructorsAndInvalidatesTheHandle) {
  pool_destructor_calls = 0;
  void* pool = nrq_cdmm_pool_create(16, 8, 8, 0, count_destructor, nullptr);
  ASSERT_NE(pool, nullptr);
  for (int index = 0; index < 3; ++index)
    ASSERT_NE(nrq_cdmm_pool_allocate(pool), nullptr);
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
  ASSERT_EQ(pool_destructor_calls, 3);
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_allocate(pool), nullptr);
  ASSERT_EQ(nrq_cdmm_pool_live(pool), uint64_t{0});
  ASSERT_EQ(nrq_cdmm_pool_slot_bytes(pool), uint64_t{0});
}

TEST(CdmmPoolTest, InvalidCreateArgumentsAreRejected) {
  ASSERT_EQ(nrq_cdmm_pool_create(0, 8, 0, 0, nullptr, nullptr), nullptr);
  ASSERT_EQ(nrq_cdmm_pool_create(16, 0, 0, 0, nullptr, nullptr), nullptr);
  ASSERT_EQ(nrq_cdmm_pool_create(16, 3, 0, 0, nullptr, nullptr), nullptr);
  void* pool = nrq_cdmm_pool_create(16, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  ASSERT_EQ(nrq_cdmm_pool_allocate(nullptr), nullptr);
  ASSERT_EQ(nrq_cdmm_pool_release(pool, nullptr), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_reset(nullptr), int32_t{0});
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}
// A pool slot is reused, so a managed field has to be destroyed before the slot
// goes back on the free list. The registration names the release by a kind in the
// low bits of the context, exactly as the arena does, and the pool consumes the
// record when that object leaves.
TEST(CdmmPoolTest, RegisteredFieldTeardownRunsWhenTheSlotIsReleased) {
  void* pool = nrq_cdmm_pool_create(32, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  void* slot = nrq_cdmm_pool_allocate(pool);
  ASSERT_NE(slot, nullptr);
  void* buffer = nrq_array_create(4, 8, 8);
  ASSERT_NE(buffer, nullptr);
  *static_cast<void**>(slot) = buffer;
  const int32_t live_before = nrq_array_live_count();
  // Kind 3 is `nrq_array_free`; the address is the field's pointer word, which a
  // pool slot keeps eight-byte aligned.
  void* context = reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(slot) | uintptr_t{3});
  ASSERT_EQ(nrq_cdmm_pool_register_teardown(pool, slot, context), int32_t{1});
  ASSERT_EQ(nrq_cdmm_pool_release(pool, slot), int32_t{1});
  ASSERT_EQ(nrq_array_live_count(), live_before - 1);
  // The record belonged to that object, so the next occupant inherits nothing.
  void* reused = nrq_cdmm_pool_allocate(pool);
  ASSERT_EQ(reused, slot);
  *static_cast<void**>(reused) = nullptr;
  ASSERT_EQ(nrq_cdmm_pool_release(pool, reused), int32_t{1});
  ASSERT_EQ(nrq_array_live_count(), live_before - 1);
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
}

// A pool that dies with live slots still runs their teardowns: destroying the
// chunks without them would leak every buffer those objects owned.
TEST(CdmmPoolTest, RegisteredFieldTeardownRunsForLiveSlotsOnPoolDestroy) {
  void* pool = nrq_cdmm_pool_create(32, 8, 0, 0, nullptr, nullptr);
  ASSERT_NE(pool, nullptr);
  const int32_t live_before = nrq_array_live_count();
  for (int index = 0; index < 3; ++index) {
    void* slot = nrq_cdmm_pool_allocate(pool);
    ASSERT_NE(slot, nullptr);
    void* buffer = nrq_array_create(4, 8, 8);
    ASSERT_NE(buffer, nullptr);
    *static_cast<void**>(slot) = buffer;
    void* context = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(slot) | uintptr_t{3});
    ASSERT_EQ(nrq_cdmm_pool_register_teardown(pool, slot, context), int32_t{1});
  }
  ASSERT_EQ(nrq_array_live_count(), live_before + 3);
  ASSERT_EQ(nrq_cdmm_pool_destroy(pool), int32_t{1});
  ASSERT_EQ(nrq_array_live_count(), live_before);
}
