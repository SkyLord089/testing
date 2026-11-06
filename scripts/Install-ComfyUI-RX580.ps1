#requires -Version 5.1
<#
.SYNOPSIS
  Installs ComfyUI + PyTorch (cu118 wheel, for ZLUDA) + patched ZLUDA for RX 580 (gfx803) on Windows 11.
.DESCRIPTION
  Steps:
    1) sanity checks (Windows, python, git, HIP SDK discovery)
    2) clone comfyanonymous/ComfyUI into <InstallRoot>\ComfyUI
    3) venv + ComfyUI requirements
    4) pip install torch (cu118 index) - pinned 2.7.0 by default
     5) download ZLUDA v3.9.5 (rocm6 + rocm5), merge payload; nvcuda.dll: rocm5 swap on HIP 5.7/6.2, rocm6 kept on HIP 6.4 (both switchable)
    6) patch <venv>\Lib\site-packages\torch\lib DLLs with ZLUDA builds (originals kept as *.orig)
    7) generate <InstallRoot>\run-comfyui.bat launcher
  Nothing is installed system-wide; HIP SDK itself must be installed manually beforehand.
.EXAMPLE
  .\Install-ComfyUI-RX580.ps1
.EXAMPLE
  .\Install-ComfyUI-RX580.ps1 -TorchVersion 2.2.1   # battle-tested fallback pinned by ComfyUI-Zluda
.EXAMPLE
  .\Install-ComfyUI-RX580.ps1 -RocmVersion 6.2      # default (HIP SDK 6.2.4); 6.4 is confirmed broken (WinError 127)
#>
[CmdletBinding()]
param(
    [string]$InstallRoot   = (Join-Path $env:USERPROFILE 'RX580-ComfyUI'),
    [string]$RocmVersion   = '6.2',
    [string]$TorchIndexUrl = 'https://download.pytorch.org/whl/cu118',
    [string]$TorchVersion  = '2.7.0',
    [string]$VisionVersion = '0.22.0',
    [string]$AudioVersion  = '2.7.0',
    # ZLUDA v3.9.5 - last release shipping both rocm5 and rocm6 Windows builds
    [string]$ZludaTag      = 'rel.5e717459179dc272b7d7d23391f0fad66c7459cf',
    [switch]$SkipRocmCheck,
    [switch]$SkipZludaPatch,
    [switch]$KeepRocm6Nvcuda,   # always keep the rocm6 nvcuda.dll (consistent family)
    [switch]$SwapRocm5Nvcuda    # force the advanced-lvl-up recipe (nvcuda.dll from rocm5); default for HIP 5.7/6.2
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$InformationPreference = 'Continue'

function Step($m) { Write-Host ""; Write-Host ("== " + $m) -ForegroundColor Cyan }
function Info($m) { Write-Host ("   " + $m) -ForegroundColor Gray }
function Warn2($m){ Write-Host ("   [WARN] " + $m) -ForegroundColor Yellow }
function Die($m)  { Write-Host ("   [ERROR] " + $m) -ForegroundColor Red; exit 1 }

function Download-File([string]$Url, [string]$OutFile) {
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            Info ("Downloading {0}" -f $Url)
            Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing -TimeoutSec 1800
            if (Test-Path -LiteralPath $OutFile) { return }
        } catch {
            Warn2 ("Attempt {0}/3 failed: {1}" -f $attempt, $_.Exception.Message)
            Start-Sleep -Seconds 5
        }
    }
    Die ("Failed to download {0}" -f $Url)
}

function Find-RocmBin([string]$PreferredVersion) {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($e in 'HIP_PATH','HIP_HOME','ROCM_PATH') {
        $v = [Environment]::GetEnvironmentVariable($e)
        if ($v) { $candidates.Add($v); $candidates.Add(($v.TrimEnd('\') + '\bin')) }
    }
    $base = Join-Path $env:ProgramFiles 'AMD\ROCm'
    if ($PreferredVersion) { $candidates.Add((Join-Path (Join-Path $base $PreferredVersion) 'bin')) }
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

# ---------- 1. sanity checks ----------
Step '1/8  Sanity checks'
if ($env:OS -ne 'Windows_NT') { Die 'This script targets Windows only.' }

$pyCmd = Get-Command python.exe -ErrorAction SilentlyContinue
if (-not $pyCmd) { Die 'python.exe not in PATH. Install Python 3.11 from python.org (tick Add to PATH), then re-run.' }
$pyVer = (& python.exe -c "import sys;print('%d.%d'%sys.version_info[:2])").Trim()
$pyBits = (& python.exe -c "import struct;print(struct.calcsize('P')*8)").Trim()
if ($pyBits -ne '64') { Die ("Python {0} is not 64-bit." -f $pyVer) }
Info ("python {0} (64-bit) at {1}" -f $pyVer, $pyCmd.Source)

$gitCmd = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $gitCmd) { Die 'git.exe not in PATH. Install Git from https://git-scm.com/download/win' }
Info ("git at {0}" -f $gitCmd.Source)

try {
    $gpus = Get-CimInstance Win32_VideoController
    foreach ($g in $gpus) { Info ("GPU: {0}" -f $g.Name) }
    if (-not ($gpus | Where-Object { $_.Name -match 'RX\s*(5[67]0|580)|Polaris' })) {
        Warn2 'No Polaris-class GPU found. The patch is specific to gfx803; other archs are out of scope.'
    }
} catch { Warn2 ("Could not enumerate GPUs: {0}" -f $_.Exception.Message) }

if (-not $SkipRocmCheck) {
    $rocmBin = Find-RocmBin -PreferredVersion $RocmVersion
    if (-not $rocmBin) {
        Write-Host '   HIP SDK not found. It must be installed MANUALLY from AMD:' -ForegroundColor Yellow
        Write-Host '   https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html  (6.2.4, or 5.7.1)' -ForegroundColor Yellow
        Start-Process 'https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html' | Out-Null
        Die 'Install HIP SDK, then re-run this script (or pass -SkipRocmCheck to bypass).'
    }
    Info ("ROCm/HIP bin: {0}" -f $rocmBin)
    $pathEntries = @($env:Path -split ';' | ForEach-Object { $_.TrimEnd('\') })
    if ($pathEntries -contains $rocmBin.TrimEnd('\')) {
        Info 'ROCm bin is on PATH'
    } else {
        Warn2 'ROCm bin is NOT on PATH - amdhip64.dll and its dependencies will not load (classic cause of WinError 126/127 on import torch).'
        Warn2 ("Fix it (user PATH): [Environment]::SetEnvironmentVariable('Path', ([Environment]::GetEnvironmentVariable('Path','User') + ';{0}'), 'User')" -f $rocmBin)
    }
    if ($rocmBin -notmatch [regex]::Escape($RocmVersion)) {
        Warn2 ("Expected ROCm {0} but resolved {1} - make sure PATH does not shadow it with another ROCm version." -f $RocmVersion, $rocmBin)
    }
    if (-not (Get-ChildItem -LiteralPath $rocmBin -Filter 'amdhip64.dll' -ErrorAction SilentlyContinue)) {
        Warn2 'amdhip64.dll not found in the ROCm bin directory - is the HIP SDK runtime complete?'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $rocmBin 'rocblas\gfx803.zluda-patched'))) {
        Warn2 'gfx803 rocBLAS patch not applied yet. Run .\Patch-RocBLAS-gfx803.ps1 (elevated) BEFORE first launch.'
    } else {
        Info 'rocBLAS gfx803 patch marker: present'
    }
} else {
    Warn2 'Skipping ROCm discovery (-SkipRocmCheck).'
}

# ---------- 2. layout ----------
Step '2/8  Preparing install root'
$comfyDir  = Join-Path $InstallRoot 'ComfyUI'
$zludaDir  = Join-Path $InstallRoot 'zluda'
$toolsDir  = Join-Path $InstallRoot 'tools'
$venvDir   = Join-Path $comfyDir   'venv'
$venvPy    = Join-Path $venvDir    'Scripts\python.exe'
foreach ($d in @($InstallRoot, $zludaDir, $toolsDir)) {
    if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
}
Info ("InstallRoot: {0}" -f $InstallRoot)

# ---------- 3. ComfyUI ----------
Step '3/8  ComfyUI (comfyanonymous)'
if (Test-Path -LiteralPath (Join-Path $comfyDir '.git')) {
    Info 'ComfyUI already cloned - leaving it as-is (update it yourself with git pull if you want).'
} else {
    & git.exe clone --depth 1 https://github.com/comfyanonymous/ComfyUI.git $comfyDir
    if ($LASTEXITCODE -ne 0) { Die 'git clone of ComfyUI failed.' }
}

# ---------- 4. venv + requirements ----------
Step '4/8  Python venv + ComfyUI requirements'
if (-not (Test-Path -LiteralPath $venvPy)) {
    & python.exe -m venv $venvDir
    if ($LASTEXITCODE -ne 0) { Die 'venv creation failed.' }
}
& $venvPy -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) { Die 'pip upgrade failed.' }
& $venvPy -m pip install -r (Join-Path $comfyDir 'requirements.txt')
if ($LASTEXITCODE -ne 0) { Die 'Installing ComfyUI requirements failed.' }

# ---------- 5. torch (cu118) ----------
Step ("5/8  torch {0} (+cu118 wheel via {1})" -f $TorchVersion, $TorchIndexUrl)
& $venvPy -m pip uninstall -y torch torchvision torchaudio | Out-Null
& $venvPy -m pip install --index-url $TorchIndexUrl ("torch==" + $TorchVersion) ("torchvision==" + $VisionVersion) ("torchaudio==" + $AudioVersion)
if ($LASTEXITCODE -ne 0) {
    Write-Host '   torch cu118 install failed. Things to check:' -ForegroundColor Red
    Write-Host '    - Python must be 64-bit' -ForegroundColor Red
    Write-Host ('    - Check the index for the wheel: {0} (torch {1}%2Bcu118)' -f $TorchIndexUrl, $TorchVersion) -ForegroundColor Red
    Write-Host '    - Fallback that is known to work on gfx803: -TorchVersion 2.2.1 -VisionVersion 0.17.1 -AudioVersion 2.2.1' -ForegroundColor Red
    exit 1
}
$torchLib = (& $venvPy -c "import os, torch; print(os.path.join(os.path.dirname(torch.__file__), 'lib'))").Trim()
Info ("torch lib dir: {0}" -f $torchLib)

# ---------- 6. ZLUDA ----------
Step '6/8  ZLUDA (v3.9.5, rocm6 + nvcuda.dll from rocm5)'
$rocm6Zip   = Join-Path $toolsDir 'ZLUDA-windows-rocm6-amd64.zip'
$rocm5Zip   = Join-Path $toolsDir 'ZLUDA-windows-rocm5-amd64.zip'
$rocm6Dir   = Join-Path $toolsDir 'zluda-rocm6'
$rocm5Dir   = Join-Path $toolsDir 'zluda-rocm5'
$base = ("https://github.com/lshqqytiger/ZLUDA/releases/download/{0}" -f $ZludaTag)
Download-File "$base/ZLUDA-windows-rocm6-amd64.zip" $rocm6Zip
Download-File "$base/ZLUDA-windows-rocm5-amd64.zip" $rocm5Zip
foreach ($pair in @(@($rocm6Zip, $rocm6Dir), @($rocm5Zip, $rocm5Dir))) {
    if (Test-Path -LiteralPath $pair[1]) { Remove-Item -LiteralPath $pair[1] -Recurse -Force }
    Expand-Archive -LiteralPath $pair[0] -DestinationPath $pair[1] -Force
}

# --- layout-agnostic payload assembly ---
# The rocm6/rocm5 zips do NOT necessarily use the classic flat "everything next to
# zluda.exe" layout. So: index every file in both trees, merge the .dll/.exe payload
# (rocm6 preferred, rocm5 as fallback), then HARD-CHECK the required set and dump the
# archives' layout if something is missing. (A previous version of this step assumed a
# flat layout and silently installed an EMPTY zluda folder - do not regress.)
$idx5 = @(Get-ChildItem -LiteralPath $rocm5Dir -Recurse -File -ErrorAction SilentlyContinue)
$idx6 = @(Get-ChildItem -LiteralPath $rocm6Dir -Recurse -File -ErrorAction SilentlyContinue)
if ($idx5.Count -eq 0 -and $idx6.Count -eq 0) { Die 'Both ZLUDA archives expanded to nothing - corrupted download?' }
Info ("extracted: {0} file(s) (rocm6), {1} file(s) (rocm5)" -f $idx6.Count, $idx5.Count)

function Show-Tree([string]$root, $idx) {
    Info ("--- archive layout: {0}" -f $root)
    $idx | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($root.Length).TrimStart('\')
        Info ("    {0,10} B  {1}" -f $_.Length, $rel)
    }
}

$payload = @{}
foreach ($f in $idx5) { if (($f.Extension.ToLower() -in '.dll', '.exe') -and $f.Length -gt 0) { $payload[$f.Name] = $f.FullName } }
foreach ($f in $idx6) { if (($f.Extension.ToLower() -in '.dll', '.exe') -and $f.Length -gt 0) { $payload[$f.Name] = $f.FullName } }
$required = @('zluda.exe', 'nvcuda.dll', 'cublas.dll', 'cusparse.dll', 'nvrtc.dll')
$missing = @($required | Where-Object { -not $payload.ContainsKey($_) })
if ($missing.Count -gt 0) {
    Warn2 ("missing from both ZLUDA packages: {0}" -f ($missing -join ', '))
    Show-Tree $rocm6Dir $idx6
    Show-Tree $rocm5Dir $idx5
    Die 'Unexpected ZLUDA package layout (full listing above). Extracted trees are kept under tools\ for inspection.'
}
if (-not (Test-Path -LiteralPath $zludaDir)) { New-Item -ItemType Directory -Force -Path $zludaDir | Out-Null }
foreach ($k in ($payload.Keys | Sort-Object)) {
    Copy-Item -LiteralPath $payload[$k] -Destination (Join-Path $zludaDir $k) -Force
}
Info ("ZLUDA payload: {0} libraries merged into {1} (rocm6 preferred, rocm5 fallback)" -f $payload.Count, $zludaDir)

# Version-aware default: the rocm5-nvcuda swap is the documented advanced-lvl-up
# recipe for HIP SDK 5.7/6.2.4. For HIP SDK 6.4 there is no confirmed recipe,
# so keep the consistent rocm6 family unless the user forces the swap.
$doSwap = $SwapRocm5Nvcuda
if (-not $SwapRocm5Nvcuda -and -not $KeepRocm6Nvcuda) {
    $doSwap = ($RocmVersion -notmatch '^6\.4')
}
if ($KeepRocm6Nvcuda) {
    Info 'keeping the rocm6 nvcuda.dll (-KeepRocm6Nvcuda)'
} elseif ($doSwap) {
    $nv5 = $idx5 | Where-Object { $_.Name -ieq 'nvcuda.dll' } | Select-Object -First 1
    if ($nv5) {
        $nv6target = Join-Path $zludaDir 'nvcuda.dll'
        if (Test-Path -LiteralPath $nv6target) { Copy-Item -LiteralPath $nv6target -Destination ($nv6target + '.zluda-rocm6.orig') -Force }
        Copy-Item -LiteralPath $nv5.FullName -Destination $nv6target -Force
        Info 'nvcuda.dll swapped for the rocm5 build (advanced-lvl-up recipe for HIP SDK 5.7/6.2.4)'
    } else {
        Warn2 'nvcuda.dll not found in the rocm5 package - keeping the rocm6 one.'
    }
} else {
    Info 'keeping the rocm6 nvcuda.dll (default for HIP SDK 6.4; use -SwapRocm5Nvcuda for the 6.2.4 recipe)'
}

# ---------- 7. patch torch\lib ----------
Step '7/8  Patch torch\lib with ZLUDA DLLs'
$manifest = Join-Path $toolsDir 'patch-manifest.txt'
$manifestLines = New-Object System.Collections.Generic.List[string]
$manifestLines.Add(("RX580 ComfyUI patch manifest - {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')))
$manifestLines.Add("torch lib: $torchLib")
$patchMap = @(
    @{ Zluda = 'cublas.dll';   Pattern = 'cublas64_*.dll';   Required = $true  },
    @{ Zluda = 'cusparse.dll'; Pattern = 'cusparse64_*.dll'; Required = $true  },
    @{ Zluda = 'cublasLt.dll'; Pattern = 'cublasLt64_*.dll'; Required = $false },
    @{ Zluda = 'nvrtc.dll';    Pattern = 'nvrtc64_*.dll';    Required = $false }
)
if (-not $SkipZludaPatch) {
    foreach ($m in $patchMap) {
        $src = Join-Path $zludaDir $m.Zluda
        if (-not (Test-Path -LiteralPath $src)) {
            if ($m.Required) { Die ("ZLUDA DLL missing: {0}" -f $src) }
            Warn2 ("ZLUDA does not ship {0} - skipping" -f $m.Zluda)
            continue
        }
        $targets = Get-ChildItem -LiteralPath $torchLib -Filter $m.Pattern -ErrorAction SilentlyContinue
        if (-not $targets) {
            if ($m.Required) { Warn2 ("torch lib has no {0} - was torch installed from the cu118 index?" -f $m.Pattern) }
            else             { Info ("torch lib has no {0} - skipping" -f $m.Pattern) }
            continue
        }
        foreach ($t in $targets) {
            $bak = $t.FullName + '.orig'
            if (-not (Test-Path -LiteralPath $bak)) { Copy-Item -LiteralPath $t.FullName -Destination $bak -Force }
            Copy-Item -LiteralPath $src -Destination $t.FullName -Force
            $manifestLines.Add(("patched: {0}  <=  {1}  (backup: {2})" -f $t.Name, $m.Zluda, $bak))
            Info ("patched {0}" -f $t.Name)
        }
    }
} else {
    Warn2 'Skipping torch\lib patch (-SkipZludaPatch).'
    $manifestLines.Add('torch lib patch SKIPPED by user (-SkipZludaPatch)')
}
Set-Content -LiteralPath $manifest -Value $manifestLines -Encoding ASCII
Info ("manifest: {0}" -f $manifest)

# ---------- 8. launcher ----------
Step '8/8  Launcher'
$launcher = Join-Path $InstallRoot 'run-comfyui.bat'
$lines = @(
    '@echo off',
    'chcp 65001 >nul',
    ('set "ZLUDA_DIR={0}"' -f $zludaDir),
    ('set "COMFY_DIR={0}"' -f $comfyDir),
    'if not defined ZLUDA_COMGR_LOG_LEVEL set ZLUDA_COMGR_LOG_LEVEL=0',
    'rem if you have an AMD iGPU you do NOT want used, uncomment:',
    'rem set HIP_VISIBLE_DEVICES=0',
    'rem === fake-OOM fix for torch 2.3+ on ZLUDA (see docs/fake-oom.md) ===',
    'rem Keep the caching allocator OUT of cudaMallocAsync mode and never VMM:',
    'if not defined PYTORCH_CUDA_ALLOC_CONF set PYTORCH_CUDA_ALLOC_CONF=expandable_segments:False',
    'set CUDA_MODULE_LOADING=LAZY',
    'rem If you STILL get fake OOM, uncomment this (slow but bypasses the allocator entirely):',
    'rem set PYTORCH_NO_CUDA_MEMORY_CACHING=1',
    'rem If OOM is fragmentation-related, try instead:',
    'rem set PYTORCH_CUDA_ALLOC_CONF=expandable_segments:False,max_split_size_mb:512',
    'cd /d "%COMFY_DIR%"',
    '"%ZLUDA_DIR%\zluda.exe" -- "%COMFY_DIR%\venv\Scripts\python.exe" main.py --force-fp16 --disable-cuda-malloc --disable-smart-memory %*'
)
[IO.File]::WriteAllLines($launcher, $lines)
Info ("launcher: {0}" -f $launcher)

Write-Host ""
Write-Host '============================================================' -ForegroundColor Green
Write-Host ' DONE. Next steps:' -ForegroundColor Green
Write-Host '  1) (admin) .\Patch-RocBLAS-gfx803.ps1 -RocmVersion 6.2   # if not done yet' -ForegroundColor Green
Write-Host '  2) .\Verify-Setup.ps1                                     # torch sees the GPU?' -ForegroundColor Green
Write-Host ('  3) "{0}"' -f $launcher) -ForegroundColor Green
Write-Host '     First run compiles ZLUDA kernels: 10-30 min, be patient.' -ForegroundColor Green
Write-Host '  3) If torch 2.3+ crashes with fake OOM, read docs/fake-oom.md (flags already baked into the launcher).' -ForegroundColor Green
Write-Host '  4) Checkpoints go to: ' (Join-Path $comfyDir 'models\checkpoints') -ForegroundColor Green
Write-Host '============================================================' -ForegroundColor Green
exit 0
