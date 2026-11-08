# Compiles the header-layout feasibility probe with the pinned MSVC toolset.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$tc = Get-Content (Join-Path $repo 'toolchain/toolchain.json') -Raw | ConvertFrom-Json
$dest = Join-Path $repo 'build/bottleneck_discovery/layout_probe'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$bat = Join-Path $dest 'build.bat'
@"
@echo off
call "$($tc.vcvars64)" > /dev/null 2>&1 || exit /b 1
cl /nologo /O2 /std:c++20 /EHsc /MD /W0 /Fe:"$dest\layout_candidates.exe" "$PSScriptRoot\layout_candidates.cpp"
"@ | Set-Content -Path $bat -Encoding ascii
& $bat | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'layout probe compile failed' }
& "$dest\layout_candidates.exe"
