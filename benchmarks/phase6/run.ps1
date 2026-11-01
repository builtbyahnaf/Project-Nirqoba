[CmdletBinding()]
param([int]$Iterations=1000000,[int]$Warmups=5,[int]$Runs=30,[string]$Output='benchmark-results.json')
$ErrorActionPreference='Stop'
if($Iterations -lt 1 -or $Warmups -lt 0 -or $Runs -lt 1){throw 'Iterations/runs must be positive and warmups nonnegative.'}
$root=$PSScriptRoot
$cppExe=Join-Path $root 'allocation-cpp.exe';$rsExe=Join-Path $root 'allocation-rust.exe'
$cl=Get-Command cl.exe -ErrorAction SilentlyContinue
if(-not $cl){throw 'cl.exe is required; run from a configured Visual Studio x64 developer shell.'}
$rustc=Get-Command rustc.exe -ErrorAction SilentlyContinue
if(-not $rustc){throw 'rustc.exe is required to run the C++/Rust comparison; no partial comparison will be reported.'}
$temp=Join-Path ([IO.Path]::GetTempPath()) ('nrq-bench-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $temp|Out-Null
try {
  Push-Location $temp
  try {
    & $cl.Source /nologo /O2 /std:c++20 $root\allocation.cpp "/Fe:$cppExe" | Out-Null
    if($LASTEXITCODE -ne 0){throw 'C++ benchmark build failed.'}
    & $rustc.Source -C opt-level=3 $root\allocation.rs -o $rsExe
    if($LASTEXITCODE -ne 0){throw 'Rust benchmark build failed.'}
  } finally { Pop-Location }
  function Invoke-Samples([string]$Language,[string]$Mode,[string]$Exe){
    for($i=0;$i -lt $Warmups;$i++){& $Exe $Iterations $Mode | Out-Null;if($LASTEXITCODE -ne 0){throw "$Language/$Mode warmup failed"}}
    $times=@();$checksums=@()
    for($i=0;$i -lt $Runs;$i++){
      $timer=[Diagnostics.Stopwatch]::StartNew();$line=& $Exe $Iterations $Mode;$timer.Stop()
      if($LASTEXITCODE -ne 0){throw "$Language/$Mode failed on run $i"}
      $text=$line -join ''; $times += [uint64]([regex]::Match($text,'ns=(\d+)').Groups[1].Value)
      $checksums += [uint64]([regex]::Match($text,'checksum=(\d+)').Groups[1].Value)
    }
    if(@($checksums|Select-Object -Unique).Count -ne 1){throw "$Language/$Mode produced unstable checksums"}
    $sorted=@($times|Sort-Object)
    [pscustomobject]@{language=$Language;mode=$Mode;samples_ns=$times;p50_ns=$sorted[[int][math]::Floor(($Runs-1)*.50)];p95_ns=$sorted[[int][math]::Ceiling(($Runs-1)*.95)];p99_ns=$sorted[[int][math]::Ceiling(($Runs-1)*.99)];checksum=$checksums[0]}
  }
  $results=@(); foreach($mode in @('scalar','array','alloc_small','alloc_large','alloc_mixed','alloc','graph','threads')){$results+=Invoke-Samples 'cpp' $mode $cppExe;$results+=Invoke-Samples 'rust' $mode $rsExe}
  for($i=0;$i -lt $results.Count;$i+=2){if($results[$i].checksum -ne $results[$i+1].checksum){throw "C++/Rust checksum mismatch for $($results[$i].mode)"}}
  $cpu=(Get-CimInstance Win32_Processor|Select-Object -First 1 -ExpandProperty Name)
  $payload=[pscustomobject]@{timestamp=(Get-Date).ToString('o');os=[Environment]::OSVersion.VersionString;cpu=$cpu;logical_processors=[Environment]::ProcessorCount;iterations=$Iterations;warmups=$Warmups;runs=$Runs;cxx_compiler=((& $cl.Source /Bv 2>&1|Select-Object -First 2)-join ' ');rustc_version=(& $rustc.Source --version);cxx_flags='/O2 /std:c++20';rust_flags='-C opt-level=3';results=$results}
  $payload|ConvertTo-Json -Depth 7|Set-Content -Encoding utf8 $Output
  $payload.results|Format-Table language,mode,p50_ns,p95_ns,p99_ns,checksum
  Write-Host "Full metadata and samples: $Output"
} finally { Remove-Item -LiteralPath $temp -Recurse -Force }
