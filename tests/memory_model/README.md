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

Run the complete suite (integration plus these cases):

```text
python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe
```

`reverse_destruction_order` uses `expect_stdout_index_order`, which asserts that
each listed token appears after the previous one in the captured TIR. A drop
must name its release, so this observes destruction order directly.
