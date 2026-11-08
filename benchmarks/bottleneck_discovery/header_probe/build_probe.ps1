# Builds and runs the object-header probe against one revision's runtime sources.
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
#define NIRQOBA_VERSION_STRING "probe"
#define NIRQOBA_VERSION_LABEL "probe"
#define NIRQOBA_VERSION_MAJOR 0
#define NIRQOBA_VERSION_MINOR 0
#define NIRQOBA_VERSION_PATCH 0
#define NIRQOBA_PHASE "6"
#define NIRQOBA_LLVM_VERSION "probe"
#define NIRQOBA_BUILD_TYPE "probe"
#define NIRQOBA_DEFAULT_TARGET_TRIPLE "x86_64-pc-windows-msvc"
"@ | Set-Content -Path $versionHeader -Encoding ascii

# Compile every runtime source except gc.cpp (probe.cpp includes it) so the
# probe links without duplicate symbols.
$sources = @()
Get-ChildItem (Join-Path $dest 'runtime/src') -Filter *.cpp | ForEach-Object {
    if ($_.Name -ne 'gc.cpp' -and $_.Name -ne 'socket_windows.cpp') {
        $sources += $_.FullName
    }
}
$exe = Join-Path $dest "header_probe_$sha.exe"
$quoted = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
$bat = Join-Path $dest 'build_probe.bat'
$cl = @"
@echo off
call "$($tc.vcvars64)" > nul 2>&1 || exit /b 1
cd /d "$dest" || exit /b 1
cl /nologo /O2 /DNDEBUG /std:c++20 /EHsc /MD /W0 ^
   /DNRQ_RUNTIME_DEBUG_ENABLED=0 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /I "$dest\runtime\include" /I "$dest\runtime\src" ^
   "$PSScriptRoot\probe.cpp" $quoted ^
   /Fe:"$exe" /link dxgi.lib
"@
Set-Content -Path $bat -Value $cl -Encoding ascii
& $bat
if ($LASTEXITCODE -ne 0) { throw "compile failed for $sha" }
& $exe
