#requires -Version 5.1
<#
.SYNOPSIS
  Backs up and replaces HIP SDK rocBLAS with the community gfx803/gfx900 build.
  Required because HIP SDK ships NO rocBLAS kernels for gfx803 (RX 580).
  v1.0.0 targets HIP SDK 5.7.1 / 6.2.4 ONLY (6.4 confirmed broken: WinError 127).
.DESCRIPTION
  - Locates the ROCm/HIP bin dir (default target: 6.2; falls back to HIP_PATH / newest installed ROCm)
  - Makes full backups: rocblas.dll -> rocblas.dll.zluda-bak ; rocblas\library -> rocblas\library.zluda-bak
  - Downloads the advanced-lvl-up gfx803/gfx900 patch (v1.0.0) and overlays it
  - Leaves a marker file rocblas\gfx803.zluda-patched for Test-Prerequisites/Verify-Setup
  Re-run safe. Requires Administrator (auto-elevates).
.PARAMETER RocmVersion
  Which ROCm tree to patch. Default 6.2. The v1.0.0 patch binary was built for HIP SDK 5.7.1 and 6.2.4;
  on 6.4 it replaces host rocblas.dll with a build LACKING the rocblas_*_64 entry points that
  hipblas.dll 6.4 imports -> WinError 127, torch fails to load (confirmed on hardware, 2026-11-06).
  Do NOT use 6.4. Use 6.2 (or 5.7.1).
.PARAMETER Force
  Overwrite existing .zluda-bak backups (default keeps the original first backup).
.EXAMPLE
  .\Patch-RocBLAS-gfx803.ps1
.EXAMPLE
  .\Patch-RocBLAS-gfx803.ps1 -RocmVersion 6.2
#>
[CmdletBinding()]
param(
    [string]$RocmVersion = '6.2',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Info($m) { Write-Host ("   " + $m) -ForegroundColor Gray }
function Warn2($m){ Write-Host ("   [WARN] " + $m) -ForegroundColor Yellow }
function Die($m)  { Write-Host ("   [ERROR] " + $m) -ForegroundColor Red; exit 1 }

# ---- elevation ----
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host 'Relaunching with Administrator rights...' -ForegroundColor Yellow
    $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"{0}"' -f $PSCommandPath),'-RocmVersion',$RocmVersion)
    if ($Force) { $argList += '-Force' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    exit 0
}

Write-Host ''
Write-Host ("== Patching rocBLAS for gfx803 (target ROCm {0})" -f $RocmVersion) -ForegroundColor Cyan
if ($RocmVersion -eq '6.4') {
    Warn2 'HIP SDK 6.4 is a KNOWN-BROKEN combo with this patch (2026-11-06, confirmed on hardware):'
    Warn2 'its hipblas.dll imports rocblas_*_64 entry points that the patched (6.2.4/5.7.1-built) rocblas.dll does NOT export -> WinError 127, torch will not import.'
    Warn2 'Use HIP SDK 6.2.4 instead: .\Patch-RocBLAS-gfx803.ps1 -RocmVersion 6.2'
    Warn2 'Continuing with 6.4 anyway; revert with .\Restore-RocBLAS.ps1 -RocmVersion 6.4 (loses gfx803 BLAS kernels).'
}
if ($RocmVersion -like '7.*') { Die 'HIP SDK 7.x is NOT supported by ZLUDA. Use 6.2.4 / 5.7.1.' }

# ---- locate ROCm bin ----
$candidates = New-Object System.Collections.Generic.List[string]
foreach ($e in 'HIP_PATH','HIP_HOME','ROCM_PATH') {
    $v = [Environment]::GetEnvironmentVariable($e)
    if ($v) { $candidates.Add($v); $candidates.Add(($v.TrimEnd('\') + '\bin')) }
}
$base = Join-Path $env:ProgramFiles 'AMD\ROCm'
if ($RocmVersion) { $candidates.Add((Join-Path (Join-Path $base $RocmVersion) 'bin')) }
if (Test-Path -LiteralPath $base) {
    Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { $candidates.Add((Join-Path $_.FullName 'bin')) }
}
$rocmBin = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path -LiteralPath (Join-Path $c 'rocblas'))) { $rocmBin = (Resolve-Path -LiteralPath $c).Path; break }
}
if (-not $rocmBin) {
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { $rocmBin = (Resolve-Path -LiteralPath $c).Path; break }
    }
}
if (-not $rocmBin) {
    Write-Host '   HIP SDK not found.' -ForegroundColor Red
    Write-Host '   Install it manually from AMD: https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html' -ForegroundColor Yellow
    Start-Process 'https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html' | Out-Null
    Die 'Install HIP SDK (6.2.4, or 5.7.1), then re-run.'
}
Info ("ROCm bin: {0}" -f $rocmBin)
$rocmFolder = Split-Path (Split-Path $rocmBin -Parent) -Leaf
if ($rocmFolder -ne $RocmVersion) {
    Warn2 ("Resolved ROCm '{0}' but '{1}' was requested. PATH must contain ONLY the bin of your chosen ROCm (check both user and system PATH)." -f $rocmFolder, $RocmVersion)
}
$rocblasDir = Join-Path $rocmBin 'rocblas'
$libraryDir = Join-Path $rocblasDir 'library'
$rocblasDll = Join-Path $rocmBin 'rocblas.dll'

# ---- backups ----
$dllBak  = $rocblasDll + '.zluda-bak'
$libBak  = $libraryDir + '.zluda-bak'
if (Test-Path -LiteralPath $rocblasDll) {
    if ($Force -or -not (Test-Path -LiteralPath $dllBak)) {
        Copy-Item -LiteralPath $rocblasDll -Destination $dllBak -Force
        Info ("backup: {0}" -f $dllBak)
    } else { Info 'rocblas.dll backup already exists - keeping it (use -Force to overwrite)' }
} else {
    Die ("rocblas.dll not found in {0} - is the rocBLAS component of HIP SDK installed?" -f $rocmBin)
}
if (Test-Path -LiteralPath $libraryDir) {
    if ($Force -or -not (Test-Path -LiteralPath $libBak)) {
        Info 'backing up rocblas\library (may take a minute)...'
        if (Test-Path -LiteralPath $libBak) { Remove-Item -LiteralPath $libBak -Recurse -Force }
        Copy-Item -LiteralPath $libraryDir -Destination $libBak -Recurse -Force
        Info ("backup: {0}" -f $libBak)
    } else { Info 'rocblas\library backup already exists - keeping it (use -Force to overwrite)' }
} else {
    Warn2 'rocblas\library not found - the patch will create a fresh one.'
    New-Item -ItemType Directory -Force -Path $libraryDir | Out-Null
}

# ---- download + extract patch ----
$patchUrl = 'https://github.com/advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU/releases/download/v1.0.0/rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z'
$tmpRoot  = Join-Path $env:TEMP ('gfx803-patch-' + [IO.Path]::GetRandomFileName())
$tmp7z    = Join-Path $tmpRoot 'patch.7z'
New-Item -ItemType Directory -Force -Path $tmpRoot | Out-Null
try {
    Info ("downloading patch from advanced-lvl-up v1.0.0...")
    Invoke-WebRequest -Uri $patchUrl -OutFile $tmp7z -UseBasicParsing -TimeoutSec 1800
    $extractDir = Join-Path $tmpRoot 'extracted'
    New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    $sevenZip = Get-Command 7z.exe -ErrorAction SilentlyContinue
    $extracted = $false
    if ($tar) {
        & tar.exe -xf $tmp7z -C $extractDir
        if ($LASTEXITCODE -eq 0) { $extracted = $true }
    }
    if (-not $extracted -and $sevenZip) {
        & 7z.exe x -y ('-o' + $extractDir) $tmp7z | Out-Null
        if ($LASTEXITCODE -eq 0) { $extracted = $true }
    }
    if (-not $extracted) { Die 'Could not extract the .7z patch. Install 7-Zip (winget install 7zip.7zip) and re-run.' }

    $patchedDll = Get-ChildItem -LiteralPath $extractDir -Recurse -Filter 'rocblas.dll' -ErrorAction SilentlyContinue |
        Sort-Object Length -Descending | Select-Object -First 1
    $patchedLib = Get-ChildItem -LiteralPath $extractDir -Recurse -Directory -Filter 'library' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $patchedDll -or -not $patchedLib) {
        Die 'Patch archive layout is not recognized (expected rocblas.dll and a library\ folder). Aborting WITHOUT touching HIP SDK.'
    }

    Copy-Item -LiteralPath $patchedDll.FullName -Destination $rocblasDll -Force
    Info ("installed patched rocblas.dll -> {0}" -f $rocblasDll)
    & robocopy.exe $patchedLib.FullName $libraryDir /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Die ("robocopy failed with code {0}" -f $LASTEXITCODE) }
    Info ("overlaid gfx803 kernels -> {0}" -f $libraryDir)

    $marker = Join-Path $rocblasDir 'gfx803.zluda-patched'
    @(
        ("patched: {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
        ("source: {0}" -f $patchUrl)
        ("target ROCm: {0}" -f $rocmBin)
        "backups: rocblas.dll.zluda-bak ; library.zluda-bak"
    ) | Set-Content -LiteralPath $marker -Encoding ASCII
    Info ("marker: {0}" -f $marker)
} finally {
    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host 'rocBLAS gfx803 patch applied.' -ForegroundColor Green
Write-Host 'Reboot the PC, then run .\Verify-Setup.ps1 and launch ComfyUI via run-comfyui.bat.' -ForegroundColor Green
Write-Host 'Undo at any time: .\Restore-RocBLAS.ps1 -RocmVersion' $RocmVersion -ForegroundColor DarkGray
exit 0
