#requires -Version 5.1
<#
.SYNOPSIS
  Verifies the RX 580 ComfyUI install: torch under ZLUDA, rocBLAS patch marker, launcher.
.PARAMETER InstallRoot
  Default: %USERPROFILE%\RX580-ComfyUI
.PARAMETER SmokeTest
  Additionally runs a tiny CUDA matmul through ZLUDA.
  NOTE: on first ever run this triggers kernel compilation for GEMM and can take MINUTES.
.EXAMPLE
  .\Verify-Setup.ps1
#>
[CmdletBinding()]
param(
    [string]$InstallRoot = (Join-Path $env:USERPROFILE 'RX580-ComfyUI'),
    [switch]$SmokeTest
)

$ErrorActionPreference = 'Continue'
$script:Fails = 0
function Ok($m)  { Write-Host ("  [OK]   " + $m) -ForegroundColor Green }
function Warn2($m){ Write-Host ("  [WARN] " + $m) -ForegroundColor Yellow }
function Fail($m){ Write-Host ("  [FAIL] " + $m) -ForegroundColor Red; $script:Fails++ }
function Info($m){ Write-Host ("         " + $m) -ForegroundColor DarkGray }

Write-Host 'RX 580 ComfyUI setup verification' -ForegroundColor White

$comfyDir = Join-Path $InstallRoot 'ComfyUI'
$venvPy   = Join-Path $comfyDir 'venv\Scripts\python.exe'
$zludaExe = Join-Path $InstallRoot 'zluda\zluda.exe'
$launcher = Join-Path $InstallRoot 'run-comfyui.bat'

Write-Host ""; Write-Host '== Files' -ForegroundColor Cyan
if (Test-Path -LiteralPath $venvPy)   { Ok "venv python: $venvPy" } else { Fail "venv python not found at $venvPy - run .\Install-ComfyUI-RX580.ps1" }
if (Test-Path -LiteralPath $zludaExe) { Ok "ZLUDA: $zludaExe" }        else { Fail "zluda.exe not found at $zludaExe - run .\Install-ComfyUI-RX580.ps1" }
if (Test-Path -LiteralPath (Join-Path $comfyDir 'main.py')) { Ok 'ComfyUI main.py present' } else { Fail "ComfyUI not found at $comfyDir" }
if (Test-Path -LiteralPath $launcher) { Ok "launcher: $launcher" }     else { Warn2 "run-comfyui.bat missing at $launcher" }

Write-Host ""; Write-Host '== rocBLAS gfx803 patch' -ForegroundColor Cyan
$markerFound = $false
$base = Join-Path $env:ProgramFiles 'AMD\ROCm'
if (Test-Path -LiteralPath $base) {
    foreach ($d in (Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue)) {
        $m = Join-Path $d.FullName 'bin\rocblas\gfx803.zluda-patched'
        if (Test-Path -LiteralPath $m) { Ok ("patch marker: {0}" -f $m); $markerFound = $true }
    }
}
if (-not $markerFound) { Warn2 'No gfx803.zluda-patched marker under Program Files\AMD\ROCm\*\bin\rocblas - did you run .\Patch-RocBLAS-gfx803.ps1 (elevated)?' }

Write-Host ""; Write-Host '== torch under ZLUDA' -ForegroundColor Cyan
if ((Test-Path -LiteralPath $venvPy) -and (Test-Path -LiteralPath $zludaExe)) {
    $check = @'
import torch
print("torch_version=" + torch.__version__)
print("cuda_build=" + str(torch.version.cuda))
print("cuda_available=" + str(torch.cuda.is_available()))
if torch.cuda.is_available():
    print("device_name=" + torch.cuda.get_device_name(0))
    print("device_capability=" + ".".join(str(x) for x in torch.cuda.get_device_capability(0)))
'@
    $checkFile = Join-Path $env:TEMP ('verify-torch-' + [IO.Path]::GetRandomFileName() + '.py')
    Set-Content -LiteralPath $checkFile -Value $check -Encoding ASCII
    try {
        $out = & $zludaExe -- $venvPy $checkFile 2>&1 | Out-String
        $out -split "`r?`n" | ForEach-Object { Info $_ }
        if ($out -match 'cuda_available=True') { Ok 'torch.cuda.is_available() == True (ZLUDA path works)' }
        else { Fail 'torch.cuda.is_available() != True - see docs/troubleshooting.md (rocBLAS patch / PATH / nvcuda.dll)' }
        if ($out -match 'torch_version=([0-9.]+[^\r\n]*)') { Info ("torch: {0}" -f $Matches[1]) }
    } catch {
        Fail ("torch check crashed: {0}" -f $_.Exception.Message)
    } finally {
        Remove-Item -LiteralPath $checkFile -Force -ErrorAction SilentlyContinue
    }
} else {
    Fail 'Skipping torch check - install is incomplete.'
}

if ($SmokeTest) {
    Write-Host ""; Write-Host '== Smoke test (tiny matmul on GPU; may take minutes on first run)' -ForegroundColor Cyan
    $smoke = @'
import torch
a = torch.randn(512, 512, device="cuda")
b = a @ a
torch.cuda.synchronize()
print("matmul_ok shape=" + str(tuple(b.shape)) + " mean=" + str(float(b.mean())))
'@
    $smokeFile = Join-Path $env:TEMP ('smoke-torch-' + [IO.Path]::GetRandomFileName() + '.py')
    Set-Content -LiteralPath $smokeFile -Value $smoke -Encoding ASCII
    try {
        $out = & $zludaExe -- $venvPy $smokeFile 2>&1 | Out-String
        $out -split "`r?`n" | ForEach-Object { Info $_ }
        if ($out -match 'matmul_ok') { Ok 'CUDA matmul through ZLUDA succeeded' }
        else { Fail 'matmul did not report success - check docs/troubleshooting.md' }
    } finally {
        Remove-Item -LiteralPath $smokeFile -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
if ($script:Fails -gt 0) {
    Write-Host ("RESULT: {0} check(s) FAILED - see docs/troubleshooting.md" -f $script:Fails) -ForegroundColor Red
    exit 1
}
Write-Host 'RESULT: all checks passed. Launch with run-comfyui.bat - first run takes 10-30 minutes (kernel compilation).' -ForegroundColor Green
exit 0
