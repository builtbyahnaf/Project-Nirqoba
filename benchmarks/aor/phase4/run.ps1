[CmdletBinding()]
param(
  [int]$Objects = 200000,
  [int]$Runs = 9,
  [string]$BuildDirectory = 'build/msvc-dev'
)
$ErrorActionPreference = 'Stop'
if ($Objects -lt 16) { throw 'Objects must be at least 16.' }
if ($Runs -lt 1) { throw 'Runs must be positive.' }

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$build = Join-Path $root $BuildDirectory
if (-not (Test-Path $build)) { throw "Build directory not found: $build" }

# The benchmark is its own target, so the default build does not produce it.
& cmake --build $build --target nirqoba_phase4_benchmark
if ($LASTEXITCODE -ne 0) { throw 'Benchmark build failed.' }

$exe = Get-ChildItem -Path $build -Filter nirqoba_phase4_benchmark.exe -Recurse |
  Select-Object -First 1
if (-not $exe) { throw 'nirqoba_phase4_benchmark.exe was not produced.' }

$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
Write-Host "host=$cpu os=$([Environment]::OSVersion.VersionString) processors=$([Environment]::ProcessorCount)"
Write-Host "objects=$Objects runs=$Runs build=$BuildDirectory"
& $exe.FullName $Objects $Runs
if ($LASTEXITCODE -ne 0) { throw "Benchmark exited with $LASTEXITCODE." }
