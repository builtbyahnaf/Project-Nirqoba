# Nirqoba Version Log

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

I, Muhammad Tahmid Ahnaf (@builtbyahnaf), am the sole author and owner of Nirqoba and
hold sole authority over its direction, architecture, language design, and every
technical or project decision. No change to the language, compiler, runtime, toolchain,
documentation, or process is authorized without my explicit approval, and every release
recorded below is made under that authority.

> Note: The changes before `0.102-beta` were massive, so they were intentionally left out here.

---

## Version 0.637-beta

### Summary of Changes (since `0.633-beta`)

Phase 9 opens with Phase 10.10: the compiler now reports the memory decisions it
makes on its own, states the facts those decisions rest on to LLVM, and skips the
allocations that cannot be observed. Release label `+0.004` (`0.633-beta` ->
`0.637-beta`) per the three-upgrade rule.

#### CDMM diagnostics

- `--inspect-cdmm` reports every allocation site to stderr: its source location,
the binding that holds it, the layout it allocates, the selected strategy
(`stack`, `pool`, `arena`, `gc`) and the fact that selected it.
- The decision is recorded during TIR translation, where the lifetime analysis and
the choice are both in hand, and reported by the strategy audit pass, which is the
single place an automated decision becomes visible. The carrier attribute is
removed as it is printed, so the diagnostic never reaches the emitted program.
- Rationales use the analysis vocabulary: `bounded to function`, `loop-scoped`,
`promoted to the stack`, and the five escape reasons.

#### LLVM optimization compatibility

- Runtime allocator returns now carry `noalias` and an explicit alignment
(`nrq_memory_allocate`, `nrq_memory_allocate_from`, `nrq_cdmm_arena_allocate`,
`nrq_cdmm_pool_allocate`). A fresh allocation cannot overlap anything the caller
can already name, and its base is a multiple of the payload's alignment.
- The attributes are stated on the allocator declarations, which is where LLVM
keeps a callee's return attributes: every call site inherits them.

#### Dead-allocation elision

- An allocation nothing reads, whose pointer stays in the activation and whose
payload needs no destructor, is removed from the TIR before any strategy claims
it: no allocator call, no arena, no release.
- Elision reuses the evidence selection already computes (bounded lifetime, no
local use, plain payload), and it removes the obligation along with the storage,
so the function no longer claims a destruction it does not perform.

#### Call-site arena promotion

- The lifetime classifier is now interprocedural in one direction: a call whose
returned storage the caller keeps in its own frame is marked *caller-bounded*.
- A callee that returns fresh storage gains a trailing arena handle as soon as one
call in the module bounds the result; the caller creates the arena in its entry
block, passes it, releases the chunk on the way out, and no longer releases the
returned binding.
- The handle is nullable, so the other callers do not have to agree: an escaping
site passes null and the callee allocates that storage from the general allocator
at runtime. One signature serves every call site, and the decision stays local to
the caller.
- The CDMM report names every half: the callee's site reports `arena (returned to
bounded and escaping callers, arena handle checked at runtime)`, a bounded call
site reports `arena (bounded to the caller's frame, filled by <callee>)`, and an
escaping one reports `gc (escaping call site, null arena handle passed to
<callee>)`.

#### Dynamic strategy dispatch

- Promotion is per call site now. A callee that returns fresh storage always gains
  the trailing arena handle when at least one caller bounds the result; every call
  supplies that argument, with null standing for "the storage leaves this frame".
- The callee branches on the handle at the allocation it returns: a real handle
  fills the caller's arena, null falls back to `nrq_memory_allocate`. One
  signature therefore serves both kinds of caller, and an escaping caller no
  longer denies the arena to the bounded ones.
- The classifier reports a callee with both kinds of call site as partially
  promoted, and the CDMM report names the escaping call site and the null handle
  it passes, so both halves of the dispatch are reviewable.

#### Loop-scoped interprocedural arenas

- A promoted call site whose storage is created and consumed inside one loop now
  gets a loop-scoped arena: created on the loop's entry edge, released on its exit
  edges, so a long-running loop's peak is one iteration's work instead of
  everything the frame ever allocated.
- The call-site classifier reuses the allocation classifier's loop nesting
  (`LoopNesting`), so a call site and a local allocation in the same loop describe
  the same span; the narrowest loop holding the call and every use of its result
  wins, and everything else keeps the frame's arena.

