# Builds the per-operation runtime-call probe against one revision's runtime.
# Diagnostic only; not part of the build.
param(
    [string]$Rev = 'HEAD',
    [string]$Out = 'build/bottleneck_discovery'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$tc = Get-Content (Join-Path $repo 'toolchain/toolchain.json') -Raw | ConvertFrom-Json
$sha = (& git -C $repo rev-parse --short $Rev).Trim()
$dest = Join-Path $repo (Join-Path $Out $sha)

if (-not (Test-Path (Join-Path $dest 'runtime/src/gc.cpp'))) {
    $tar = Join-Path $dest 'runtime.tar'
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    & git -C $repo archive --format=tar -o $tar $Rev runtime
    if ($LASTEXITCODE -ne 0) { throw "git archive failed" }
    & tar -xf $tar -C $dest
    Remove-Item $tar
}

$versionHeader = Join-Path $dest 'runtime/include/nirqoba/Version.h'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $versionHeader) | Out-Null
@"
#pragma once
#define NIRQOBA_VERSION_STRING "call-probe"
#define NIRQOBA_VERSION_LABEL "call-probe"
#define NIRQOBA_VERSION_MAJOR 0
#define NIRQOBA_VERSION_MINOR 0
#define NIRQOBA_VERSION_PATCH 0
#define NIRQOBA_PHASE "6"
#define NIRQOBA_LLVM_VERSION "call-probe"
#define NIRQOBA_BUILD_TYPE "call-probe"
#define NIRQOBA_DEFAULT_TARGET_TRIPLE "x86_64-pc-windows-msvc"
"@ | Set-Content -Path $versionHeader -Encoding ascii

$sources = @()
Get-ChildItem (Join-Path $dest 'runtime/src') -Filter *.cpp | ForEach-Object {
    if ($_.Name -ne 'socket_windows.cpp') { $sources += $_.FullName }
}
$probe = Join-Path $PSScriptRoot 'main.cpp'
$exe = Join-Path $dest "call_probe_$sha.exe"
$quoted = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
$bat = Join-Path $dest 'build_call_probe.bat'
$cl = @"
@echo off
call "$($tc.vcvars64)" > nul 2>&1 || exit /b 1
cd /d "$dest" || exit /b 1
cl /nologo /O2 /DNDEBUG /std:c++20 /EHsc /MD /W0 ^
   /DNRQ_RUNTIME_DEBUG_ENABLED=0 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /I "$dest\runtime\include" /I "$dest\runtime\src" ^
   "$probe" $quoted ^
   /Fe:"$exe" /link dxgi.lib
"@
Set-Content -Path $bat -Value $cl -Encoding ascii
& $bat
if ($LASTEXITCODE -ne 0) { throw "compile failed for $sha" }
& $exe frame 100000
& $exe alloc-unrooted 1000000
& $exe alloc-rooted0 1000000
& $exe alloc-rooted 1000000
& $exe store 1000000
& $exe pin 1000000
& $exe pair 500000
