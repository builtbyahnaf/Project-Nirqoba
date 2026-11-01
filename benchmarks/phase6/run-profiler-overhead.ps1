[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Compiler, [int]$Warmups=5, [int]$Runs=30, [string]$Output='profiler-overhead.json')
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$temp=Join-Path ([IO.Path]::GetTempPath()) ('nrq-profile-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
  $bins=@{}
  foreach ($mode in @('off','on')) {
    $source=Join-Path $root "profile-$mode.nrqba"
    $exe=Join-Path $temp "profile-$mode.exe"
    & $Compiler build $source -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Could not build profiler $mode workload" }
    $bins[$mode]=$exe
  }
  function Measure-Workload([string]$mode) {
    for($i=0;$i -lt $Warmups;$i++){ & $bins[$mode] | Out-Null }
    $values=@()
    for($i=0;$i -lt $Runs;$i++){
      $watch=[Diagnostics.Stopwatch]::StartNew(); & $bins[$mode] | Out-Null; $watch.Stop()
      if($LASTEXITCODE -gt 127){ throw "$mode workload failed with exit $LASTEXITCODE" }
      $values += $watch.Elapsed.TotalMilliseconds
    }
    $s=@($values | Sort-Object)
    [pscustomobject]@{mode=$mode;samples_ms=$values;p50_ms=$s[[int][math]::Floor(($Runs-1)*.50)];p95_ms=$s[[int][math]::Ceiling(($Runs-1)*.95)];p99_ms=$s[[int][math]::Ceiling(($Runs-1)*.99)]}
  }
  $offResult=Measure-Workload 'off'
  $onResult=Measure-Workload 'on'
  $data=@($offResult,$onResult)
  $payload=[pscustomobject]@{date=(Get-Date).ToString('o');compiler=(Resolve-Path $Compiler).Path;warmups=$Warmups;runs=$Runs;iterations=100000;results=$data}
  $payload | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $Output
  $payload.results | Format-Table mode,p50_ms,p95_ms,p99_ms
} finally { Remove-Item -LiteralPath $temp -Recurse -Force }
