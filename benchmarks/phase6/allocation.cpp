#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <vector>
int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
  volatile std::uint64_t sum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < n; ++i) {
    auto* p = new std::uint64_t[8];
    p[0] = i; p[7] = i ^ 0x5a5a;
    sum += p[0] + p[7];
    delete[] p;
  }
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - begin).count();
  std::cout << "iterations=" << n << " ns=" << ns << " checksum=" << sum << '\n';
}
