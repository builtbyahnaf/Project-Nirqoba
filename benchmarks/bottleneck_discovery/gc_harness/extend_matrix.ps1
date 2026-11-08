# Adds repeats 4..8 to the GC regression matrix for the key revisions.
# Diagnostic only; raw output goes next to the existing runs.
param(
    [int]$First = 4,
    [int]$Last = 8
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$raw = Join-Path $repo 'build/bottleneck_discovery/raw'
$revs = @('98a1099', 'f1d2b42', '1ca3c37', 'e830b65', '5d62abf', '904aee0')
foreach ($sha in $revs) {
    $exe = Join-Path $repo "build/bottleneck_discovery/$sha/gc_harness_$sha.exe"
    if (-not (Test-Path $exe)) { throw "missing $exe" }
    foreach ($mode in @('02', '03')) {
        for ($r = $First; $r -le $Last; $r++) {
            $path = Join-Path $raw "${sha}_${mode}_run$r.txt"
            if (Test-Path $path) { continue }
            Write-Host "== run $sha mode=$mode repeat=$r"
            & $exe $mode | Out-File -FilePath $path -Encoding utf8
        }
    }
}
Write-Host 'extended matrix complete'
