// vector_alignment.cpp — E3: what does 16-byte alignment buy a vectorized scan?
//
// Three variants over the same buffer, same loop, same data:
//   A  aligned base, aligned 16-byte loads   (_mm_load_si128)
//   B  aligned base, unaligned loads         (_mm_loadu_si128)
//   C  base + 8, unaligned loads             (_mm_loadu_si128)
// A vs B is the instruction choice alignment permits; B vs C is true misalignment.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <emmintrin.h>
#include <algorithm>
#include <cstring>

namespace {

constexpr std::size_t kElements = 1u << 15;  // 256 KiB: cache resident
constexpr int kPasses = 2048;
constexpr int kRounds = 7;

volatile std::int64_t sink;

std::int64_t sum_aligned(const std::int64_t* p, std::size_t n) {
  __m128i acc = _mm_setzero_si128();
  for (std::size_t i = 0; i < n; i += 2) {
    const __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(p + i));
    acc = _mm_add_epi64(acc, v);
  }
  alignas(16) std::int64_t lanes[2];
  _mm_store_si128(reinterpret_cast<__m128i*>(lanes), acc);
  return lanes[0] + lanes[1];
}

std::int64_t sum_unaligned(const std::int64_t* p, std::size_t n) {
  __m128i acc = _mm_setzero_si128();
  for (std::size_t i = 0; i < n; i += 2) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
    acc = _mm_add_epi64(acc, v);
  }
  alignas(16) std::int64_t lanes[2];
  _mm_store_si128(reinterpret_cast<__m128i*>(lanes), acc);
  return lanes[0] + lanes[1];
}

template <typename Fn>
double measure(Fn&& pass) {
  double best = 1e30;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    std::int64_t sum = 0;
    for (int i = 0; i < kPasses; ++i) sum += pass();
    const auto end = std::chrono::steady_clock::now();
    sink = sum;
    best = std::min(best, std::chrono::duration<double, std::nano>(end - start).count() /
                              (static_cast<double>(kElements) * kPasses));
  }
  return best;
}

}  // namespace

int main() {
  void* raw = _aligned_malloc(kElements * sizeof(std::int64_t) + 64, 64);
  auto* base = static_cast<std::int64_t*>(raw);
  for (std::size_t i = 0; i < kElements; ++i) base[i] = static_cast<std::int64_t>(i & 15);
  const std::int64_t* misaligned = reinterpret_cast<const std::int64_t*>(
      reinterpret_cast<const unsigned char*>(base) + 8);

  const double a = measure([&] { return sum_aligned(base, kElements); });
  const double b = measure([&] { return sum_unaligned(base, kElements); });
  const double c = measure([&] { return sum_unaligned(misaligned, kElements - 1); });

  std::printf("%-38s %6.3f ns/element\n", "A aligned base, aligned loads", a);
  std::printf("%-38s %6.3f ns/element\n", "B aligned base, unaligned loads", b);
  std::printf("%-38s %6.3f ns/element\n", "C misaligned base (+8), unaligned", c);
  std::printf("\nA/B %.3f   C/B %.3f\n", a / b, c / b);
  _aligned_free(raw);
  return 0;
}
