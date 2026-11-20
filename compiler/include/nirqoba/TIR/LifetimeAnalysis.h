// TIR/LifetimeAnalysis.h — where an allocation's storage goes.
//
// Phase Psi routes an allocation to the strategy its lifetime asks for: storage
// that dies with the activation it was created in belongs in the activation's
// scratch arena, and storage that leaves the activation keeps the general
// allocator. That decision needs one fact the front end never states: does the
// pointer escape the function it was allocated in?
//
// The classification is a conservative, flow-insensitive analysis of the whole
// function body. "Bounded" therefore means "no path in this function lets the
// pointer out", which is the only answer a rewrite may act on: a single escaping
// path is enough to keep the general allocator.
#pragma once

#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "nirqoba/TIR/IR.h"

namespace nirq::tir {

// Where an allocation's storage is observable.
enum class AllocationLifetime {
  // The pointer reaches a return, a callee, or memory this activation does not
  // own. It can outlive the activation, so only a collector may reclaim it.
  Escaping,
  // Every use stays inside the activation: the storage dies with the frame that
  // allocated it and is released wholesale.
  Bounded,
};

// The first rule that made a site escape. A classifier that can only answer
// "escaping" cannot be reviewed or debugged, so the transport reason travels
// with the answer.
enum class EscapeReason {
  None,
  Returned,                 // `return` yields the pointer (directly or moved)
  PassedToCallee,           // a call receives it and may retain it
  StoredOutsideActivation,  // written through a pointer this frame does not own
  ContainedInEscapingObject,  // stored inside an object that itself escapes
  CyclicGraph,              // the object graph references itself
};

std::string_view lifetime_name(AllocationLifetime lifetime);
std::string_view escape_reason_name(EscapeReason reason);

// What the analysis learned about one allocation site.
struct AllocationSite {
  AllocationLifetime lifetime = AllocationLifetime::Escaping;
  EscapeReason reason = EscapeReason::None;
  // The programmer released this storage by hand. Bulk release cannot replace
  // that call, so a rewrite must leave the site alone.
  bool manually_released = false;
  // Every value that names the same storage: the allocation result, the binding
  // slot it is stored in, the values loaded back out, and the element pointers
  // derived from it. Ownership bookkeeping and rewrites need the whole set.
  std::unordered_set<int> aliases;
};

// The classification of every explicit allocation in one function, keyed by the
// allocation's result value.
class LifetimeAnalysis {
 public:
  static LifetimeAnalysis analyze(const Function& function);

  // The site for an allocation result, or null when the value is not one.
  const AllocationSite* site(int allocation_result) const;
  bool empty() const { return sites_.empty(); }
  std::size_t size() const { return sites_.size(); }
  const std::unordered_map<int, AllocationSite>& sites() const { return sites_; }

 private:
  std::unordered_map<int, AllocationSite> sites_;
};

// True when an allocation of this payload is explicit storage the front end
// hands to the programmer: `memory.allocate<T>`.
bool is_allocation_site(const Op& op);

// True when an arena may hold the payload: plain data with no managed reference
// (the collector cannot trace arena memory) and no component that needs a
// destructor (an arena frees in bulk and never runs field destruction).
bool is_arena_eligible_payload(const Type& type);

}  // namespace nirq::tir
