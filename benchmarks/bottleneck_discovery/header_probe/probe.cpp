// Object-header and allocation-footprint probe (diagnostic only).
//
// This translation unit includes the real collector source so sizeof, alignof
// and offsetof are taken from the exact definitions the runtime is built with.
// It never runs a collection; it only prints layout facts.
//
//   powershell -File build_probe.ps1 -Rev HEAD
//
// The extracted runtime tree lives under build/bottleneck_discovery/<sha>/.

#define NOMINMAX
#include <cstddef>
#include <cstdio>

#include "gc.cpp"

int main() {
  std::printf("PROBE alignof_max_align_t %zu\n", alignof(std::max_align_t));
  std::printf("PROBE sizeof_max_align_t %zu\n", sizeof(std::max_align_t));
  std::printf("PROBE sizeof_ObjectHeader %zu\n", sizeof(ObjectHeader));
  std::printf("PROBE alignof_ObjectHeader %zu\n", alignof(ObjectHeader));
  std::printf("PROBE sizeof_ReferenceLayout %zu\n", sizeof(ReferenceLayout));
  std::printf("PROBE kGcPageBytes %zu\n", kGcPageBytes);
  std::printf("PROBE kObjectStartAlignment %zu\n", kObjectStartAlignment);

#define FIELD(name)                                                       \
  std::printf("PROBE offset_%s %zu size_%s %zu\n", #name,                 \
              offsetof(ObjectHeader, name), #name,                        \
              sizeof(((ObjectHeader*)nullptr)->name))

  FIELD(next);
  FIELD(mark_next);
  FIELD(page);
  FIELD(size);
  FIELD(element_size);
  FIELD(trace);
  FIELD(marked);
  FIELD(young);
  FIELD(remembered);
  FIELD(age);
  FIELD(birth_epoch);
  FIELD(reference_offsets);
#undef FIELD

  const size_t payloads[] = {0, 8, 16, 24, 32, 40, 48, 64, 96, 120, 128,
                             256, 1024};
  for (size_t payload : payloads) {
    std::printf("PROBE span payload=%zu span=%zu total=%zu\n", payload,
                object_span_bytes(payload),
                sizeof(ObjectHeader) + payload);
  }
  for (size_t payload = 0; payload <= 64; payload += 4) {
    std::printf("PROBE span4 payload=%zu span=%zu\n", payload,
                object_span_bytes(payload));
  }
  return 0;
}
