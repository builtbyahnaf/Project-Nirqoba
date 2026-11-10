// What resolving an object's page costs on this machine: today's header field
// against a mask, a mask plus a registry probe, a mask plus a range search, and a
// quantum map that registers every 64 KiB a large page covers.
//
// Build and run:
//   cl /nologo /std:c++20 /O2 /Ob2 /DNDEBUG /EHsc /MD page_of_cost.cpp ^
//      /link /SUBSYSTEM:CONSOLE
//
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

struct Header48 { void* next; void* mark_next; void* page; uint64_t epoch;
                  uint32_t len; uint32_t state; void* layout; };
struct Header32 { void* next; uint64_t epoch; uint32_t len; uint32_t state;
                  void* layout; };

constexpr uintptr_t kPageSize = 65536;

// Open-addressed registry of page bases that are NOT ordinary (large-object
// pages). Empty in the common case, which is the point: it is a probe that has to
// answer "no" quickly.
struct Registry {
  std::vector<uintptr_t> slots;
  size_t mask = 0;
  void init(size_t capacity) { slots.assign(capacity, 0); mask = capacity - 1; }
  void insert(uintptr_t base) {
    size_t i = (base >> 16) & mask;
    while (slots[i] != 0) i = (i + 1) & mask;
    slots[i] = base;
  }
  bool contains(uintptr_t base) const {
    size_t i = (base >> 16) & mask;
    for (;;) {
      const uintptr_t v = slots[i];
      if (v == base) return true;
      if (v == 0) return false;
      i = (i + 1) & mask;
    }
  }
};

int main() {
  const size_t count = 1 << 22;
  std::vector<Header48> h48(count);
  std::vector<Header32> h32(count);
  std::vector<uintptr_t> addrs(count);
  for (size_t i = 0; i < count; ++i) {
    void* base = reinterpret_cast<void*>((i / 500) * kPageSize);
    h48[i].page = base;
    h32[i].next = base;
    addrs[i] = reinterpret_cast<uintptr_t>(base) + (i % 500) * 128 + 64;
  }
  Registry reg; reg.init(64);
  Registry reg8; reg8.init(256);
  for (int i = 0; i < 8; ++i) reg8.insert(0x700000000000ull + i * 0x100000ull);

  // A large-object page is bigger than the mask quantum, so the masked base is not
  // its start. The fallback is a range search over the few large pages that exist.
  struct Range { uintptr_t begin, end; };
  std::vector<Range> large;
  for (size_t i = 0; i < 8; ++i) {
    const uintptr_t begin = 0x700000000000ull + i * 0x400000ull;
    large.push_back({begin, begin + 0x400000ull});
  }
  auto in_large = [&](uintptr_t addr) {
    size_t lo = 0, hi = large.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (addr < large[mid].begin) hi = mid;
      else if (addr >= large[mid].end) lo = mid + 1;
      else return true;
    }
    return false;
  };

  auto bench = [&](const char* name, auto&& fn) {
    double best = 1e9;
    uintptr_t sink = 0;
    for (int round = 0; round < 3; ++round) {
      const auto t0 = std::chrono::steady_clock::now();
      for (size_t i = 0; i < count; ++i) sink += fn(i);
      const auto t1 = std::chrono::steady_clock::now();
      const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / count;
      if (ns < best) best = ns;
    }
    std::printf("%-34s %6.3f ns/op (sink %llu)\n", name, best,
                static_cast<unsigned long long>(sink & 0xff));
  };

  bench("field read (today)", [&](size_t i) { return reinterpret_cast<uintptr_t>(h48[i].page); });
  bench("mask only", [&](size_t i) { return addrs[i] & ~(kPageSize - 1); });
  bench("mask + registry (empty)", [&](size_t i) {
    const uintptr_t base = addrs[i] & ~(kPageSize - 1);
    return reg.contains(base) ? 0 : base; });
  bench("mask + registry (8 large)", [&](size_t i) {
    const uintptr_t base = addrs[i] & ~(kPageSize - 1);
    return reg8.contains(base) ? 0 : base; });
  bench("mask + range search (8 large)", [&](size_t i) {
    const uintptr_t base = addrs[i] & ~(kPageSize - 1);
    return in_large(addrs[i]) ? 0 : base; });
  bench("range search, large-page address", [&](size_t i) {
    const uintptr_t addr = large[i % 8].begin + (i % 64) * 128;
    return in_large(addr) ? 0 : addr; });
  // The alternative that serves large pages without a range search: register one
  // entry per 64 KiB quantum a large page covers, so the same masked base is the
  // key and one probe answers "ordinary" or "belongs to large page N".
  Registry quantum; quantum.init(1024);
  for (size_t i = 0; i < 8; ++i) {
    const uintptr_t begin = 0x700000000000ull + i * 0x400000ull;
    for (uintptr_t q = begin; q < begin + 0x400000ull; q += kPageSize)
      quantum.insert(q);
  }
  bench("mask + quantum map (8 large)", [&](size_t i) {
    const uintptr_t base = addrs[i] & ~(kPageSize - 1);
    return quantum.contains(base) ? 0 : base; });
  bench("field read, 32-byte header", [&](size_t i) { return reinterpret_cast<uintptr_t>(h32[i].next); });
  return 0;
}
