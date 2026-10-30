# Nirqoba Test Suite

This directory contains the automated test suite for Project Nirqoba.

## Directory Layout

- `support/`: Lightweight C++ unit test harness (`test_harness.h`) and `unit_test_main.cpp`.
- `unit/`: C++ unit tests covering internal compiler components (Lexer, Parser, Sema, TIR, Lowering, CLI).
- `integration/`: End-to-end integration test cases driven by `run_tests.py`.
  - `positive/`: Programs that should compile and run successfully.
  - `negative/`: Programs that must fail with specific diagnostic error codes.
  - `cli/`: Command-line interface behavior tests (missing flags, wrong options, exit codes).
  - `mlir/`: Tests checking MLIR generation and lowering invariants.
- `run_tests.py`: Standalone Python integration test runner using only standard library modules.

## How to Run Tests

### Run all tests (unit + integration):
```powershell
.\scripts\test.ps1 -Configuration RelWithDebInfo
```

### Run C++ unit tests only:
```powershell
.\scripts\test.ps1 -UnitOnly
```

### Run integration tests directly:
```powershell
python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe
```

## Adding an Integration Test Case

Create a directory under `tests/integration/<category>/<case_name>/` containing:
1. `main.nrqba`: The source file to test.
2. `expect.json`: Test expectation specification:
```json
{
  "mode": "build_and_run",
  "expect_exit": 0,
  "expect_stdout": "Hello, Nirqoba!\n"
}
```
Supported `mode` values:
- `build_and_run`: Compiles and runs `main.nrqba`.
- `build`: Compiles `main.nrqba` with given `args` (e.g. `--emit=mlir` or `--emit=obj`).
- `check`: Runs `nirqoba check main.nrqba`.
- `raw`: Invokes `nirqoba` directly with arbitrary `args`.
