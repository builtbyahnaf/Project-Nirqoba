// Unit tests for the development runtime's observation layer.
//
// The test binary links nirqoba_runtime_debug (see tests/CMakeLists.txt), so these
// exercise the exact code a --debug-enabled build runs. The production runtime
// is covered end to end by tests/run_tests.py, which never passes the flag.

#include "test_harness.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "nirqoba/Driver/RuntimeConfiguration.h"
#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/gc_debug.h"

// The runtime's private statement of its own configuration. The test binary
// links the development library, so both halves below must agree on that.
#include "runtime_config.h"

namespace {

struct DebugNode {
  DebugNode* next;
  uint64_t value;
};

void trace_debug_node(void* object, size_t object_bytes, nrq_gc_visit_fn visit,
                      void* context) {
  (void)object_bytes;
  auto* node = static_cast<DebugNode*>(object);
  visit(node->next, context);
}

void drain_queued_events() {
  nrq_gc_event scratch[256];
  while (nrq_gc_debug_drain_events(scratch, 256) == 256) {
  }
}

void drain_queued_tags() {
  nrq_gc_debug_tag scratch[256];
  while (nrq_gc_debug_drain_tags(scratch, 256) == 256) {
  }
}

}  // namespace

TEST(GcDebugTest, ReportsTheDevelopmentConfiguration) {
  ASSERT_EQ(nrq_runtime_debug_enabled(), int32_t{1});
  ASSERT_TRUE(nirq::runtime_debug_enabled);
  ASSERT_TRUE(nirq::runtime_debug_enabled);
}

TEST(GcDebugTest, RuntimeAndCompilerAgreeOnTheConfiguration) {
  // The compiler derives the ABI symbol, the library file name and whether
  // instrumentation is emitted from one enum; the runtime derives the same names
  // from one macro. If either side is renamed alone, a program built here would
  // reference a symbol its library does not define, and the failure would only
  // appear at link time in a user's build.
  constexpr nirq::RuntimeConfiguration configuration =
      nirq::runtime_configuration(nirq::runtime_debug_enabled);
  ASSERT_TRUE(nirq::is_development(configuration));
  ASSERT_TRUE(std::strcmp(nirq::runtime_abi_symbol(configuration),
                          nirq::runtime_abi_symbol_name) == 0);
  ASSERT_TRUE(std::strcmp(nirq::runtime_library_name(configuration),
                          nirq::runtime_library_file_name) == 0);
  // And the library actually linked reports the configuration those names
  // describe.
  ASSERT_EQ(nrq_runtime_debug_enabled(),
            nirq::runtime_debug_enabled ? int32_t{1} : int32_t{0});
}

TEST(GcDebugTest, RecordsOnlyTheSelectedEventKinds) {
  // Nothing selected means nothing recorded: an ordinary development run pays a
  // relaxed load per candidate event and writes no records.
  nrq_gc_debug_set_event_mask(0);
  drain_queued_events();
  nrq_gc_event scratch[16];
  void* quiet = nrq_gc_allocate(sizeof(DebugNode), trace_debug_node);
  ASSERT_NE(quiet, nullptr);
  ASSERT_EQ(nrq_gc_debug_drain_events(scratch, 16), size_t{0});

  nrq_gc_debug_set_event_mask(NRQ_GC_EVENT_ALLOCATION);
  void* recorded = nrq_gc_allocate(sizeof(DebugNode), trace_debug_node);
  ASSERT_NE(recorded, nullptr);

  nrq_gc_event events[16];
  const size_t count = nrq_gc_debug_drain_events(events, 16);
  ASSERT_TRUE(count != 0);

  bool found = false;
  for (size_t index = 0; index < count; ++index) {
    if (events[index].kind != NRQ_GC_EVENT_ALLOCATION) continue;
    if (events[index].address !=
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(recorded)))
      continue;
    found = true;
    ASSERT_EQ(events[index].value, static_cast<uint64_t>(sizeof(DebugNode)));
    ASSERT_TRUE(events[index].sequence != 0);
  }
  ASSERT_TRUE(found);
  nrq_gc_debug_set_event_mask(0);
}

TEST(GcDebugTest, TagsAllocationsWithDeclaredSourceSites) {
  drain_queued_tags();
  // Stable hash of (file, line) as the compiler will emit it; the runtime only
  // has to key the table by whatever id it is given.
  const uint64_t site = UINT64_C(0x9f3c5a1b7e2d4608);
  const char* path = "tests/fixtures/site.nrqba";
  nrq_gc_debug_declare_site(site, path, std::strlen(path), 42);
  // Declaring the same site again must not disturb the mapping.
  nrq_gc_debug_declare_site(site, path, std::strlen(path), 42);

  void* object = nrq_gc_allocate(sizeof(DebugNode), trace_debug_node);
  ASSERT_NE(object, nullptr);
  nrq_gc_debug_tag_allocation(object, site);

  nrq_gc_debug_tag tags[16];
  const size_t count = nrq_gc_debug_drain_tags(tags, 16);
  ASSERT_TRUE(count != 0);
  bool found = false;
  for (size_t index = 0; index < count; ++index) {
    if (tags[index].object !=
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(object)))
      continue;
    ASSERT_EQ(tags[index].site, site);
    found = true;
  }
  ASSERT_TRUE(found);

  char file[64] = {};
  uint32_t line = 0;
  ASSERT_EQ(nrq_gc_debug_describe_site(site, file, sizeof(file), &line),
            int32_t{1});
  ASSERT_EQ(std::strcmp(file, path), 0);
  ASSERT_EQ(line, uint32_t{42});
  ASSERT_EQ(nrq_gc_debug_describe_site(site, nullptr, 0, nullptr),
            int32_t{1});
  ASSERT_EQ(nrq_gc_debug_describe_site(site + 1, file, sizeof(file), &line),
            int32_t{0});
}

TEST(GcDebugTest, SnapshotsRootSlotsWithTheirReferents) {
  void* local = nrq_gc_allocate(sizeof(DebugNode), trace_debug_node);
  ASSERT_NE(local, nullptr);
  const uint64_t frame = nrq_gc_root_frame_begin();
  nrq_gc_root_register(reinterpret_cast<void**>(&local));

  std::vector<nrq_gc_debug_root> roots(128);
  const size_t written = nrq_gc_debug_snapshot_roots(roots.data(), roots.size());
  ASSERT_TRUE(written != 0);

  bool found = false;
  for (size_t index = 0; index < written; ++index) {
    if (roots[index].slot !=
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&local)))
      continue;
    found = true;
    ASSERT_EQ(roots[index].referent,
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(local)));
    ASSERT_EQ(roots[index].reference_count, uint32_t{1});
    ASSERT_EQ(roots[index].element_count, uint32_t{1});
    ASSERT_EQ(roots[index].is_pointer_root, uint32_t{1});
  }
  ASSERT_TRUE(found);

  nrq_gc_root_frame_end(frame);
  local = nullptr;
}

TEST(GcDebugTest, CountsRecordsLostToAFullRing) {
  nrq_gc_debug_set_event_mask(NRQ_GC_EVENT_ALLOCATION);
  drain_queued_events();
  const uint64_t dropped_before = nrq_gc_debug_dropped_events();
  // The ring holds 1024 records; allocating past that must drop records and
  // report the loss rather than grow or block an allocator.
  for (int index = 0; index < 1500; ++index) {
    void* object = nrq_gc_allocate(sizeof(DebugNode), trace_debug_node);
    ASSERT_NE(object, nullptr);
  }
  ASSERT_TRUE(nrq_gc_debug_dropped_events() > dropped_before);
  drain_queued_events();
  nrq_gc_debug_set_event_mask(0);
}
