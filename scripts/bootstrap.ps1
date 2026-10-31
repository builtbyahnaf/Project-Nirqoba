<#
.SYNOPSIS
    Verifies and prepares the pinned Nirqoba developer toolchain.

.DESCRIPTION
    Checks every prerequisite named in toolchain/versions.lock.json, downloads the pinned
    LLVM/MLIR SDK if it is absent, verifies its SHA-256, extracts it, and writes
    toolchain/toolchain.json describing where the compiler, linker and SDK live.

    The script is idempotent: a complete toolchain is reported and left alone unless -Force is used.

.PARAMETER Force
    Redownload and re-extract the LLVM/MLIR SDK even when it is already present.

.PARAMETER SkipToolchain
    Verify the local prerequisites only. Useful on CI images that pre-populate the SDK.

.NOTES
    Writes: toolchain/toolchain.json (gitignored)
    Exits non-zero on the first unmet prerequisite, naming the exact remediation.
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$SkipToolchain
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:LockPath = Join-Path $RepoRoot 'toolchain/versions.lock.json'
$script:ToolchainJson = Join-Path $RepoRoot 'toolchain/toolchain.json'

function Write-Step([string]$Message) { Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Ok([string]$Message) { Write-Host "  ok  $Message" -ForegroundColor Green }
function Stop-WithError([string]$Message) {
    Write-Host "  ERR $Message" -ForegroundColor Red
    exit 1
}

function Get-CommandVersion([string]$Exe, [string[]]$Arguments) {
    $out = & $Exe @Arguments 2>&1 | Out-String
    return $out.Trim()
}

Write-Step "Reading $LockPath"
if (-not (Test-Path $LockPath)) { Stop-WithError "toolchain/versions.lock.json is missing from the repository root." }
$lock = Get-Content $LockPath -Raw | ConvertFrom-Json
$llvmVersion = $lock.llvm.version
Write-Ok "lock schema $($lock.schema), LLVM/MLIR $llvmVersion"

# --- Prerequisite: Python -----------------------------------------------------
Write-Step 'Checking Python'
$python = Get-Command py -ErrorAction SilentlyContinue
if ($null -eq $python) { $python = Get-Command python -ErrorAction SilentlyContinue }
if ($null -eq $python) {
    Stop-WithError "Python $($lock.python.minimum)+ not found. Install it with: winget install Python.Python.3.12"
}
# Passed inline rather than through Get-CommandVersion: PowerShell 5.1 splits an
# array argument containing a ';' when it forwards it to a native executable.
$pythonVersion = (& $python.Source -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>&1 | Out-String).Trim()
Write-Ok "python $pythonVersion"

# --- Prerequisite: CMake ------------------------------------------------------
Write-Step "Checking CMake (minimum $($lock.cmake.minimum))"
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($null -eq $cmake) {
    Stop-WithError "CMake $($lock.cmake.minimum)+ not found. Install it with: winget install Kitware.CMake"
}
$cmakeVersion = (Get-CommandVersion $cmake.Source @('--version')).Split("`n")[0].Trim()
$cmakeNumeric = ($cmakeVersion -replace '[^0-9.]', '').Split('.')[0..1] -join '.'
if ([version]$cmakeNumeric -lt [version]$lock.cmake.minimum) {
    Stop-WithError "CMake $cmakeNumeric is older than the required $($lock.cmake.minimum). Run: winget upgrade Kitware.CMake"
}
Write-Ok "$cmakeVersion"
if ($cmakeVersion -match 'rc') { Write-Host "  note: $cmakeVersion is a release candidate; the lock pins $($lock.cmake.pinned)." -ForegroundColor Yellow }

# --- Prerequisite: Ninja ------------------------------------------------------
Write-Step "Checking Ninja (minimum $($lock.ninja.minimum))"
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
$ninjaPath = $null
if ($null -eq $ninja) {
    $pipNinja = Join-Path $env:APPDATA 'Python\Python312\Scripts\ninja.exe'
    if (Test-Path $pipNinja) { $ninjaPath = $pipNinja } else {
        Stop-WithError "Ninja not found. Install it with: py -m pip install --user ninja"
    }
}
if (-not $ninjaPath) { $ninjaPath = $ninja.Source }
if (-not $ninjaPath) { $ninjaPath = $ninja.FullName }
$ninjaVersion = (Get-CommandVersion $ninjaPath @('--version'))
Write-Ok "$ninjaVersion"

# --- Prerequisite: MSVC Build Tools --------------------------------------------
Write-Step "Checking MSVC Build Tools (toolset $($lock.msvc.minimum_toolset)+)"
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = ''
if (Test-Path $vswhere) {
    $vsPath = (& $vswhere -latest -products 'Microsoft.VisualStudio.Product.BuildTools' -property installationPath 2>$null | Out-String).Trim()
    if (-not $vsPath) {
        $vsPath = (& $vswhere -latest -products '*' -property installationPath 2>$null | Out-String).Trim()
    }
}
if (-not $vsPath) {
    $fallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools'
    if (Test-Path $fallback) { $vsPath = $fallback }
}
if (-not $vsPath) {
    Stop-WithError "MSVC Build Tools not found. Install with: winget install Microsoft.VisualStudio.2022.BuildTools --override '--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'"
}
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    Stop-WithError "vcvars64.bat is missing under $vsPath. The C++ workload is incomplete; repair it with the Visual Studio Installer."
}
$msvcToolset = ''
$clExe = ''
foreach ($t in (Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending)) {
    $candidate = Join-Path $t.FullName 'bin\Hostx64\x64\cl.exe'
    if (Test-Path $candidate) { $clExe = $candidate; $msvcToolset = $t.Name; break }
}
if (-not $clExe) {
    Stop-WithError "cl.exe was not found. The C++ compiler workload is incomplete; repair it with the Visual Studio Installer."
}
Write-Ok "toolset $msvcToolset"
$msvcLib = Join-Path $vsPath ("VC\Tools\MSVC\" + $msvcToolset + "\lib\x64")
$linkExe = Join-Path $vsPath ("VC\Tools\MSVC\" + $msvcToolset + "\bin\Hostx64\x64\link.exe")
if (-not (Test-Path $msvcLib)) { Stop-WithError "MSVC x64 libraries missing at $msvcLib; the C++ workload is incomplete." }

# --- Prerequisite: Windows SDK ------------------------------------------------
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkInclude = Join-Path $sdkRoot 'Include'
if (-not (Test-Path $sdkInclude)) {
    Stop-WithError "Windows SDK headers not found under $sdkInclude. Install a Windows 10/11 SDK from the Visual Studio Installer."
}
$sdkVersion = ''
foreach ($v in (Get-ChildItem $sdkInclude -Directory | Sort-Object Name -Descending)) {
    if (Test-Path (Join-Path $sdkRoot ("Lib\" + $v.Name + "\um\x64"))) { $sdkVersion = $v.Name; break }
}
if (-not $sdkVersion) {
    Stop-WithError "Windows SDK x64 libraries (um\x64) not found under $sdkRoot\Lib. Install the Desktop C++ SDK component."
}
$sdkUcrtLib = Join-Path $sdkRoot ("Lib\" + $sdkVersion + "\ucrt\x64")
$sdkUmLib = Join-Path $sdkRoot ("Lib\" + $sdkVersion + "\um\x64")
Write-Ok "Windows SDK $sdkVersion"

# --- LLVM / MLIR SDK ----------------------------------------------------------
$llvmRoot = Join-Path $RepoRoot $lock.llvm.install_dir
$mlirDir = Join-Path $llvmRoot 'lib\cmake\mlir'
$llvmDir = Join-Path $llvmRoot 'lib\cmake\llvm'
$mlirConfig = Join-Path $mlirDir 'MLIRConfig.cmake'
$mlirTblgen = Join-Path $llvmRoot 'bin\mlir-tblgen.exe'
$lldLink = Join-Path $llvmRoot 'bin\lld-link.exe'

if ($SkipToolchain) {
    Write-Step 'Skipping LLVM/MLIR SDK (-SkipToolchain)'
} elseif ((Test-Path $mlirConfig) -and (Test-Path $mlirTblgen) -and -not $Force) {
    Write-Step "LLVM/MLIR $llvmVersion already present at $llvmRoot"
} else {
    $downloads = Join-Path $RepoRoot 'toolchain/downloads'
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    $archive = Join-Path $downloads $lock.llvm.asset
    $expectedBytes = [int64]$lock.llvm.bytes

    $needDownload = $true
    if ((Test-Path $archive) -and ((Get-Item $archive).Length -eq $expectedBytes)) { $needDownload = $false }
    if ($needDownload) {
        Write-Step "Downloading $($lock.llvm.asset) ($([math]::Round($expectedBytes / 1MB)) MiB)"
        & curl.exe -L --fail --retry 3 --continue-at - -o $archive $lock.llvm.url
        if ($LASTEXITCODE -ne 0) { Stop-WithError "Download failed with exit code $LASTEXITCODE. Check network access to github.com." }
    }
    $size = (Get-Item $archive).Length
    if ($size -ne $expectedBytes) { Stop-WithError "Archive is $size bytes, but the lock pins $expectedBytes bytes." }

    Write-Step 'Verifying SHA-256 against the lock file'
    $hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $lock.llvm.sha256) { Stop-WithError "SHA-256 mismatch. Got $hash, expected $($lock.llvm.sha256). Delete the archive and retry." }
    Write-Ok "sha256 $($hash.Substring(0, 16))..."

    Write-Step 'Extracting the SDK (this takes a few minutes)'
    if (Test-Path $llvmRoot) { Remove-Item $llvmRoot -Recurse -Force }
    
    # Extract zstd-compressed tarball
    $tempTar = $archive -replace '\.zst$', '.tar'
    Write-Step "Decompressing zstd archive"
    & zstd.exe -d --memory=1024MB -o $tempTar $archive
    if ($LASTEXITCODE -ne 0) { Stop-WithError "zstd decompression failed with exit code $LASTEXITCODE." }
    
    Write-Step "Extracting tar archive"
    & tar.exe -xf $tempTar -C $downloads
    if ($LASTEXITCODE -ne 0) { Stop-WithError "tar extraction failed with exit code $LASTEXITCODE." }
    
    $extractedName = $lock.llvm.asset -replace '\.tar\.zst$', ''
    $extracted = Join-Path $downloads $extractedName
    if (-not (Test-Path $extracted)) { 
        # Try without the .zst in name
        $extractedName = $lock.llvm.asset -replace '\.tar\.zst$', '' -replace '_noassert$', ''
        $extracted = Join-Path $downloads $extractedName
    }
    if (-not (Test-Path $extracted)) { 
        # List what was extracted
        $dirs = Get-ChildItem $downloads -Directory | Where-Object { $_.Name -ne 'llvm-mlir_llvmorg-23.1.4_x86_64-pc-windows-msvc_noassert' }
        if ($dirs.Count -eq 1) { $extracted = $dirs[0].FullName }
    }
    if (-not (Test-Path $extracted)) { Stop-WithError "Extracted directory '$extracted' was not found." }
    Move-Item $extracted $llvmRoot
    Remove-Item $tempTar -Force -ErrorAction SilentlyContinue
    Write-Ok "extracted to $llvmRoot"
}

if (-not $SkipToolchain) {
    if (-not (Test-Path $mlirConfig)) { Stop-WithError "MLIRConfig.cmake is missing at $mlirConfig. This archive does not contain an MLIR SDK." }
    if (-not (Test-Path $mlirTblgen)) { Stop-WithError "mlir-tblgen.exe is missing at $mlirTblgen." }
    Write-Ok "MLIR CMake package at $mlirDir"
    if (Test-Path $lldLink) { Write-Ok "lld-link at $lldLink" }
    else { Write-Host '  note: lld-link.exe not found; the driver falls back to MSVC link.exe.' -ForegroundColor Yellow }
}

# --- Record the resolved toolchain -------------------------------------------
$toolchain = [ordered]@{
    schema         = 1
    generatedAt    = (Get-Date).ToString('o')
    llvmVersion    = $llvmVersion
    llvmPrefix     = $llvmRoot
    mlirDir        = $mlirDir
    llvmDir        = $llvmDir
    mlirTblgen     = $mlirTblgen
    linker         = $(if (Test-Path $lldLink) { $lldLink } else { $null })
    linkerFallback = $linkExe
    vcvars64       = $vcvars
    msvcToolset    = $msvcToolset
    clExe          = $clExe
    msvcLib        = $msvcLib
    sdkVersion     = $sdkVersion
    sdkUcrtLib     = $sdkUcrtLib
    sdkUmLib       = $sdkUmLib
    cmakeVersion   = $cmakeVersion
    ninjaVersion   = $ninjaVersion
    ninjaExe       = $ninjaPath
    pythonVersion  = $pythonVersion
}
$toolchain | ConvertTo-Json -Depth 5 | Set-Content -Path $ToolchainJson -Encoding utf8
Write-Ok "wrote $ToolchainJson"

Write-Host ''
Write-Host 'Toolchain ready. Next:  .\scripts\build.ps1' -ForegroundColor Green