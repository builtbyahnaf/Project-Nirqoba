# Builds the GC harness against the runtime sources of one git revision.
#
#   powershell -File build_and_run.ps1 -Rev HEAD -Out build/bottleneck_discovery
#
# Each revision is extracted with `git archive` into its own directory, so the
# harness never sees a mixed tree. The collector and the harness are compiled by
# the pinned MSVC toolset with the same optimisation level as a release build
# (/O2 /DNDEBUG) and the production runtime configuration
# (NRQ_RUNTIME_DEBUG_ENABLED=0).
param(
    [string]$Rev = 'HEAD',
    [string]$Out = 'build/bottleneck_discovery'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
if (-not (Test-Path (Join-Path $repo 'toolchain/toolchain.json'))) {
    throw "toolchain/toolchain.json missing"
}
$tc = Get-Content (Join-Path $repo 'toolchain/toolchain.json') -Raw | ConvertFrom-Json

$sha = (& git -C $repo rev-parse --short $Rev).Trim()
$dest = Join-Path $repo (Join-Path $Out $sha)
New-Item -ItemType Directory -Force -Path $dest | Out-Null

# Extract exactly the runtime tree of this revision.
$tar = Join-Path $dest 'runtime.tar'
& git -C $repo archive --format=tar -o $tar $Rev runtime
if ($LASTEXITCODE -ne 0) { throw "git archive failed" }
& tar -xf $tar -C $dest
Remove-Item $tar

$harness = Join-Path $PSScriptRoot 'main.cpp'
# runtime_version.cpp needs the CMake-generated nirqoba/Version.h. The harness
# never calls nrq_runtime_version(), so a stub header keeps the measurement
# independent of the build system.
$stubInclude = Join-Path $dest 'runtime/include'
$versionHeader = Join-Path $stubInclude 'nirqoba/Version.h'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $versionHeader) | Out-Null
@"
#pragma once
#define NIRQOBA_VERSION_STRING "harness"
#define NIRQOBA_VERSION_LABEL "harness"
#define NIRQOBA_VERSION_MAJOR 0
#define NIRQOBA_VERSION_MINOR 0
#define NIRQOBA_VERSION_PATCH 0
#define NIRQOBA_PHASE "6"
#define NIRQOBA_LLVM_VERSION "harness"
#define NIRQOBA_BUILD_TYPE "harness"
#define NIRQOBA_DEFAULT_TARGET_TRIPLE "x86_64-pc-windows-msvc"
"@ | Set-Content -Path $versionHeader -Encoding ascii
$sources = @()
Get-ChildItem (Join-Path $dest 'runtime/src') -Filter *.cpp | ForEach-Object {
    if ($_.Name -ne 'socket_windows.cpp') { $sources += $_.FullName }
}
$exe = Join-Path $dest "gc_harness_$sha.exe"

$quoted = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
$bat = Join-Path $dest 'build.bat'
$cl = @"
@echo off
call "$($tc.vcvars64)" > nul 2>&1 || exit /b 1
cd /d "$dest" || exit /b 1
cl /nologo /O2 /DNDEBUG /std:c++20 /EHsc /MD /W0 ^
   /DNRQ_RUNTIME_DEBUG_ENABLED=0 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /I "$dest\runtime\include" /I "$dest\runtime\src" ^
   "$harness" $quoted ^
   /Fe:"$exe" /link dxgi.lib
"@
Set-Content -Path $bat -Value $cl -Encoding ascii
& $bat | Out-Host
if ($LASTEXITCODE -ne 0) { throw "compile failed for $sha" }
Write-Host "built $exe"
