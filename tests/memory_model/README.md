# Memory-model tests

These cases exercise the Phase 2 move and destruction rules at runtime and in
generated TIR. They use the same `expect.json` schema as `tests/integration` and
are included automatically by `tests/run_tests.py`.

| Case | Checks |
|---|---|
| `move_local` | A moved local is released once by its new owner |
| `move_argument` | An owning argument transfers to the callee |
| `move_return` | A returned owner stays valid in the caller |
| `move_chain` | A three-link move chain has one destruction |
| `field_move` | A moved aggregate field is skipped by aggregate cleanup |
| `conditional_move` | Both arms of a conditional move destroy exactly once |
| `early_return` | Nested scopes are closed before an early return |
| `multiple_returns` | Every return branch releases its owner |
| `loop_exit` | `continue` and `break` close the loop-body scope |
| `recursive_calls` | Each recursive frame destroys its own owner |
| `partial_initialization` | Uninitialized aggregate fields are not destroyed |
| `nested_aggregate` | Nested fields are destroyed on normal and early exits |
| `reowned_slot` | A moved slot that receives a new owner gets a fresh obligation |
| `overwrite_owner` | A still-live owner is destroyed before replacement |
| `shared_move` | A shared token transfers without changing its count |
| `defer_rebinding` | A deferred call keeps its captured value while the binding is rebound |
| `reverse_destruction_order` | Drops follow reverse declaration order in TIR |
| `borrow_immutable_pair` | Two immutable borrow arguments coexist |
| `borrow_mutable_write` | Mutation through a mutable borrow is visible in the owner |
| `borrow_branch_merge` | A borrow stays valid across a branch merge |
| `borrow_loop_scope` | A borrow is read and written across loop iterations |
| `borrow_disjoint_fields` | Writing a disjoint field does not conflict with `&p.x` |
| `borrow_forwarded_call` | A borrowed parameter is forwarded through a call |
| `borrow_parameter_return` | A returned borrow names and preserves its source parameter |
| `borrow_nested_scope` | A nested lexical borrow ends with its scope |
| `borrow_mutable_expiration` | A mutable borrow expires at scope exit |
| `borrow_loop_expiration` | A loop-local borrow does not mark its scalar owner moved |
| `borrow_recursive` | A borrowed parameter survives recursive calls |
| `borrow_conflict` | Two incompatible borrow arguments in one call are rejected |
| `borrow_owner_move` | Moving an owner with a live borrow is rejected |
| `borrow_after_release` | Releasing an owner with a live borrow is rejected |
| `borrow_local_return` | Returning an address of a local is rejected |
| `borrow_aggregate_escape` | Returning an aggregate holding a borrow is rejected |
| `borrow_mutable_forward` | A mutable borrow is forwarded through two calls |
| `borrow_disjoint_branch` | A disjoint field write stays legal across a branch |
| `borrow_immutable_write` | Writing through an immutable borrow is rejected |
| `borrow_mutable_conflict` | Two mutable arguments for one owner are rejected |
| `borrow_mutable_return` | A mutable borrow returned from its declared source works |
| `borrow_field_scope` | A mutable borrowed field is usable inside its owner scope |
| `borrow_owner_access` | A direct owner write while its borrow is live is rejected |
| `manual_lifecycle` | Manual allocation writes, reads and releases explicitly |
| `manual_automatic_cleanup` | A manual owner with no explicit release gets the lexical fallback |
| `manual_move` | Moving a manual owner invalidates the source and releases once |
| `manual_borrow` | A scoped borrow writes and reads manual storage before release |
| `manual_allocator_handle` | Manual allocation preserves an explicit general allocator handle |
| `manual_managed_payload` | A managed field in manual storage stays rooted across collection |
| `manual_conditional_release` | Every branch discharges the manual owner exactly once |
| `manual_deallocate_rejected` | Ordinary deallocation of manual storage is rejected |
| `manual_secure_release_rejected` | Secure release of manual storage is rejected |
| `manual_double_release` | A second manual release is rejected |
| `manual_use_after_release` | Reading after manual release is rejected |
| `manual_borrow_release` | Releasing while a manual borrow is live is rejected |
| `manual_reallocate_rejected` | Manual storage cannot be resized |
| `manual_arena_rejected` | Manual allocation cannot use a tracked arena/pool |
| `manual_function_argument_rejected` | Manual storage cannot cross an owned parameter |
| `manual_return_rejected` | Manual storage cannot cross an owned return |
| `manual_owned_payload_rejected` | Deterministic-cleanup payloads are rejected |
| `manual_ffi_unsupported` | The parser rejects the absent `extern` surface |

Run the complete suite (integration plus these cases):

```text
python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe
```

`reverse_destruction_order` uses `expect_stdout_index_order`, which asserts that
each listed token appears after the previous one in the captured TIR. A drop
must name its release, so this observes destruction order directly.
