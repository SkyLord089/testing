#requires -Version 5.1
<#
.SYNOPSIS
  Read-only preflight check for the RX 580 + ComfyUI setup. Changes nothing.
.PARAMETER InstallRoot
  Target install root (same as in Install-ComfyUI-RX580.ps1). Used only to check
  free space on the right drive, e.g. -InstallRoot 'D:\RX580-ComfyUI'.
.EXAMPLE
  .\Test-Prerequisites.ps1 -InstallRoot D:\RX580-ComfyUI
#>
[CmdletBinding()]
param([string]$InstallRoot)

$ErrorActionPreference = 'Continue'
$script:Fails = 0
$script:Warns = 0

function Ok($m)   { Write-Host ("  [OK]   " + $m) -ForegroundColor Green }
function Warn($m) { Write-Host ("  [WARN] " + $m) -ForegroundColor Yellow; $script:Warns++ }
function Fail($m) { Write-Host ("  [FAIL] " + $m) -ForegroundColor Red;    $script:Fails++ }
function Info($m) { Write-Host ("         " + $m) -ForegroundColor DarkGray }
function Section($m) { Write-Host ""; Write-Host ("== " + $m) -ForegroundColor Cyan }

function Find-RocmBin {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($e in 'HIP_PATH','HIP_HOME','ROCM_PATH') {
        $v = [Environment]::GetEnvironmentVariable($e)
        if ($v) { $candidates.Add($v); $candidates.Add(($v.TrimEnd('\') + '\bin')) }
    }
    $base = Join-Path $env:ProgramFiles 'AMD\ROCm'
    if (Test-Path -LiteralPath $base) {
        Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { $candidates.Add((Join-Path $_.FullName 'bin')) }
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'rocblas'))) { return (Resolve-Path -LiteralPath $c).Path }
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return $null
}

Write-Host "RX 580 + Windows 11 preflight check (read-only)" -ForegroundColor White

Section 'Operating system'
try {
    $os = Get-CimInstance Win32_OperatingSystem
    Ok ("{0} (build {1}), {2}-bit" -f $os.Caption, $os.BuildNumber, $os.OSArchitecture)
    if ([int]$os.BuildNumber -lt 19041) { Warn 'Windows 10 2004+ recommended' }
} catch { Fail "Cannot query OS: $($_.Exception.Message)" }

Section 'GPU'
try {
    $gpus = Get-CimInstance Win32_VideoController
    $found580 = $false
    foreach ($g in $gpus) {
        Info ("GPU: {0} | driver {1}" -f $g.Name, $g.DriverVersion)
        if ($g.Name -match 'RX\s*(5[67]0|580)|Polaris') { $found580 = $true }
    }
    if ($found580) { Ok 'Polaris-class GPU (RX 570/580) detected' }
    else { Warn 'RX 580 not found among the listed GPUs. This kit targets gfx803; other archs need different patches.' }
} catch { Fail "Cannot query GPUs: $($_.Exception.Message)" }

Section 'Visual C++ runtime'
if (Test-Path -LiteralPath "$env:SystemRoot\System32\vcruntime140.dll") { Ok 'vcruntime140.dll present' }
else { Warn 'VC++ x64 runtime missing - install https://aka.ms/vs/17/release/vc_redist.x64.exe' }

Section 'Python'
$py = Get-Command python.exe -ErrorAction SilentlyContinue
if ($py) {
    try {
        $v = (& python.exe -c "import sys;print('%d.%d.%d'%sys.version_info[:3])").Trim()
        $bits = (& python.exe -c "import struct;print(struct.calcsize('P')*8)").Trim()
        $ver = [version]$v
        $isStore = $py.Source -like "*WindowsApps*"
        if ($isStore)          { Warn "Python looks like the Microsoft Store build ($($py.Source)) - use python.org instead" }
        elseif ($bits -ne '64'){ Fail "Python $v is $bits-bit - 64-bit Python required" }
        elseif ($ver -lt [version]'3.10' -or $ver -ge [version]'3.13') { Warn "Python $v - supported range is 3.10-3.12 (3.11 recommended)" }
        else                   { Ok "Python $v (64-bit): $($py.Source)" }
    } catch { Fail "python.exe found but failed to run: $($py.Source)" }
} else { Fail 'python.exe not in PATH - install Python 3.11 from python.org (tick "Add Python to PATH")' }

Section 'Git'
$git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($git) { Ok ("git: {0}" -f $git.Source) } else { Fail 'git not in PATH - install from https://git-scm.com/download/win' }

Section 'HIP SDK / ROCm'
$rocmBin = Find-RocmBin
if ($rocmBin) {
    Ok "ROCm bin found: $rocmBin"
    $verDir = Split-Path (Split-Path $rocmBin -Parent) -Leaf
    Info "Detected ROCm version folder: $verDir"
    $hipDll = Get-ChildItem -LiteralPath $rocmBin -Filter 'amdhip64.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hipDll) { Ok 'amdhip64.dll present (HIP runtime found)' }
    else { Warn 'amdhip64.dll NOT in that bin dir - is the HIP SDK runtime component installed?' }
    $pathEntries = @($env:Path -split ';' | ForEach-Object { $_.TrimEnd('\') })
    if ($pathEntries -contains $rocmBin.TrimEnd('\')) { Ok 'ROCm bin is on PATH' }
    else { Warn "ROCm bin is NOT on PATH - 'import torch' will die with WinError 126/127. Add $rocmBin to user PATH (System Properties > Environment Variables)" }
    if (Test-Path -LiteralPath (Join-Path $rocmBin 'rocblas\gfx803.zluda-patched')) { Ok 'rocBLAS gfx803 patch marker found' }
    else { Warn 'gfx803 rocBLAS patch NOT applied yet - run .\Patch-RocBLAS-gfx803.ps1 (elevated)' }
    if ($rocmBin -notmatch '6\.4') { Warn 'This looks like a non-6.4 ROCm. HIP SDK 6.4 is what this kit targets; 6.2.4 is the fully supported fallback for the patch.' }
} else {
    Fail 'HIP SDK not found. Install it manually from https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html (6.4, or 6.2.4 fallback), then re-run.'
}

Section 'Virtual memory (pagefile)'
try {
    $pf = Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue
    if ($pf) {
        $total = ($pf | Measure-Object -Property AllocatedBaseSize -Sum).Sum
        Info ("Pagefile allocated: {0:N0} MB" -f $total)
        if ($total -lt 12000) { Warn 'Pagefile < ~12 GB - ComfyUI on 8 GB VRAM usually wants >= 16 GB pagefile (System > Advanced > Virtual memory)' }
        else { Ok 'Pagefile looks sufficient' }
    } else {
        $auto = (Get-CimInstance Win32_ComputerSystem).AutomaticManagedPagefile
        if ($auto) { Info 'Windows manages the pagefile automatically - make sure >= 16 GB is actually available on the system drive' }
        else { Warn 'No pagefile detected at all - strongly recommended >= 16 GB' }
    }
} catch { Warn "Could not query pagefile: $($_.Exception.Message)" }

Section 'Disk space'
try {
    $targetDrive = $null
    if ($InstallRoot) {
        $qualifier = Split-Path -Qualifier $InstallRoot -ErrorAction SilentlyContinue
        if ($qualifier -match '^[A-Za-z]:') { $targetDrive = $qualifier.TrimEnd(':').ToUpper() }
        else { Warn ("Could not parse a drive letter from '{0}' - checking system drive instead" -f $InstallRoot) }
    }
    if (-not $targetDrive) { $targetDrive = ($env:SystemDrive).TrimEnd(':') }
    $drive = Get-PSDrive -Name $targetDrive -ErrorAction Stop
    $freeGB = [math]::Round($drive.Free / 1GB, 1)
    $where = $(if ($InstallRoot) { $InstallRoot } else { 'default %USERPROFILE%\RX580-ComfyUI' })
    Info ("Target: {0}  |  free on {1}:\ : {2} GB" -f $where, $targetDrive, $freeGB)
    if ($freeGB -lt 25) { Warn ("Less than 25 GB free on {0}: - torch (~2.5 GB) + models + ComfyUI want more headroom" -f $targetDrive) }
    else { Ok 'Enough free space for install + a couple of SD 1.5 checkpoints' }
} catch { Warn "Could not query the target drive: $($_.Exception.Message)" }

Section 'Summary'
Write-Host ("  FAIL: {0}   WARN: {1}" -f $script:Fails, $script:Warns)
if ($script:Fails -gt 0) { Write-Host '  Fix the FAIL items, then re-run.' -ForegroundColor Red; exit 1 }
Write-Host '  Preflight done. Next: .\Patch-RocBLAS-gfx803.ps1 (elevated), then .\Install-ComfyUI-RX580.ps1' -ForegroundColor Green
exit 0
