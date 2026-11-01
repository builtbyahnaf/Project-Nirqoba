#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
struct Node { std::uint64_t value; Node* next; };
int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
  const std::string mode = argc > 2 ? argv[2] : "alloc";
  std::uint64_t checksum = 0;
  const auto begin = Clock::now();
  if (mode == "alloc") {
    for (std::size_t i=0;i<n;++i) { auto* p=new std::uint64_t[8]; p[0]=i; p[7]=i^0x5a5a; checksum+=p[0]+p[7]; delete[] p; }
  } else if (mode == "alloc_small" || mode == "alloc_large" || mode == "alloc_mixed") {
    for (std::size_t i=0;i<n;++i) {
      const bool large = mode == "alloc_large" || (mode == "alloc_mixed" && (i & 1));
      const std::size_t words = large ? 512 : 2;
      auto* p = new std::uint64_t[words]{};
      p[0] = i;
      p[words - 1] = i ^ 0x5a5a;
      checksum += p[0] + p[words - 1];
      delete[] p;
    }
  } else if (mode == "scalar") {
    std::uint64_t x=1; for(std::size_t i=0;i<n;++i) x=(x*1664525u+1013904223u)^(i+0x9e37u); checksum=x;
  } else if (mode == "array") {
    std::vector<std::uint64_t> a(n); for(std::size_t i=0;i<n;++i) a[i]=i*3+1;
    checksum=std::accumulate(a.begin(),a.end(),std::uint64_t{0});
  } else if (mode == "graph") {
    const std::size_t count=std::min<std::size_t>(n,500000);
    std::vector<std::unique_ptr<Node>> nodes; nodes.reserve(count);
    for(std::size_t i=0;i<count;++i) nodes.push_back(std::make_unique<Node>(Node{i,nullptr}));
    for(std::size_t i=0;i<count;++i) nodes[i]->next=nodes[(i+1)%count].get();
    Node* p=count?nodes[0].get():nullptr; for(std::size_t i=0;i<count*4&&p;++i,p=p->next) checksum+=p->value;
  } else if (mode == "threads") {
    std::atomic<std::uint64_t> sum{0}; std::vector<std::thread> workers;
    for(int t=0;t<4;++t) workers.emplace_back([&,t]{std::uint64_t local=0; for(std::size_t i=t;i<n;i+=4){auto* p=new std::uint64_t[8];p[0]=i;p[7]=i^0x5a5a;local+=p[0]+p[7];delete[] p;}sum.fetch_add(local,std::memory_order_relaxed);});
    for(auto& worker:workers) worker.join(); checksum=sum.load();
  } else { return 2; }
  const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count();
  std::cout<<"mode="<<mode<<" ns="<<ns<<" checksum="<<checksum<<'\n';
}

