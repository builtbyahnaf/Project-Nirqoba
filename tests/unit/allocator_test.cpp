#include "test_harness.h"

#include <cstdint>
#include <atomic>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

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
