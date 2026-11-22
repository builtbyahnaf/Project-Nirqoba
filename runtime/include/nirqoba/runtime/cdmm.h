// Explicit CDMM strategies: arena and fixed-slot pool.
//
// These are the deterministic-lifetime strategies that sit beside the collector
// and beside ordinary owned allocation. Neither carries an object header: an
// arena bumps through chunks it owns, and a pool derives the layout, slot size
// and liveness of every object from the chunk that contains it. That is the same
// page-dedicated reasoning Phase 4 uses for the headerless GC representation
// (H0), applied to explicitly managed storage.
//
// Lifetime contract, in one place:
//   * Every arena has a generation. Reset and destroy advance it, so a caller
//     that stores the generation beside a pointer can reject a reference that
//     outlived its arena with `nrq_cdmm_arena_generation`.
//   * Reset and destroy are refused while a scope is open on the arena or while
//     an allocation is in flight; the failure return leaves the arena untouched.
//   * Destructors registered with `nrq_cdmm_arena_allocate_destructible` run in
//     reverse allocation order on reset and destroy, so a nested owned resource
//     is destroyed before the object that owns it.
//   * Pool slots keep a 32-bit allocation generation when the pool is created
//     with `NRQ_CDMM_POOL_GENERATIONS`. `nrq_cdmm_pool_release_checked` then
//     rejects a release of a slot that has since been handed to another
//     allocation, which a plain address and a free bitmap cannot distinguish.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runs on reset/destroy (arena) or release/reset/destroy (pool). The object is
// still readable; its storage is released after the call returns. A destructor
// must not allocate from the arena or pool it is being destroyed with.
typedef void (*NrqCdmmDestructor)(void* object, void* context);

typedef enum NrqCdmmStrategy {
  NRQ_CDMM_STRATEGY_NONE = 0,
  NRQ_CDMM_STRATEGY_ARENA = 1,
  NRQ_CDMM_STRATEGY_POOL = 2
} NrqCdmmStrategy;

// Pool creation flag: keep a 32-bit generation per slot. Costs four bytes per
// slot; without it a reused address and a stale pointer are indistinguishable.
#define NRQ_CDMM_POOL_GENERATIONS 1u

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

// `initial_bytes` is the first chunk and the size used for growth chunks;
// `max_bytes` caps the total committed bytes, and zero means no cap. A request
// that cannot be served without exceeding the cap returns null.
void* nrq_cdmm_arena_create(size_t initial_bytes, size_t max_bytes);
// Runs the registered destructors, releases every chunk, and invalidates the
// handle. Returns zero when the handle is unknown or a scope is still open.
int32_t nrq_cdmm_arena_destroy(void* arena);
// Runs the registered destructors in reverse allocation order and rewinds the
// arena so all but the first chunk is released. Returns zero when the handle is
// unknown or a scope is still open.
int32_t nrq_cdmm_arena_reset(void* arena);
void* nrq_cdmm_arena_allocate(void* arena, size_t bytes, size_t alignment);
// Allocates one object described by a compiler layout descriptor. A deterministic
// region does not trace, so a layout that owns managed references, describes an
// array, or needs a custom trace callback is refused with a null result rather
// than accepted and traced by nobody.
struct nrq_object_layout;
void* nrq_cdmm_arena_allocate_layout(void* arena,
                                     const struct nrq_object_layout* layout);
// The same geometry, taken from a compiler allocation plan: the slot size and
// alignment are the plan's, and the plan must describe deterministic storage (no
// header, no references, not an array). A plan that survives those checks is
// still clamped to the region's own alignment rules.
struct NrqAllocPlan;
void* nrq_cdmm_arena_allocate_planned(void* arena,
                                      const struct NrqAllocPlan* plan);
void* nrq_cdmm_arena_allocate_destructible(void* arena, size_t bytes,
                                           size_t alignment,
                                           NrqCdmmDestructor destructor,
                                           void* context);

// Registers a teardown for an object the arena already handed out, so a bulk
// destroy destroys the object's managed fields before releasing its chunks.
//
// The compiler knows where a field's pointer word sits, and the object exists
// only after the allocation, so registration follows allocation instead of
// riding on it. `context` is the field's pointer-word address with the release
// kind in the three low bits - the address is at least eight-byte aligned, so the
// kind fits without displacing a bit of it:
//   1 = nrq_ffi_free, 2 = nrq_file_text_release, 3 = nrq_array_free,
//   4 = an owned array's buffer (nrq_array_free on the field's first word),
//   5 = a file resource (nrq_file_close_handle).
// `nrq_cdmm_release_managed_field` is the destructor that reads that encoding,
// which is why no function pointer crosses the boundary.
//
// Returns one when the record was made and zero when the handle is unknown, the
// object is null, or the arena has no room for the record.
int32_t nrq_cdmm_arena_register_teardown(void* arena, void* object,
                                         void* context);

// The destructor registered above: loads the pointer word at the decoded
// address, does nothing when it is null, and hands it to the release the kind
// names. Exposed so a caller can register it directly with
// `nrq_cdmm_arena_allocate_destructible` when the field address is already
// known.
void nrq_cdmm_release_managed_field(void* object, void* context);
// Allocates `count` blocks and stores the successful prefix in `output`. Stops
// at the first failure, so the return value is the number of blocks written.
size_t nrq_cdmm_arena_allocate_bulk(void* arena, size_t bytes, size_t alignment,
                                    size_t count, void** output);
// Bytes handed out in the current generation, and bytes committed from the heap.
uint64_t nrq_cdmm_arena_used(const void* arena);
uint64_t nrq_cdmm_arena_committed(const void* arena);
// Advances on every successful reset. A pointer plus the generation it was
// allocated under can be validated after a reset.
uint64_t nrq_cdmm_arena_generation(const void* arena);
int32_t nrq_cdmm_arena_contains(const void* arena, const void* pointer);

// A scope pins the arena and hands out its allocation path without touching the
// registry on each call. A scope belongs to one thread and must be the only user
// of its arena while it is open; reset and destroy are refused while it is open.
typedef struct NrqCdmmArenaScope {
  void* arena;
  void* state;
  uint64_t generation;
} NrqCdmmArenaScope;

// Returns zero when the handle is unknown or already pinned by another scope.
int32_t nrq_cdmm_arena_begin(void* arena, NrqCdmmArenaScope* scope);
void* nrq_cdmm_arena_scope_allocate(NrqCdmmArenaScope* scope, size_t bytes,
                                    size_t alignment);
void* nrq_cdmm_arena_scope_allocate_destructible(NrqCdmmArenaScope* scope,
                                                 size_t bytes, size_t alignment,
                                                 NrqCdmmDestructor destructor,
                                                 void* context);
size_t nrq_cdmm_arena_scope_allocate_bulk(NrqCdmmArenaScope* scope,
                                          size_t bytes, size_t alignment,
                                          size_t count, void** output);
void nrq_cdmm_arena_end(NrqCdmmArenaScope* scope);

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------

// `slot_bytes` is the payload size of every slot; `slot_alignment` must be a
// power of two and divides the stride. `slots_per_chunk` of zero selects a
// chunk of at most 64 KiB. The optional destructor runs on every release, and
// on every live slot during reset and destroy, which keeps per-slot metadata to
// one liveness bit plus the optional generation word.
void* nrq_cdmm_pool_create(size_t slot_bytes, size_t slot_alignment,
                           size_t slots_per_chunk, uint32_t flags,
                           NrqCdmmDestructor destructor, void* context);
// Creates a pool whose slot size comes from a compiler layout descriptor, so the
// size the compiler computed and the size the pool reserves cannot drift apart.
// The same restriction as `nrq_cdmm_arena_allocate_layout` applies: only a plain
// record layout is accepted, because a pool cannot trace managed references.
void* nrq_cdmm_pool_create_layout(const struct nrq_object_layout* layout,
                                  size_t slots_per_chunk, uint32_t flags,
                                  NrqCdmmDestructor destructor, void* context);
// Creates a pool from a compiler plan, which additionally carries the alignment
// the compiler chose. Refused for the same plans the layout form refuses.
void* nrq_cdmm_pool_create_planned(const struct NrqAllocPlan* plan,
                                   size_t slots_per_chunk, uint32_t flags,
                                   NrqCdmmDestructor destructor, void* context);
int32_t nrq_cdmm_pool_reset(void* pool);
int32_t nrq_cdmm_pool_destroy(void* pool);
void* nrq_cdmm_pool_allocate(void* pool);
size_t nrq_cdmm_pool_allocate_bulk(void* pool, size_t count, void** output);
// Returns zero for an unknown pool, a foreign address, or a double release. The
// slot is left in its previous state. A scope must be the only user of its pool
// while it is open, so a release does not also test for one.
int32_t nrq_cdmm_pool_release(void* pool, void* slot);
// Same, and also rejects `generation` when the slot has been reallocated since
// that generation was handed out.
int32_t nrq_cdmm_pool_release_checked(void* pool, void* slot, uint64_t generation);
// Registers a field teardown for one live slot, with the same `context` encoding
// `nrq_cdmm_arena_register_teardown` uses. The record is consumed when the object
// is released or when the pool is reset or destroyed, so a reused slot never runs
// the previous occupant's teardown. Returns one when the record was made.
int32_t nrq_cdmm_pool_register_teardown(void* pool, void* object, void* context);
size_t nrq_cdmm_pool_release_bulk(void* pool, void* const* slots, size_t count);
// The generation handed out with the slot's current allocation, or zero when the
// pool was created without `NRQ_CDMM_POOL_GENERATIONS` or the slot is foreign.
uint64_t nrq_cdmm_pool_generation(const void* pool, const void* slot);
uint64_t nrq_cdmm_pool_live(const void* pool);
uint64_t nrq_cdmm_pool_committed(const void* pool);
uint64_t nrq_cdmm_pool_slot_bytes(const void* pool);
int32_t nrq_cdmm_pool_contains(const void* pool, const void* pointer);

#if defined(NRQ_RUNTIME_DEBUG_ENABLED) && NRQ_RUNTIME_DEBUG_ENABLED
// Testing seam: seeds a slot's generation so the wraparound rule can be checked
// without 2^32 allocations. Debug runtime only; the production runtime has no
// such symbol and no path that writes a generation from outside an allocation.
int32_t nrq_cdmm_pool_seed_generation(void* pool, void* slot, uint32_t generation);
#endif

// A pool scope is the pool's bulk path: one registry lookup and one pin for a
// whole batch of allocations and releases. It carries the same exclusive-use
// rule as an arena scope.
typedef struct NrqCdmmPoolScope {
  void* pool;
  void* state;
} NrqCdmmPoolScope;

int32_t nrq_cdmm_pool_begin(void* pool, NrqCdmmPoolScope* scope);
void* nrq_cdmm_pool_scope_allocate(NrqCdmmPoolScope* scope);
size_t nrq_cdmm_pool_scope_allocate_bulk(NrqCdmmPoolScope* scope, size_t count,
                                         void** output);
int32_t nrq_cdmm_pool_scope_release(NrqCdmmPoolScope* scope, void* slot);
// Generation-checked release through a scope, so validating a stale reference
// does not also pay for the registry lookup a single-object call needs.
int32_t nrq_cdmm_pool_scope_release_checked(NrqCdmmPoolScope* scope, void* slot,
                                            uint64_t generation);
size_t nrq_cdmm_pool_scope_release_bulk(NrqCdmmPoolScope* scope,
                                        void* const* slots, size_t count);
void nrq_cdmm_pool_end(NrqCdmmPoolScope* scope);

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

// Which explicit strategy owns `pointer`. Returns NONE for unallocated memory
// and for collector-managed objects: the two metadata systems never claim each
// other's storage.
NrqCdmmStrategy nrq_cdmm_strategy_of(const void* pointer);

// Page-level facts about the object that owns `pointer`. `header_bytes` is zero
// for both explicit strategies, because slot size and layout come from the
// chunk; `object_bytes` is the pool slot size and zero for a variable-size
// arena. `generation` is the arena generation or the pool slot generation.
typedef struct NrqCdmmFacts {
  NrqCdmmStrategy strategy;
  uint64_t header_bytes;
  uint64_t object_bytes;
  uint64_t generation;
  uint64_t metadata_bytes;
} NrqCdmmFacts;

int32_t nrq_cdmm_object_facts(const void* pointer, NrqCdmmFacts* output);

#ifdef __cplusplus
}
#endif
