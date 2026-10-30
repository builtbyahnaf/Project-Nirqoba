# ADR 0001: Pinned LLVM/MLIR Toolchain

## Status
Accepted

## Context
Nirqoba uses MLIR and LLVM as its foundational compiler infrastructure. MLIR C++ APIs are internal and change rapidly between LLVM releases. Furthermore, on Windows, prebuilt LLVM binaries must be compiled with matching MSVC CRT and include development headers and CMake configuration files (`MLIRConfig.cmake`).

## Decision
Pin LLVM/MLIR to version **23.1.4**, specifically the `clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.xz` release asset.
This archive is built with `LLVM_INSTALL_TOOLCHAIN_ONLY=OFF`, providing MLIR headers, static libraries, `mlir-tblgen`, and `lld-link`.
All toolchain hashes and versions are locked in `toolchain/versions.lock.json` and verified automatically by `scripts/bootstrap.ps1`.
