<#
.SYNOPSIS
    Builds Nirqoba and runs the full Phase 1 test suite.

.DESCRIPTION
    Runs the C++ unit tests through ctest and the CLI/integration suite through
    tests/run_tests.py. Both logs are written to build/logs. The script exits non-zero if any
    test fails, and never reports a suite as passing when it did not run.

.PARAMETER Configuration
    Release or RelWithDebInfo.

.PARAMETER SkipBuild
    Test the existing build output instead of rebuilding first.

.PARAMETER UnitOnly
    Run ctest only, skipping the CLI/integration suite.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string]$Configuration = 'RelWithDebInfo',
    [switch]$SkipBuild,
    [switch]$UnitOnly
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
$compiler = Join-Path $buildDir 'bin/nirqoba.exe'

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$logDir = Join-Path $RepoRoot 'build/logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$unitLog = Join-Path $logDir 'ctest.log'
$integrationLog = Join-Path $logDir 'integration.log'

Write-Host "==> ctest ($preset)"
$batPath = Join-Path $env:TEMP 'nirqoba-ctest.bat'
$batContent = @"
@echo off
call "$($tc.vcvars64)" > "%TEMP%\nirqoba-vcvars.log" 2>&1 || exit /b 1
cd /d "$RepoRoot" || exit /b 1
ctest --preset $preset --output-on-failure || exit /b 1
"@
Set-Content -Path $batPath -Value $batContent -Encoding ascii
& $batPath 2>&1 | Tee-Object -FilePath $unitLog
$unitCode = $LASTEXITCODE
if ($unitCode -ne 0) { Write-Host "unit tests failed (exit $unitCode)" -ForegroundColor Red }

if ($UnitOnly) {
    if ($unitCode -ne 0) { exit $unitCode }
    Write-Host 'unit tests passed; integration suite skipped (-UnitOnly)' -ForegroundColor Yellow
    exit 0
}

Write-Host '==> CLI/integration suite'
$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
if ($null -eq $pythonCommand) {
    Write-Host 'Python was not found on PATH. Install Python or run scripts/bootstrap.ps1.' -ForegroundColor Red
    exit 4
}
$pythonExe = $pythonCommand.Source
& $pythonExe (Join-Path $RepoRoot 'tests/run_tests.py') --compiler $compiler --configuration $Configuration 2>&1 | Tee-Object -FilePath $integrationLog
$integrationCode = $LASTEXITCODE
if ($integrationCode -ne 0) { Write-Host "integration tests failed (exit $integrationCode)" -ForegroundColor Red }

if ($unitCode -ne 0 -or $integrationCode -ne 0) { exit 1 }
Write-Host 'all tests passed' -ForegroundColor Green
exit 0
