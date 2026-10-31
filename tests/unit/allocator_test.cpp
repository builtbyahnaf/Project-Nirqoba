#include "test_harness.h"

#include <cstdint>
#include <atomic>
#include <thread>

#include "nirqoba/runtime/allocator.h"

TEST(AllocatorTest, StrategiesHonorAlignmentAndLifecycle) {
  const NrqAllocatorKind kinds[] = {
      NRQ_ALLOCATOR_GENERAL, NRQ_ALLOCATOR_POOL, NRQ_ALLOCATOR_ARENA,
      NRQ_ALLOCATOR_STACK, NRQ_ALLOCATOR_SLAB, NRQ_ALLOCATOR_BUDDY,
      NRQ_ALLOCATOR_FREE_LIST, NRQ_ALLOCATOR_TLSF};
  for (NrqAllocatorKind kind : kinds) {
    void* allocator = nrq_allocator_create(kind, 64 * 1024);
    ASSERT_NE(allocator, nullptr);
    void* previous = nrq_memory_use_allocator(allocator);
    void* pointer = nrq_memory_allocate(37, 64);
    ASSERT_NE(pointer, nullptr);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(pointer) % 64, uintptr_t{0});
    nrq_memory_deallocate_checked(pointer);
    ASSERT_EQ(nrq_memory_use_allocator(previous), allocator);
    ASSERT_EQ(nrq_allocator_reset(allocator), int32_t{1});
    ASSERT_EQ(nrq_allocator_destroy(allocator), int32_t{1});
  }
}

TEST(AllocatorTest, PoolRejectsRequestsLargerThanItsCapacity) {
  void* allocator = nrq_allocator_create(NRQ_ALLOCATOR_POOL, 32);
  ASSERT_NE(allocator, nullptr);
  void* previous = nrq_memory_use_allocator(allocator);
  ASSERT_EQ(nrq_memory_allocate(64, 8), nullptr);
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
  ASSERT_EQ(profile.peak_bytes, uint64_t{32});
  ASSERT_EQ(profile.allocations, uint64_t{1});
  ASSERT_EQ(profile.deallocations, uint64_t{1});
  nrq_memory_profiler_stop();
}
