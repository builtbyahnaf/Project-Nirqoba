#include "test_harness.h"

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

#include "nirqoba/runtime/allocator.h"

using namespace std;

namespace {

struct CustomAllocatorState {
  size_t last_bytes = 0;
  size_t last_alignment = 0;
  int allocations = 0;
  int deallocations = 0;
  bool destroyed = false;
  bool fail_allocation = false;
};

void* custom_allocate(void* context, size_t bytes, size_t alignment) {
  auto* state = static_cast<CustomAllocatorState*>(context);
  if (state->fail_allocation) return nullptr;
  state->last_bytes = bytes;
  state->last_alignment = alignment;
  ++state->allocations;
  return _aligned_malloc(bytes, std::max(alignment, sizeof(void*)));
}

void custom_deallocate(void* context, void* pointer, size_t bytes,
                       size_t alignment) {
  auto* state = static_cast<CustomAllocatorState*>(context);
  state->last_bytes = bytes;
  state->last_alignment = alignment;
  ++state->deallocations;
  _aligned_free(pointer);
}

void custom_destroy(void* context) {
  static_cast<CustomAllocatorState*>(context)->destroyed = true;
}

}  // namespace

TEST(AllocatorTest, StrategiesHonorAlignmentAndLifecycle) {
  const NrqAllocatorKind kinds[] = {
      NRQ_ALLOCATOR_GENERAL, NRQ_ALLOCATOR_POOL, NRQ_ALLOCATOR_ARENA,
      NRQ_ALLOCATOR_STACK, NRQ_ALLOCATOR_SLAB, NRQ_ALLOCATOR_BUDDY,
      NRQ_ALLOCATOR_FREE_LIST, NRQ_ALLOCATOR_TLSF};
  for (NrqAllocatorKind kind : kinds) {
    void* allocator = nrq_allocator_create(kind, 64 * 1024);
    ASSERT_NE(allocator, nullptr);
    ASSERT_EQ(nrq_memory_allocator_kind(
                  static_cast<uint64_t>(reinterpret_cast<uintptr_t>(allocator))),
              static_cast<int32_t>(kind));
    void* previous = nrq_memory_use_allocator(allocator);
    void* pointer = nrq_memory_allocate(37, 4096);
    ASSERT_NE(pointer, nullptr);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(pointer) % 4096, uintptr_t{0});
    nrq_memory_deallocate_checked(pointer);
    ASSERT_EQ(nrq_memory_use_allocator(previous), allocator);
    ASSERT_EQ(nrq_allocator_reset(allocator), int32_t{1});
    ASSERT_EQ(nrq_allocator_destroy(allocator), int32_t{1});
  }
  ASSERT_EQ(nrq_memory_allocator_kind(uint64_t{0}),
            static_cast<int32_t>(NRQ_ALLOCATOR_GENERAL));
  ASSERT_EQ(nrq_memory_allocator_kind(uint64_t{1}), int32_t{-1});
}

TEST(AllocatorTest, CustomCallbacksReceiveOriginalSizeAndAlignment) {
  CustomAllocatorState state;
  const NrqCustomAllocatorVTable callbacks{
      &state, custom_allocate, custom_deallocate, custom_destroy};
  void* allocator = nrq_allocator_create_custom(&callbacks);
  ASSERT_NE(allocator, nullptr);
  ASSERT_EQ(nrq_memory_allocator_kind(static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(allocator))),
            static_cast<int32_t>(NRQ_ALLOCATOR_CUSTOM));
  void* previous = nrq_memory_use_allocator(allocator);
  state.fail_allocation = true;
  ASSERT_EQ(nrq_memory_allocate(31, 64), nullptr);
  state.fail_allocation = false;
  void* pointer = nrq_memory_allocate(31, 64);
  ASSERT_NE(pointer, nullptr);
  ASSERT_EQ(reinterpret_cast<uintptr_t>(pointer) % 64, uintptr_t{0});
  ASSERT_EQ(state.allocations, 1);
  nrq_memory_deallocate_checked(pointer);
  ASSERT_EQ(state.deallocations, 1);
  ASSERT_EQ(state.last_bytes, size_t{31});
  ASSERT_EQ(state.last_alignment, size_t{64});
  ASSERT_EQ(nrq_memory_use_allocator(previous), allocator);
  ASSERT_EQ(nrq_allocator_destroy(allocator), int32_t{1});
  ASSERT_TRUE(state.destroyed);
}

TEST(AllocatorTest, PoolRejectsRequestsLargerThanItsCapacity) {
  void* allocator = nrq_allocator_create(NRQ_ALLOCATOR_POOL, 32);
  ASSERT_NE(allocator, nullptr);
  void* previous = nrq_memory_use_allocator(allocator);
  ASSERT_EQ(nrq_memory_allocate(64, 8), nullptr);
  ASSERT_EQ(nrq_memory_use_allocator(previous), allocator);
  ASSERT_EQ(nrq_allocator_destroy(allocator), int32_t{1});
}

TEST(AllocatorTest, PerAllocationAllocatorDoesNotChangeThreadSelection) {
  void* allocator = nrq_allocator_create(NRQ_ALLOCATOR_ARENA, 4096);
  ASSERT_NE(allocator, nullptr);
  ASSERT_EQ(nrq_memory_get_allocator(), nullptr);
  void* pointer = nrq_memory_allocate_from(allocator, 48, 16);
  ASSERT_NE(pointer, nullptr);
  ASSERT_EQ(nrq_memory_get_allocator(), nullptr);
  nrq_memory_deallocate_checked(pointer);
  ASSERT_EQ(nrq_allocator_destroy(allocator), int32_t{1});
}

TEST(AllocatorTest, TlsfReusesAndCoalescesMixedSizeBlocks) {
  void* allocator = nrq_allocator_create(NRQ_ALLOCATOR_TLSF, 64 * 1024);
  ASSERT_NE(allocator, nullptr);
  void* previous = nrq_memory_use_allocator(allocator);
  std::vector<void*> allocations;
  for (size_t index = 0; index < 80; ++index) {
    const size_t bytes = 17 + (index * 37) % 511;
    const size_t alignment = size_t{1} << (3 + index % 5);
    void* pointer = nrq_memory_allocate(bytes, alignment);
    ASSERT_NE(pointer, nullptr);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(pointer) % alignment, uintptr_t{0});
    allocations.push_back(pointer);
  }
  for (auto pointer = allocations.rbegin(); pointer != allocations.rend(); ++pointer)
    nrq_memory_deallocate_checked(*pointer);
  void* large = nrq_memory_allocate(48 * 1024, 16);
  ASSERT_NE(large, nullptr);
  nrq_memory_deallocate_checked(large);
  ASSERT_EQ(nrq_memory_use_allocator(previous), allocator);
  ASSERT_EQ(nrq_allocator_destroy(allocator), int32_t{1});
}

TEST(AllocatorTest, ConcurrentUsersKeepAllocatorAliveUntilThreadExit) {
  void* allocator = nrq_allocator_create(NRQ_ALLOCATOR_GENERAL, 0);
  ASSERT_NE(allocator, nullptr);
  std::atomic<bool> completed = false;
  std::thread worker([allocator, &completed] {
    nrq_memory_use_allocator(allocator);
    for (int index = 0; index < 256; ++index) {
      void* pointer = nrq_memory_allocate(24, 16);
      if (!pointer) return;
      nrq_memory_deallocate_checked(pointer);
    }
    completed = true;
    // Deliberately leave the allocator selected: the thread-local destructor
    // must release its selection when this worker exits.
  });
  worker.join();
  ASSERT_TRUE(completed.load());
  ASSERT_EQ(nrq_allocator_destroy(allocator), int32_t{1});
}

TEST(AllocatorTest, SecureBuffersAreZeroedAndProfilerTracksLifetime) {
  nrq_memory_profiler_start();
  auto* secret = static_cast<unsigned char*>(nrq_memory_secure_allocate(32, 16));
  ASSERT_NE(secret, nullptr);
  ASSERT_EQ(secret[0], static_cast<unsigned char>(0));
  ASSERT_EQ(nrq_memory_profiler_get_current_bytes(), uint64_t{32});
  secret[0] = 0xA5;
  nrq_memory_secure_zero(secret, 32);
  ASSERT_EQ(secret[0], static_cast<unsigned char>(0));
  nrq_memory_secure_release(secret);
  const NrqMemoryProfile profile = nrq_memory_profiler_capture();
  ASSERT_EQ(profile.current_bytes, uint64_t{0});
  ASSERT_EQ(profile.total_bytes, uint64_t{32});
  ASSERT_EQ(profile.peak_bytes, uint64_t{32});
  ASSERT_EQ(profile.allocations, uint64_t{1});
  ASSERT_EQ(profile.deallocations, uint64_t{1});
  nrq_memory_profiler_stop();
}

TEST(AllocatorTest, FileMappingsReportSizeAndUnmapDeterministically) {
  wchar_t directory[MAX_PATH]{};
  ASSERT_TRUE(GetTempPathW(MAX_PATH, directory) != 0);
  wchar_t path[MAX_PATH]{};
  ASSERT_TRUE(GetTempFileNameW(directory, L"nrq", 0, path) != 0);
  HANDLE file = CreateFileW(path, GENERIC_WRITE | GENERIC_READ, 0, nullptr,
      CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  const unsigned char original[] = {1, 2, 3, 4};
  DWORD written = 0;
  ASSERT_TRUE(WriteFile(file, original, sizeof(original), &written, nullptr));
  ASSERT_EQ(written, static_cast<DWORD>(sizeof(original)));
  CloseHandle(file);

  const int path_bytes = WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                              nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>(path_bytes), '\0');
  ASSERT_TRUE(WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(),
                                  path_bytes, nullptr, nullptr) != 0);
  void* mapping = nrq_memory_map_file(utf8.c_str(), 1);
  ASSERT_NE(mapping, nullptr);
  ASSERT_EQ(nrq_memory_mapping_size(mapping), uint64_t{4});
  auto* bytes = static_cast<unsigned char*>(mapping);
  ASSERT_EQ(bytes[0], static_cast<unsigned char>(1));
  unsigned char value = 0;
  ASSERT_EQ(nrq_memory_mapping_read(mapping, 1, 1, &value), int32_t{1});
  ASSERT_EQ(value, static_cast<unsigned char>(2));
  const unsigned char replacement = 9;
  ASSERT_EQ(nrq_memory_mapping_write(mapping, 0, 1, &replacement), int32_t{1});
  ASSERT_EQ(nrq_memory_unmap_file(mapping), int32_t{1});
  ASSERT_EQ(nrq_memory_mapping_size(mapping), uint64_t{0});
  ASSERT_EQ(nrq_memory_unmap_file(mapping), int32_t{0});

  file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  unsigned char updated[sizeof(original)]{};
  DWORD read = 0;
  ASSERT_TRUE(ReadFile(file, updated, sizeof(updated), &read, nullptr));
  ASSERT_EQ(updated[0], static_cast<unsigned char>(9));
  CloseHandle(file);
  DeleteFileW(path);
}

TEST(AllocatorTest, ReallocatePreservesContentsAndAllocatorIdentity) {
  // Reallocation must keep the owning allocator and the alignment recorded when
  // the block was created, and it must carry the surviving bytes across.
  void* region = nrq_allocator_create(NRQ_ALLOCATOR_FREE_LIST, 64 * 1024);
  ASSERT_NE(region, nullptr);
  auto* first = static_cast<unsigned char*>(nrq_memory_allocate_from(region, 8, 16));
  ASSERT_NE(first, nullptr);
  for (int index = 0; index < 8; ++index)
    first[index] = static_cast<unsigned char>(index + 1);

  auto* grown = static_cast<unsigned char*>(nrq_memory_reallocate(first, 4096));
  ASSERT_NE(grown, nullptr);
  ASSERT_EQ(reinterpret_cast<uintptr_t>(grown) % 16, uintptr_t{0});
  for (int index = 0; index < 8; ++index)
    ASSERT_EQ(static_cast<int>(grown[index]), index + 1);

  // The block still belongs to its original allocator, so destroying the
  // allocator must fail while the resized allocation is live.
  ASSERT_EQ(nrq_allocator_destroy(region), int32_t{0});

  auto* shrunk = static_cast<unsigned char*>(nrq_memory_reallocate(grown, 8));
  ASSERT_NE(shrunk, nullptr);
  ASSERT_EQ(static_cast<int>(shrunk[7]), 8);
  nrq_memory_deallocate_checked(shrunk);
  ASSERT_EQ(nrq_allocator_destroy(region), int32_t{1});
}

TEST(AllocatorTest, ReallocateRespectsStackAndRegionLifetimeRules) {
  // A stack allocator can only resize its most recent block, because releasing
  // anything else would break the LIFO order it guarantees.
  void* stack = nrq_allocator_create(NRQ_ALLOCATOR_STACK, 64 * 1024);
  ASSERT_NE(stack, nullptr);
  auto* below = static_cast<unsigned char*>(nrq_memory_allocate_from(stack, 64, 8));
  auto* top = static_cast<unsigned char*>(nrq_memory_allocate_from(stack, 64, 8));
  ASSERT_NE(below, nullptr);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(nrq_memory_reallocate(below, 128), nullptr);
  auto* resized = static_cast<unsigned char*>(nrq_memory_reallocate(top, 128));
  ASSERT_TRUE(resized == top);
  nrq_memory_deallocate_checked(resized);
  nrq_memory_deallocate_checked(below);
  ASSERT_EQ(nrq_allocator_destroy(stack), int32_t{1});

  // A region allocator extends its bump pointer when the block is the most
  // recent allocation, which keeps the address stable.
  void* arena = nrq_allocator_create(NRQ_ALLOCATOR_ARENA, 64 * 1024);
  ASSERT_NE(arena, nullptr);
  auto* block = static_cast<unsigned char*>(nrq_memory_allocate_from(arena, 64, 8));
  ASSERT_NE(block, nullptr);
  block[0] = 3;
  auto* extended = static_cast<unsigned char*>(nrq_memory_reallocate(block, 1024));
  ASSERT_TRUE(extended == block);
  ASSERT_EQ(static_cast<int>(extended[0]), 3);
  nrq_memory_deallocate_checked(extended);
  ASSERT_EQ(nrq_allocator_destroy(arena), int32_t{1});
}

TEST(AllocatorTest, DefaultAllocatorPolicyGovernsExplicitAllocations) {
  void* pool = nrq_allocator_create(NRQ_ALLOCATOR_POOL, 64);
  ASSERT_NE(pool, nullptr);
  ASSERT_EQ(nrq_memory_allocator_kind(0), int32_t{NRQ_ALLOCATOR_GENERAL});
  ASSERT_EQ(nrq_memory_set_default_allocator(pool), nullptr);
  // Handle zero now means "the configured default", so the kind follows it.
  ASSERT_EQ(nrq_memory_allocator_kind(0), int32_t{NRQ_ALLOCATOR_POOL});
  ASSERT_EQ(nrq_memory_get_default_allocator(), pool);
  // The policy default cannot be destroyed while allocations may still land in
  // it, and it must be restorable to the general-purpose default.
  ASSERT_EQ(nrq_allocator_destroy(pool), int32_t{0});

  auto* value = static_cast<unsigned char*>(nrq_memory_allocate(8, 8));
  ASSERT_NE(value, nullptr);
  value[0] = 42;
  ASSERT_EQ(static_cast<int>(value[0]), 42);
  nrq_memory_deallocate_checked(value);

  ASSERT_EQ(nrq_memory_set_default_allocator(nullptr), pool);
  ASSERT_EQ(nrq_memory_get_default_allocator(), nullptr);
  ASSERT_EQ(nrq_memory_allocator_kind(0), int32_t{NRQ_ALLOCATOR_GENERAL});
  ASSERT_EQ(nrq_allocator_destroy(pool), int32_t{1});
}

TEST(AllocatorTest, ReportsFreeSpaceAndFragmentation) {
  const size_t capacity = 64 * 1024;
  void* region = nrq_allocator_create(NRQ_ALLOCATOR_FREE_LIST, capacity);
  ASSERT_NE(region, nullptr);
  const uint64_t handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(region));
  ASSERT_EQ(nrq_memory_allocator_free_bytes(handle), static_cast<uint64_t>(capacity));
  ASSERT_EQ(nrq_memory_allocator_fragmentation_bytes(handle), uint64_t{0});

  auto* first = static_cast<unsigned char*>(nrq_memory_allocate_from(region, 1024, 8));
  auto* second = static_cast<unsigned char*>(nrq_memory_allocate_from(region, 1024, 8));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(nrq_memory_allocator_free_bytes(handle),
            static_cast<uint64_t>(capacity - 2048));

  // Freeing the lower block leaves a hole that cannot serve the largest
  // remaining request; that difference is the fragmentation estimate.
  nrq_memory_deallocate_checked(first);
  ASSERT_EQ(nrq_memory_allocator_free_bytes(handle),
            static_cast<uint64_t>(capacity - 1024));
  ASSERT_EQ(nrq_memory_allocator_fragmentation_bytes(handle), uint64_t{1024});

  nrq_memory_deallocate_checked(second);
  ASSERT_EQ(nrq_memory_allocator_fragmentation_bytes(handle), uint64_t{0});
  ASSERT_EQ(nrq_allocator_destroy(region), int32_t{1});
}

TEST(AllocatorTest, ProfilerReportsSizeHistogramAndAllocatorAttribution) {
  const NrqMemoryProfile before = nrq_memory_profiler_capture();
  nrq_memory_profiler_start();
  void* pool = nrq_allocator_create(NRQ_ALLOCATOR_POOL, 64);
  void* arena = nrq_allocator_create(NRQ_ALLOCATOR_ARENA, 64 * 1024);
  ASSERT_NE(pool, nullptr);
  ASSERT_NE(arena, nullptr);
  auto* small = nrq_memory_allocate_from(pool, 32, 8);
  auto* large = nrq_memory_allocate_from(arena, 4096, 8);
  ASSERT_NE(small, nullptr);
  ASSERT_NE(large, nullptr);

  const NrqMemoryProfile profile = nrq_memory_profiler_capture();
  ASSERT_EQ(profile.allocations, uint64_t{2});
  ASSERT_EQ(profile.allocations_le_64, uint64_t{1});
  ASSERT_EQ(profile.allocations_le_4096, uint64_t{1});
  ASSERT_EQ(profile.largest_allocation, uint64_t{4096});
  ASSERT_EQ(profile.pool_bytes, before.pool_bytes + 32);
  ASSERT_EQ(profile.arena_bytes, before.arena_bytes + 4096);
  ASSERT_TRUE(profile.free_bytes > before.free_bytes);
  ASSERT_TRUE(profile.largest_free_block > 0);
  ASSERT_EQ(profile.fragmentation_bytes,
            profile.free_bytes > profile.largest_free_block
                ? profile.free_bytes - profile.largest_free_block
                : 0);

  nrq_memory_deallocate_checked(small);
  nrq_memory_deallocate_checked(large);
  nrq_memory_profiler_stop();
  const NrqMemoryProfile after = nrq_memory_profiler_capture();
  ASSERT_EQ(after.pool_bytes, before.pool_bytes);
  ASSERT_EQ(after.arena_bytes, before.arena_bytes);
  nrq_allocator_destroy(pool);
  nrq_allocator_destroy(arena);
}
TEST(AllocatorTest, BenchmarksSmallLargeAndMixedWorkloads) {
  // Timings are recorded for CI visibility; this is not a claim that one
  // strategy is faster than another on arbitrary workloads. Each worker keeps a
  // bounded window of live blocks so region-backed strategies never run out of
  // space, and the measured work stays comparable across strategies.
  constexpr int kSmallAllocations = 20000;
  constexpr int kMixedAllocations = 4000;
  constexpr size_t kLiveWindow = 128;
  const NrqAllocatorKind kinds[] = {NRQ_ALLOCATOR_GENERAL, NRQ_ALLOCATOR_POOL,
                                    NRQ_ALLOCATOR_TLSF};
  for (NrqAllocatorKind kind : kinds) {
    // A pool's capacity is its largest request class, not a region size, so it
    // is sized to the largest block this benchmark asks for.
    const size_t capacity = kind == NRQ_ALLOCATOR_POOL ? 4096 : 1024 * 1024;
    void* allocator = nrq_allocator_create(kind, capacity);
    ASSERT_NE(allocator, nullptr);
    std::vector<void*> blocks;
    blocks.reserve(kLiveWindow);

    const auto small_start = std::chrono::steady_clock::now();
    for (int index = 0; index < kSmallAllocations; ++index) {
      void* block = nrq_memory_allocate_from(allocator, 48, 8);
      ASSERT_NE(block, nullptr);
      blocks.push_back(block);
      if (blocks.size() == kLiveWindow) {
        for (void* live : blocks) nrq_memory_deallocate_checked(live);
        blocks.clear();
      }
    }
    const auto small_end = std::chrono::steady_clock::now();
    for (void* live : blocks) nrq_memory_deallocate_checked(live);
    blocks.clear();

    const auto mixed_start = std::chrono::steady_clock::now();
    for (int index = 0; index < kMixedAllocations; ++index) {
      const size_t bytes = 16 + static_cast<size_t>(index % 32) * 128;
      void* block = nrq_memory_allocate_from(allocator, bytes, 8);
      ASSERT_NE(block, nullptr);
      blocks.push_back(block);
      if (blocks.size() == kLiveWindow) {
        for (void* live : blocks) nrq_memory_deallocate_checked(live);
        blocks.clear();
      }
    }
    const auto mixed_end = std::chrono::steady_clock::now();
    for (void* live : blocks) nrq_memory_deallocate_checked(live);
    blocks.clear();

    const auto small_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        small_end - small_start).count();
    const auto mixed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        mixed_end - mixed_start).count();
    std::cout << "[benchmark] allocator kind " << static_cast<int>(kind) << ": "
              << kSmallAllocations << " small allocations in " << small_ns / 1000
              << " us, " << kMixedAllocations << " mixed allocations in "
              << mixed_ns / 1000 << " us" << std::endl;
    ASSERT_EQ(nrq_allocator_destroy(allocator), int32_t{1});
  }
}
