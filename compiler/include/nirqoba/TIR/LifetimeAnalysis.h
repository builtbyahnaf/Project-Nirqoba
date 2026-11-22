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
#include <cstdint>
#include <string>
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

// How many sites of one layout make a group worth a shared pool. Four is the
// point where the bookkeeping a pool costs (one create, one destroy, and a reset
// per slot) is repaid by not asking the allocator four times.
inline constexpr std::size_t kPoolSiteThreshold = 4;

// What the analysis learned about one allocation site.
struct AllocationSite {
  AllocationLifetime lifetime = AllocationLifetime::Escaping;
  EscapeReason reason = EscapeReason::None;
  // The site sits inside a loop body, so it runs once per iteration.
  bool in_loop = false;
  // Sites in this function that allocate this exact layout (payload type and
  // byte count), including this one.
  std::size_t identical_sites = 0;
  // A pool hands out fixed-size slots, so the site needs a byte count the
  // compiler knows.
  bool constant_payload = false;
  std::uint64_t payload_bytes = 0;
  // The layout group this site belongs to; sites in one group can share a pool.
  std::string layout_key;
  // Repeated, plain, locally used storage whose lifetime ends with the
  // activation: the shape a bulk pool exists for.
  bool pool_eligible = false;
  // The narrowest loop that encloses this site and every use of its storage, if
  // one exists. A region created on that loop's entry edge and destroyed on its
  // exit edges then holds storage for one loop instead of for the whole
  // activation, which is what keeps a long-running loop's peak bounded by its own
  // work rather than by everything the frame ever allocated.
  // `scope_begin` is the loop header label's position in the operation stream
  // and `scope_end` the back edge that closes the loop. `scope_header` is that
  // header's label id, which names the same loop in any rewrite of the stream.
  bool loop_scoped = false;
  std::size_t scope_begin = 0;
  std::size_t scope_end = 0;
  int scope_header = -1;
  // The programmer released this storage by hand. Bulk release cannot replace
  // that call, so a rewrite must leave the site alone.
  bool manually_released = false;
  // The body reads or writes through the storage (an element access, a load
  // through the pointer). An allocation nobody touches is not evidence of a
  // local working set, and a rewrite has nothing to gain from claiming it.
  bool used_locally = false;
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

// A call whose returned storage the caller keeps in its own frame, with every
// value that names that storage in the caller.
struct BoundedCallResult {
  // The call's result value: the pointer the callee returned.
  int result = -1;
  // The callee that filled it.
  std::string callee;
  // Every value in the caller that names the same storage: the result, the slot
  // it is stored into, the values loaded back out of that slot and the addresses
  // derived from any of them. A Drop or an explicit release is recognised by it.
  std::unordered_set<int> aliases;
  // The narrowest loop that encloses the call and every use of the storage it
  // returns, when one exists. The arena this activation creates for the call then
  // lives for one iteration rather than for the whole frame: it is created on the
  // loop's entry edge and released on its exits, so a long-running loop's peak is
  // bounded by one iteration's work instead of by everything the frame allocated.
  // `scope_begin` is the loop header's position in the caller's operation stream
  // and `scope_end` the back edge that closes it.
  bool loop_scoped = false;
  std::size_t scope_begin = 0;
  std::size_t scope_end = 0;
  int scope_header = -1;
};

// Which callees take their caller's arena for the storage they return, and the
// call sites that supply it.
//
// One signature serves every call site, so a callee that returns fresh storage
// always gains the trailing handle as long as a single caller bounds the result.
// The handle is therefore nullable: a bounded caller passes the arena it created,
// an escaping caller passes null, and the callee picks its allocator from the
// handle at runtime. That is what lets one call site use the arena while another
// keeps the general allocator, which is the case a per-function decision cannot
// express.
struct CallSiteArenaPlan {
  // Callees that take a trailing arena handle instead of allocating from the
  // general allocator for the storage they return.
  std::unordered_set<std::string> promoted_callees;
  // Of those, the callees that also have a call site which does not bound the
  // result: their body checks the handle rather than assuming one arrived.
  std::unordered_set<std::string> partially_promoted;
  // Call sites per caller function: the ones whose handle the promoted callee
  // will read. Keyed by caller name because TIR values are per function.
  std::unordered_map<std::string, std::vector<BoundedCallResult>> call_sites;
  // Call sites of promoted callees that pass a null handle, keyed by caller name:
  // the result value of each call. These are the sites the callee falls back to
  // the general allocator for.
  std::unordered_map<std::string, std::unordered_set<int>> escaping_call_results;
  // Call sites that forward this activation's own arena handle down a direct
  // recursive call instead of passing null, keyed by caller name. The handle is
  // the one the caller was given, so the whole recursive chain fills one chunk
  // and the outermost bounded caller releases it once.
  std::unordered_map<std::string, std::unordered_set<int>> forwarded_call_results;

  bool promoted(const std::string& callee) const {
    return promoted_callees.count(callee) != 0;
  }

  // True when the call in `caller` (described by its result value) forwards the
  // handle rather than passing null.
  bool forwards(const std::string& caller, int result) const {
    const auto found = forwarded_call_results.find(caller);
    return found != forwarded_call_results.end() && found->second.count(result) != 0;
  }

  bool partial(const std::string& callee) const {
    return partially_promoted.count(callee) != 0;
  }
};

// Classifies the call sites of this module whose returned storage stays in the
// caller's frame, and decides which callees can be served from it.
CallSiteArenaPlan analyze_call_site_arenas(const Module& module);

// True when an allocation of this payload is explicit storage the front end
// hands to the programmer: `memory.allocate<T>`.
bool is_allocation_site(const Op& op);

// True when an arena may hold the payload: plain data with no managed reference
// (the collector cannot trace arena memory) and no component that needs a
// destructor (an arena frees in bulk and never runs field destruction).
bool is_arena_eligible_payload(const Type& type);

}  // namespace nirq::tir
