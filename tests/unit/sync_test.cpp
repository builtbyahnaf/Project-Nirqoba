#include "test_harness.h"

#include <cstdint>
#include <thread>
#include <vector>

#include "nirqoba/runtime/sync.h"

TEST(SyncTest, MutexSerializesConcurrentNativeCallers) {
  void* mutex = nrq_mutex_create();
  ASSERT_NE(mutex, nullptr);
  uint64_t counter = 0;
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([mutex, &counter] {
      for (int iteration = 0; iteration < 1000; ++iteration) {
        nrq_mutex_lock(mutex);
        ++counter;
        nrq_mutex_unlock(mutex);
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  ASSERT_EQ(counter, uint64_t{4000});
  nrq_mutex_destroy(mutex);
}
