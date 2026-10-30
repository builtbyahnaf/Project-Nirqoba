# Building Nirqoba From Source

## Prerequisites

- **Windows 11 or 10 (x64)**
- **Visual Studio 2022 Build Tools** with *Desktop development with C++* (MSVC v143+ toolset and Windows 10/11 SDK)
- **CMake >= 3.28**
- **Python >= 3.10**
- **Ninja** (`py -m pip install ninja`)

## 1. Toolchain Bootstrap

Run the bootstrap script to verify prerequisites and extract the pinned LLVM/MLIR 23.1.4 SDK:

```powershell
.\scripts\bootstrap.ps1
```

This verifies the environment, extracts the SDK into `toolchain/llvm-23.1.4/`, and generates `toolchain/toolchain.json`.

## 2. Compiling the Compiler

Build the compiler and runtime library using MSVC and Ninja:

```powershell
# Development build with debug info and assertions
.\scripts\build.ps1 -Configuration RelWithDebInfo

# Or optimized release build
.\scripts\build.ps1 -Configuration Release
```

The resulting compiler binary is located at `build/msvc-dev/bin/nirqoba.exe`.

## 3. Running the Test Suite

```powershell
.\scripts\test.ps1 -Configuration RelWithDebInfo
```

This executes both the C++ unit tests (`nirqoba_unit_tests`) and the Python integration test runner (`tests/run_tests.py`).

## 4. Compiling Your First Nirqoba Program

Create a file named `hello.nrqba`:
```nirqoba
int function main() {
    print("Hello, Nirqoba!");
    return 0;
}
```

Run it directly with the compiler:
```powershell
build\msvc-dev\bin\nirqoba.exe run hello.nrqba
```
Output:
```text
Hello, Nirqoba!
```
