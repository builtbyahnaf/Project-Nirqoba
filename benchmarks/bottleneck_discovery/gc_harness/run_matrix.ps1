# Builds the harness for every commit in the ColO-01 range and runs test 02,
# test 03 and the allocation probe on each. Raw output is written to
# build/bottleneck_discovery/raw/. Diagnostic experiment; not part of the build.
param(
    [string[]]$Revs = @(
        '98a1099', # 0.257-beta: last release before ColO-01
        'f1d2b42', # free-span ordering fix
        '1ca3c37', # page index for reusable runs
        'e830b65', # page-ordered sweep
        '1646b93', # minor skips pages without young objects
        '0708f75', # byte-sized slices (0.260-beta code)
        '5d62abf', # narrow-layout marking fast path
        'HEAD'
    ),
    [string[]]$Modes = @('02', '03', 'alloc', '01'),
    [int]$Repeats = 3
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$raw = Join-Path $repo 'build/bottleneck_discovery/raw'
New-Item -ItemType Directory -Force -Path $raw | Out-Null

foreach ($rev in $Revs) {
    $sha = (& git -C $repo rev-parse --short $rev).Trim()
    $exe = Join-Path $repo "build/bottleneck_discovery/$sha/gc_harness_$sha.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "== building $sha"
        & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build_and_run.ps1') -Rev $rev | Out-Null
        if (-not (Test-Path $exe)) { throw "build failed for $sha" }
    }
    foreach ($mode in $Modes) {
        for ($r = 1; $r -le $Repeats; $r++) {
            $path = Join-Path $raw "$($sha)_$($mode)_run$r.txt"
            if (Test-Path $path) { continue }
            Write-Host "== run $sha mode=$mode repeat=$r"
            & $exe $mode | Out-File -FilePath $path -Encoding utf8
        }
    }
}
Write-Host 'matrix complete'
