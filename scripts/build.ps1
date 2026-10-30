<#
.SYNOPSIS
    Configures and builds Nirqoba with the pinned toolchain.

.DESCRIPTION
    Loads toolchain/toolchain.json written by bootstrap.ps1, enters the MSVC x64 environment
    through vcvars64.bat, then runs the CMake preset for the requested configuration.

    Run bootstrap.ps1 first; this script fails with an actionable message if the toolchain is absent.

.PARAMETER Configuration
    Release or RelWithDebInfo. Debug is not supported: the pinned MLIR SDK is a Release build with
    the dynamic Release CRT, so a debug-CRT compiler would fail to link.

.PARAMETER Clean
    Delete the configuration's build directory before configuring.

.PARAMETER Target
    Optional CMake target to build instead of everything.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string]$Configuration = 'RelWithDebInfo',
    [switch]$Clean,
    [string]$Target = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$toolchainPath = Join-Path $RepoRoot 'toolchain/toolchain.json'
if (-not (Test-Path $toolchainPath)) {
    Write-Host 'toolchain/toolchain.json is missing. Run .\scripts\bootstrap.ps1 first.' -ForegroundColor Red
    exit 4
}
$tc = Get-Content $toolchainPath -Raw | ConvertFrom-Json

$preset = if ($Configuration -eq 'Release') { 'windows-msvc-release' } else { 'windows-msvc-dev' }
$relativeBuildDir = if ($Configuration -eq 'Release') { 'build/msvc-release' } else { 'build/msvc-dev' }
$buildDir = Join-Path $RepoRoot $relativeBuildDir

if ($Clean) {
    if (Test-Path $buildDir) {
        Write-Host "==> removing $buildDir"
        Remove-Item $buildDir -Recurse -Force
    }
}

$logDir = Join-Path $RepoRoot 'build/logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$logPath = Join-Path $logDir ("build-" + $Configuration + ".log")

$configure = 'cmake --preset ' + $preset +
    ' -DMLIR_DIR="' + $tc.mlirDir + '"' +
    ' -DLLVM_DIR="' + $tc.llvmDir + '"' +
    ' -DCMAKE_MAKE_PROGRAM="' + $tc.ninjaExe + '"'
$build = 'cmake --build --preset ' + $preset
if ($Target) { $build += ' --target ' + $Target }

$batPath = Join-Path $env:TEMP 'nirqoba-build.bat'
$batLines = @(
    '@echo off',
    'call "' + $tc.vcvars64 + '" > "%TEMP%\nirqoba-vcvars.log" 2>&1 || exit /b 1',
    $configure + ' || exit /b 1',
    $build + ' || exit /b 1'
)
Set-Content -Path $batPath -Value $batLines -Encoding ascii

Write-Host "==> configuring and building ($Configuration, preset $preset)"
Write-Host "    log: $logPath"
& cmd.exe /c $batPath 2>&1 | Tee-Object -FilePath $logPath
$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Host "build failed with exit code $code (see $logPath)" -ForegroundColor Red
    exit $code
}
Write-Host 'build succeeded' -ForegroundColor Green
