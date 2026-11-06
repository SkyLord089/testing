#requires -Version 5.1
<#
.SYNOPSIS
  Reverts the gfx803 rocBLAS patch installed by Patch-RocBLAS-gfx803.ps1.
.PARAMETER RocmVersion
  Which ROCm tree to restore. Default 6.4.
.PARAMETER FullRestore
  Also DELETES extra kernel files that are not present in the backup (robocopy /MIR).
  Default is a non-destructive overlay restore (extra gfx803 kernel blobs remain, harmless).
.EXAMPLE
  .\Restore-RocBLAS.ps1
#>
[CmdletBinding()]
param(
    [string]$RocmVersion = '6.4',
    [switch]$FullRestore
)

$ErrorActionPreference = 'Stop'

function Info($m) { Write-Host ("   " + $m) -ForegroundColor Gray }
function Warn2($m){ Write-Host ("   [WARN] " + $m) -ForegroundColor Yellow }
function Die($m)  { Write-Host ("   [ERROR] " + $m) -ForegroundColor Red; exit 1 }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host 'Relaunching with Administrator rights...' -ForegroundColor Yellow
    $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"{0}"' -f $PSCommandPath),'-RocmVersion',$RocmVersion)
    if ($FullRestore) { $argList += '-FullRestore' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    exit 0
}

Write-Host ''
Write-Host ("== Restoring original rocBLAS (target ROCm {0})" -f $RocmVersion) -ForegroundColor Cyan

$binCandidates = New-Object System.Collections.Generic.List[string]
foreach ($e in 'HIP_PATH','HIP_HOME','ROCM_PATH') {
    $v = [Environment]::GetEnvironmentVariable($e)
    if ($v) { $binCandidates.Add($v); $binCandidates.Add(($v.TrimEnd('\') + '\bin')) }
}
$base = Join-Path $env:ProgramFiles 'AMD\ROCm'
$binCandidates.Add((Join-Path (Join-Path $base $RocmVersion) 'bin'))
if (Test-Path -LiteralPath $base) {
    Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { $binCandidates.Add((Join-Path $_.FullName 'bin')) }
}
$rocmBin = $null
foreach ($c in $binCandidates) {
    if ($c -and (Test-Path -LiteralPath (Join-Path $c 'rocblas'))) { $rocmBin = (Resolve-Path -LiteralPath $c).Path; break }
}
if (-not $rocmBin) { Die "ROCm $RocmVersion not found under $base" }
Info ("ROCm bin: {0}" -f $rocmBin)

$rocblasDir = Join-Path $rocmBin 'rocblas'
$dllBak     = Join-Path $rocmBin 'rocblas.dll.zluda-bak'
$libraryDir = Join-Path $rocblasDir 'library'
$libBak     = $libraryDir + '.zluda-bak'

if (-not (Test-Path -LiteralPath $dllBak) -and -not (Test-Path -LiteralPath $libBak)) {
    Die 'No .zluda-bak backups found - nothing to restore (patch was maybe never applied, or backups were moved).'
}

if (Test-Path -LiteralPath $dllBak) {
    Copy-Item -LiteralPath $dllBak -Destination (Join-Path $rocmBin 'rocblas.dll') -Force
    Info 'rocblas.dll restored from backup'
} else { Warn2 'rocblas.dll backup missing - leaving current rocblas.dll in place' }

if (Test-Path -LiteralPath $libBak) {
    if ($FullRestore) {
        Info 'FULL restore: mirroring backup over rocblas\library (deletes patched extras)...'
        & robocopy.exe $libBak $libraryDir /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    } else {
        Info 'overlay restore of rocblas\library (patched extra blobs stay, harmless)...'
        & robocopy.exe $libBak $libraryDir /E /NFL /NDL /NJH /NJS /NP | Out-Null
    }
    if ($LASTEXITCODE -ge 8) { Die ("robocopy failed with code {0}" -f $LASTEXITCODE) }
    Info 'library restored'
} else { Warn2 'library backup missing - leaving current library folder in place' }

$marker = Join-Path $rocblasDir 'gfx803.zluda-patched'
Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host 'rocBLAS restored to pre-patch state.' -ForegroundColor Green
Write-Host 'The .zluda-bak backups were kept in place; delete them manually if you are happy.' -ForegroundColor DarkGray
exit 0
