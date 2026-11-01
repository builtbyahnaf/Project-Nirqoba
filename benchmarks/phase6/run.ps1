[CmdletBinding()]
param(
  [int]$Iterations = 1000000,
  [int]$Warmups = 5,
  [int]$Runs = 30,
  [string]$Output = "benchmark-results.json"
)
$ErrorActionPreference = 'Stop'
if ($Iterations -lt 1 -or $Warmups -lt 0 -or $Runs -lt 1) { throw 'Iterations/runs must be positive and warmups nonnegative.' }
$root = $PSScriptRoot
$cpp = Join-Path $root 'allocation.cpp'
$rs = Join-Path $root 'allocation.rs'
$cppExe = Join-Path $root 'allocation-cpp.exe'
$rsExe = Join-Path $root 'allocation-rust.exe'
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) { throw 'cl.exe is required; run from a configured Visual Studio x64 developer shell.' }
& $cl.Source /nologo /O2 /std:c++20 $cpp "/Fe:$cppExe" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'C++ benchmark build failed.' }
$rustc = Get-Command rustc.exe -ErrorAction SilentlyContinue
if (-not $rustc) { throw 'rustc.exe is required to build the Rust baseline.' }
& $rustc.Source -C opt-level=3 $rs -o $rsExe
if ($LASTEXITCODE -ne 0) { throw 'Rust benchmark build failed.' }
function Invoke-Samples([string]$Name, [string]$Exe) {
  for ($i=0; $i -lt $Warmups; $i++) { & $Exe $Iterations | Out-Null }
  $samples = @()
  for ($i=0; $i -lt $Runs; $i++) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $line = & $Exe $Iterations
    $timer.Stop()
    if ($LASTEXITCODE -ne 0) { throw "$Name benchmark failed on run $i" }
    $ns = [uint64]([regex]::Match(($line -join ''), 'ns=(\d+)').Groups[1].Value)
    $sum = [uint64]([regex]::Match(($line -join ''), 'checksum=(\d+)').Groups[1].Value)
    $samples += $ns
  }
  $sorted = @($samples | Sort-Object)
  return [pscustomobject]@{ name=$Name; samples_ns=$samples; median_ns=$sorted[[int][math]::Floor(($Runs-1)*0.50)]; p95_ns=$sorted[[int][math]::Ceiling(($Runs-1)*0.95)]; p99_ns=$sorted[[int][math]::Ceiling(($Runs-1)*0.99)]; checksum=$sum }
}
$results = @((Invoke-Samples 'cpp' $cppExe), (Invoke-Samples 'rust' $rsExe))
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)
$compilerVersion = (& cl.exe /Bv 2>&1 | Select-Object -First 2) -join ' '
$rustVersion = (& rustc.exe --version) -join ' '
$payload = [pscustomobject]@{ timestamp=(Get-Date).ToString('o'); os=[Environment]::OSVersion.VersionString; cpu=$cpu; logical_processors=[Environment]::ProcessorCount; iterations=$Iterations; warmups=$Warmups; runs=$Runs; cxx_compiler=$compilerVersion; rustc_version=$rustVersion; results=$results }
$payload | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 $Output
$payload.results | Format-Table name,median_ns,p95_ns,p99_ns,checksum
Write-Host "Full metadata and samples: $Output"

