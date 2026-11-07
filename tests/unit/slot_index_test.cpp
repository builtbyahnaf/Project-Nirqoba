// tests/unit/slot_index_test.cpp
//
// PO-27A white-box tests for the GC root-slot index.
//
// The earlier regression tests (gc_slot_index_test.cpp) drive the index through
// the public GC API and can only infer its size from timing. These tests
// include runtime/src/slot_index.h and exercise the exact table the collector
// uses, so capacity, tombstone, rehash, compaction, growth and probe-length
// behaviour are asserted directly.
//
// SlotIndex only hashes and compares slot addresses; it never dereferences
// them. The tests therefore synthesise distinct aligned addresses instead of
// allocating storage for every slot.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "slot_index.h"

namespace {

using nirq::gc::detail::SlotIndex;
using nirq::gc::detail::slot_hash;

struct TestSlotState {
  size_t count = 0;
};

using TestIndex = SlotIndex<TestSlotState>;

constexpr size_t kInitialCapacity = TestIndex::kInitialCapacity;

void* slot_address(size_t index) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000) +
                                 static_cast<uintptr_t>(index) * 16);
}

// Returns `count` distinct addresses that all land in the same initial bucket
// for a table of `capacity` entries. Inserting them with linear probing builds
// a deterministic collision chain regardless of allocator address layout.
std::vector<void*> colliding_slots(size_t capacity, size_t count) {
  const size_t mask = capacity - 1;
  std::vector<std::vector<void*>> buckets(capacity);
  for (size_t index = 1; index < capacity * count * 64; ++index) {
    void* candidate = slot_address(index);
    std::vector<void*>& bucket = buckets[slot_hash(candidate) & mask];
    bucket.push_back(candidate);
    if (bucket.size() == count) return bucket;
  }
  return {};
}

bool is_power_of_two(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Empty-table behaviour: every operation must be a cheap no-op, and the first
// insertion must allocate the initial table.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, EmptyTableOperationsAreSafe) {
  TestIndex index;

  ASSERT_EQ(index.size(), size_t{0});
  ASSERT_EQ(index.capacity(), size_t{0});
  ASSERT_EQ(index.find(slot_address(1)), nullptr);
  ASSERT_FALSE(index.release(slot_address(1), 1));
  ASSERT_EQ(index.insert(nullptr), nullptr);
  ASSERT_EQ(index.insert_for_testing(nullptr, false), nullptr);

  const TestIndex::Diagnostics empty = index.diagnostics();
  ASSERT_EQ(empty.capacity, size_t{0});
  ASSERT_EQ(empty.live_entries, size_t{0});
  ASSERT_EQ(empty.tombstones, size_t{0});
  ASSERT_EQ(empty.rehash_count, size_t{0});
  ASSERT_EQ(empty.compaction_count, size_t{0});
  ASSERT_EQ(empty.growth_count, size_t{0});
  ASSERT_EQ(empty.max_probe_length, size_t{0});

  void* slot = slot_address(1);
  TestSlotState* state = index.insert(slot);
  ASSERT_NE(state, nullptr);
  ASSERT_EQ(index.size(), size_t{1});
  ASSERT_EQ(index.capacity(), kInitialCapacity);
  ASSERT_EQ(index.find(slot), state);
  ASSERT_EQ(index.find(slot_address(2)), nullptr);

  const TestIndex::Diagnostics first = index.diagnostics();
  ASSERT_EQ(first.rehash_count, size_t{1});  // initial allocation only
  ASSERT_EQ(first.growth_count, size_t{0});
  ASSERT_EQ(first.compaction_count, size_t{0});
}

// ---------------------------------------------------------------------------
// Repeated insert/release with one live entry. This is the minimal PO-27
// reproduction: tombstones accumulate forever, live occupancy stays at one.
// Capacity must never grow.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, SingleLiveEntryChurnKeepsCapacityBounded) {
  TestIndex index;
  void* live = slot_address(1);
  TestSlotState* live_state = index.insert(live);
  ASSERT_NE(live_state, nullptr);
  live_state->count = 1;

  for (size_t iteration = 0; iteration < 100'000; ++iteration) {
    void* transient = slot_address(2 + iteration);
    ASSERT_NE(index.insert(transient), nullptr);
    ASSERT_TRUE(index.release(transient, 1));
    ASSERT_EQ(index.size(), size_t{1});
  }

  const TestIndex::Diagnostics stats = index.diagnostics();
  ASSERT_EQ(stats.capacity, kInitialCapacity);
  ASSERT_EQ(stats.live_entries, size_t{1});
  ASSERT_TRUE(stats.tombstones > 0);
  ASSERT_EQ(stats.growth_count, size_t{0});
  ASSERT_TRUE(stats.compaction_count > 0);
  TestSlotState* found = index.find(live);
  ASSERT_NE(found, nullptr);
  ASSERT_EQ(found->count, size_t{1});
}

// ---------------------------------------------------------------------------
// Repeated insert/release with several live entries (still below the growth
// threshold). The table must compact rather than grow.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, SeveralLiveEntriesChurnKeepsCapacityBounded) {
  TestIndex index;
  constexpr size_t kLive = 6;
  std::vector<void*> live;
  for (size_t i = 0; i < kLive; ++i) {
    void* slot = slot_address(i);
    ASSERT_NE(index.insert(slot), nullptr);
    live.push_back(slot);
  }
  ASSERT_EQ(index.size(), kLive);

  for (size_t iteration = 0; iteration < 50'000; ++iteration) {
    void* transient = slot_address(1000 + iteration);
    ASSERT_NE(index.insert(transient), nullptr);
    ASSERT_TRUE(index.release(transient, 1));
    ASSERT_EQ(index.size(), kLive);
  }

  const TestIndex::Diagnostics stats = index.diagnostics();
  ASSERT_EQ(stats.capacity, kInitialCapacity);
  ASSERT_EQ(stats.live_entries, kLive);
  ASSERT_EQ(stats.growth_count, size_t{0});
  ASSERT_TRUE(stats.compaction_count > 0);
  for (void* slot : live) ASSERT_NE(index.find(slot), nullptr);
}

// ---------------------------------------------------------------------------
// Genuine live-entry capacity growth: enough concurrent slots must make the
// table grow, and every entry must survive the rehashes.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, GenuineLiveEntryGrowthGrowsCapacity) {
  TestIndex index;
  constexpr size_t kEntries = 200;
  std::vector<void*> slots;
  slots.reserve(kEntries);
  for (size_t i = 0; i < kEntries; ++i) {
    void* slot = slot_address(i);
    ASSERT_NE(index.insert(slot), nullptr);
    slots.push_back(slot);
  }

  const TestIndex::Diagnostics stats = index.diagnostics();
  ASSERT_EQ(stats.live_entries, kEntries);
  ASSERT_EQ(stats.tombstones, size_t{0});
  ASSERT_TRUE(stats.capacity > kInitialCapacity);
  ASSERT_TRUE(stats.capacity >= kEntries);
  ASSERT_TRUE(is_power_of_two(stats.capacity));
  ASSERT_TRUE(stats.growth_count > 0);
  for (void* slot : slots) ASSERT_NE(index.find(slot), nullptr);

  // Releasing everything leaves tombstones but must not change the capacity;
  // reinserting the same addresses must compact in place instead of growing.
  for (void* slot : slots) ASSERT_TRUE(index.release(slot, 1));
  ASSERT_EQ(index.size(), size_t{0});
  ASSERT_EQ(index.capacity(), stats.capacity);

  for (void* slot : slots) ASSERT_NE(index.insert(slot), nullptr);
  ASSERT_EQ(index.size(), kEntries);
  ASSERT_EQ(index.capacity(), stats.capacity);
  for (void* slot : slots) ASSERT_NE(index.find(slot), nullptr);
}

// ---------------------------------------------------------------------------
// Collision-heavy insertion: eleven addresses share one initial bucket. Every
// lookup must still find its entry, and a later growth must preserve them.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, CollisionHeavyInsertAndLookup) {
  TestIndex index;
  // 11 live entries stay below the 12-entry trigger for a 16-entry table.
  std::vector<void*> colliders = colliding_slots(kInitialCapacity, 11);
  ASSERT_EQ(colliders.size(), size_t{11});

  for (void* slot : colliders) ASSERT_NE(index.insert(slot), nullptr);
  ASSERT_EQ(index.size(), size_t{11});
  ASSERT_EQ(index.capacity(), kInitialCapacity);

  for (void* slot : colliders) ASSERT_NE(index.find(slot), nullptr);
  ASSERT_TRUE(index.diagnostics().max_probe_length >= 8);

  // The next distinct insertion crosses the threshold and doubles capacity.
  void* extra = slot_address(1'000'000);
  ASSERT_NE(index.insert(extra), nullptr);
  const TestIndex::Diagnostics grown = index.diagnostics();
  ASSERT_TRUE(grown.capacity > kInitialCapacity);
  ASSERT_TRUE(grown.growth_count > 0);
  for (void* slot : colliders) ASSERT_NE(index.find(slot), nullptr);
  ASSERT_NE(index.find(extra), nullptr);
}

// ---------------------------------------------------------------------------
// Repeated reuse of the same slot address: insert/release/insert must reuse
// the tombstone, keep exactly one live entry, and never grow.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, RepeatedSlotAddressReuse) {
  TestIndex index;
  void* slot = slot_address(1);

  for (size_t iteration = 0; iteration < 100'000; ++iteration) {
    TestSlotState* state = index.insert(slot);
    ASSERT_NE(state, nullptr);
    state->count = 1;
    ASSERT_EQ(index.size(), size_t{1});
    ASSERT_TRUE(index.release(slot, 1));
    ASSERT_EQ(index.size(), size_t{0});
  }

  const TestIndex::Diagnostics stats = index.diagnostics();
  ASSERT_EQ(stats.capacity, kInitialCapacity);
  ASSERT_EQ(stats.growth_count, size_t{0});
  ASSERT_EQ(index.find(slot), nullptr);

  TestSlotState* state = index.insert(slot);
  ASSERT_NE(state, nullptr);
  ASSERT_EQ(index.find(slot), state);
}

// ---------------------------------------------------------------------------
// Lookup after a same-capacity compaction: the live entry must remain findable
// and its payload must survive the rehash.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, LookupAfterSameCapacityCompaction) {
  TestIndex index;
  void* live = slot_address(1);
  TestSlotState* live_state = index.insert(live);
  ASSERT_NE(live_state, nullptr);
  live_state->count = 7;

  void* last_released = nullptr;
  size_t iteration = 0;
  while (index.diagnostics().compaction_count == 0) {
    ASSERT_TRUE(iteration < 1'000);
    void* transient = slot_address(1000 + iteration);
    ASSERT_NE(index.insert(transient), nullptr);
    ASSERT_TRUE(index.release(transient, 1));
    last_released = transient;
    ++iteration;
  }

  const TestIndex::Diagnostics stats = index.diagnostics();
  ASSERT_EQ(stats.capacity, kInitialCapacity);
  ASSERT_TRUE(stats.compaction_count > 0);
  ASSERT_EQ(stats.live_entries, size_t{1});

  TestSlotState* found = index.find(live);
  ASSERT_NE(found, nullptr);
  ASSERT_EQ(found->count, size_t{7});
  ASSERT_EQ(index.find(last_released), nullptr);
}

// ---------------------------------------------------------------------------
// Lookup after capacity growth.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, LookupAfterCapacityGrowth) {
  TestIndex index;
  constexpr size_t kEntries = 100;
  std::vector<void*> slots;
  for (size_t i = 0; i < kEntries; ++i) {
    void* slot = slot_address(i);
    ASSERT_NE(index.insert(slot), nullptr);
    slots.push_back(slot);
  }

  ASSERT_TRUE(index.capacity() > kInitialCapacity);
  ASSERT_TRUE(index.diagnostics().growth_count > 0);
  for (void* slot : slots) ASSERT_NE(index.find(slot), nullptr);
}

// ---------------------------------------------------------------------------
// Full-table unsuccessful lookup termination. Growth is disabled for the test
// so the table can be filled exactly to capacity; a missing slot must still
// return promptly after a complete, bounded probe.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, FullTableUnsuccessfulLookupTerminates) {
  TestIndex index;
  std::vector<void*> slots;
  for (size_t i = 0; i < kInitialCapacity; ++i) {
    void* slot = slot_address(1000 + i);
    ASSERT_NE(index.insert_for_testing(slot, false), nullptr);
    slots.push_back(slot);
  }

  ASSERT_EQ(index.size(), kInitialCapacity);
  ASSERT_EQ(index.capacity(), kInitialCapacity);

  void* absent = slot_address(1'000'000);
  index.reset_diagnostics();
  ASSERT_EQ(index.find(absent), nullptr);
  ASSERT_EQ(index.diagnostics().max_probe_length, kInitialCapacity);

  // Inserting with growth disabled fails cleanly instead of corrupting.
  ASSERT_EQ(index.insert_for_testing(absent, false), nullptr);
  ASSERT_EQ(index.size(), kInitialCapacity);

  // Releasing an absent slot also terminates after a full probe.
  index.reset_diagnostics();
  ASSERT_FALSE(index.release(absent, 1));
  ASSERT_EQ(index.diagnostics().max_probe_length, kInitialCapacity);

  // After one live entry is released, the free slot accepts the new entry.
  ASSERT_TRUE(index.release(slots.front(), 1));
  ASSERT_NE(index.insert_for_testing(absent, false), nullptr);
  ASSERT_NE(index.find(absent), nullptr);
}

// ---------------------------------------------------------------------------
// Capacity-overflow policy: growth must be refused rather than wrapping, while
// compaction and safe growth still succeed.
// ---------------------------------------------------------------------------
TEST(SlotIndexTest, CapacityOverflowIsRejected) {
  size_t next = 0;

  // Highest power-of-two capacity: doubling it would overflow size_t.
  const size_t largest_capacity = size_t{1} << (sizeof(size_t) * 8 - 1);
  ASSERT_FALSE(TestIndex::select_next_capacity(
      largest_capacity, largest_capacity / 2, &next));

  // High live occupancy doubles.
  ASSERT_TRUE(TestIndex::select_next_capacity(1024, 512, &next));
  ASSERT_EQ(next, size_t{2048});

  // Low live occupancy compacts at the existing capacity.
  ASSERT_TRUE(TestIndex::select_next_capacity(1024, 511, &next));
  ASSERT_EQ(next, size_t{1024});

  // Invalid arguments are rejected.
  ASSERT_FALSE(TestIndex::select_next_capacity(0, 0, &next));
  ASSERT_FALSE(TestIndex::select_next_capacity(16, 8, nullptr));
}
