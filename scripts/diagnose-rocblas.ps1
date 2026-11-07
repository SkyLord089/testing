<#
.SYNOPSIS
    Диагностика rocBLAS перед этапом 5: что стоит, какие ядра, можно ли
    обойти CUBLAS_STATUS_INTERNAL_ERROR при cublasCreate.

.DESCRIPTION
    Скрипт ничего не меняет. Он только читает и печатает.

    Проверяет:
      1. Где лежат amdhip64.dll и rocblas.dll и какие у них версии.
      2. Есть ли в rocblas\library ядра gfx803 и fallback .dat.
      3. Отвечает ли hipMalloc на 32 МБ (та самая аллокация из конструктора
         _rocblas_handle, которая падает при cublasCreate).
      4. Работает ли rocBLAS с ROCBLAS_DEVICE_MEMORY_SIZE=0 - это отключает
         ту самую аллокацию в конструкторе.

.PARAMETER HipVersion
    6.4.2 (по умолчанию) или 6.2.4.

.PARAMETER Venv
    Путь к venv ComfyUI. По умолчанию <кит>\ComfyUI-Zluda\venv.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\diagnose-rocblas.ps1
#>
[CmdletBinding()]
param(
    [string]$HipVersion = '6.4.2',
    [string]$Venv = ''
)

$ErrorActionPreference = 'Stop'

$kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Venv) { $Venv = Join-Path $kitRoot 'ComfyUI-Zluda\venv' }
$venvPy = Join-Path $Venv 'Scripts\python.exe'

$hipDirs = @{
    '6.4.2' = 'C:\Program Files\AMD\ROCm\6.4'
    '6.4'   = 'C:\Program Files\AMD\ROCm\6.4'
    '6.2.4' = 'C:\Program Files\AMD\ROCm\6.2'
    '6.2'   = 'C:\Program Files\AMD\ROCm\6.2'
}
if (-not $hipDirs.ContainsKey($HipVersion)) { throw 'HipVersion: 6.4.2 или 6.2.4' }

$hipDir     = $hipDirs[$HipVersion]
$binDir     = Join-Path $hipDir 'bin'
$libDir     = Join-Path $hipDir 'lib'
$libraryDir = Join-Path $binDir 'rocblas\library'
$sys32      = Join-Path $env:WINDIR 'System32'

function Header([string]$t) {
    Write-Host ''
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
}
function Ok([string]$t)   { Write-Host "[OK]   $t" -ForegroundColor Green }
function Warn2([string]$t){ Write-Host "[WARN] $t" -ForegroundColor Yellow }
function Fail2([string]$t){ Write-Host "[FAIL] $t" -ForegroundColor Red }
function Info2([string]$t){ Write-Host "       $t" -ForegroundColor DarkGray }

function Get-FileVer([string]$p) {
    if (-not (Test-Path $p)) { return $null }
    $i = (Get-Item $p)
    return [pscustomobject]@{
        Path = $p
        Ver  = $i.VersionInfo.FileVersion
        MB   = [math]::Round($i.Length / 1MB, 2)
    }
}

Header 'ДИАГНОСТИКА rocBLAS / ZLUDA (ничего не меняет)'

# ---------- 1. DLL ----------
Header '1. Какой amdhip64 и какой rocblas'

$hipCandidates = @(
    (Join-Path $sys32 'amdhip64.dll'),
    (Join-Path $binDir 'amdhip64.dll'),
    (Join-Path $binDir 'amdhip64_6.dll'),
    (Join-Path $binDir 'amdhip64_7.dll')
)
foreach ($c in $hipCandidates) {
    $v = Get-FileVer $c
    if ($v) { Write-Host ("  {0,-45} {1,-18} {2,7} MB" -f (Split-Path $c -Leaf), $v.Ver, $v.MB) -ForegroundColor Gray }
}

Write-Host ''
$rocCandidates = @(
    (Join-Path $libDir 'rocblas.dll'),
    (Join-Path $binDir 'rocblas.dll'),
    (Join-Path $env:WINDIR 'System32\rocblas.dll')
)
$rocFound = @()
foreach ($c in $rocCandidates) {
    $v = Get-FileVer $c
    if ($v) {
        Write-Host ("  rocblas.dll: {0,-40} {1,-18} {2,7} MB" -f (Split-Path (Split-Path $c -Parent) -Leaf), $v.Ver, $v.MB) -ForegroundColor Gray
        $rocFound += $v
    }
}
if ($rocFound.Count -eq 0) {
    Fail2 'rocblas.dll не найдена ни в lib, ни в bin SDK'
    Info2 'ZLUDA линкуется с rocblas из %HIP_PATH%\lib - без неё cublasCreate не заработает'
} elseif ($rocFound.Count -gt 1) {
    Warn2 'rocblas.dll найдена в нескольких местах - порядок загрузки решит, какая победит'
    Info2 'В PATH первой должна идти та же папка, откуда берётся amdhip64'
}

# ---------- 2. ядра ----------
Header '2. Ядра Tensile для gfx803'

if (-not (Test-Path $libraryDir)) {
    Fail2 "нет каталога $libraryDir"
} else {
    $gfx803 = @(Get-ChildItem $libraryDir -File -ErrorAction SilentlyContinue | Where-Object Name -match 'gfx803')
    $fb     = @(Get-ChildItem $libraryDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '_fallback\.dat$' -and $_.Name -notmatch 'gfx803|gfx900' })
    $archs  = @(Get-ChildItem $libraryDir -File -ErrorAction SilentlyContinue |
                 ForEach-Object { if ($_.Name -match '(gfx[0-9a-f]{4})') { $matches[1] } } |
                 Sort-Object -Unique)

    if ($gfx803.Count -gt 0) { Ok  ("файлов с gfx803: {0}" -f $gfx803.Count) }
    else                     { Fail2 'файлов с gfx803 НЕТ - нужен этап 5' }

    if ($fb.Count -gt 0)     { Ok  ("общих fallback .dat: {0}" -f $fb.Count) }
    else                     { Fail2 'нет ни одного *_fallback.dat - rocBLAS не найдёт индексы ядер' }

    Write-Host ("       архитектуры в library: {0}" -f (($archs -join ', ') -replace '^$','(нет)')) -ForegroundColor DarkGray

    if ($gfx803.Count -eq 0) {
        Info2 ''
        Info2 'ВАЖНО: конструктор _rocblas_handle архитектуру gfx803 распознаёт.'
        Info2 'То есть cublasCreate падает НЕ из-за отсутствия ядер - ядра нужны'
        Info2 'позже, на первом GEMM. Сначала проверь пункт 4 ниже.'
    }
}

# ---------- 3. hipMalloc 32 МБ ----------
Header '3. Может ли hipMalloc выделить 32 МБ'

Info2 'Конструктор _rocblas_handle при создании хэндла делает hipMalloc(32 МБ).'
Info2 'Если свободной VRAM нет - именно это и падает в cublasCreate.'

if (-not (Test-Path $venvPy)) {
    Warn2 "нет $venvPy - пропускаю"
} else {
    $env:PATH = "$binDir;$libraryDir;$env:PATH"
    $probe = @'
import ctypes, os, sys
hip = ctypes.WinDLL("amdhip64.dll")
rc = hip.hipInit(0)
print("hipInit:", rc)
if rc != 0:
    sys.exit(1)
n = ctypes.c_int()
hip.hipGetDeviceCount(ctypes.byref(n))
print("устройств:", n.value)
if n.value == 0:
    sys.exit(1)
free_b, total_b = ctypes.c_size_t(), ctypes.c_size_t()
hip.hipMemGetInfo(ctypes.byref(free_b), ctypes.byref(total_b))
print("VRAM свободно: %.1f MB из %.1f MB" % (free_b.value/1048576, total_b.value/1048576))
for mb in (32, 16, 8, 4):
    p = ctypes.c_void_p()
    rc = hip.hipMalloc(ctypes.byref(p), ctypes.c_size_t(mb*1048576))
    print("hipMalloc %3d MB -> %d" % (mb, rc))
    if rc == 0:
        hip.hipFree(p)
'@
    $tmpPy = Join-Path $env:TEMP ('hipmalloc-' + [guid]::NewGuid().ToString('N').Substring(0,8) + '.py')
    [IO.File]::WriteAllText($tmpPy, $probe, (New-Object System.Text.UTF8Encoding($false)))
    try {
        & $venvPy $tmpPy
    } catch {
        Warn2 ("проба упала: " + $_.Exception.Message)
    } finally {
        Remove-Item $tmpPy -Force -ErrorAction SilentlyContinue
    }
}

# ---------- 4. ROCBLAS_DEVICE_MEMORY_SIZE=0 ----------
Header '4. rocBLAS с ROCBLAS_DEVICE_MEMORY_SIZE=0'

Info2 'Эта переменная переключает владельца буфера на user_managed,'
Info2 'и конструктор ПРОПУСКАЕТ hipMalloc целиком. Если cublasCreate'
Info2 'перестанет падать - причина была в аллокации, а не в ядрах.'

if (-not (Test-Path $venvPy)) {
    Warn2 "нет $venvPy - пропускаю"
} else {
    $env:PATH = "$binDir;$libraryDir;$env:PATH"
    $env:ROCBLAS_DEVICE_MEMORY_SIZE = '0'
    $env:ZLUDA_CACHE_DIR = Join-Path $kitRoot 'ComfyUI-Zluda\zluda-cache'
    $env:DISABLE_ADDMM_CUDA_LT = '1'
    $env:CUDA_MODULE_LOADING = 'EAGER'

    $roctest = @'
import ctypes, os, sys
roc = ctypes.WinDLL("rocblas.dll")
h = ctypes.c_void_p()
try:
    rc = roc.rocblas_create_handle(ctypes.byref(h))
except OSError as e:
    print("не удалось вызвать rocblas_create_handle:", e)
    sys.exit(1)
print("ROCBLAS_DEVICE_MEMORY_SIZE =", os.environ.get("ROCBLAS_DEVICE_MEMORY_SIZE"))
print("rocblas_create_handle ->", rc, "(0 = успех)")
if rc == 0:
    roc.rocblas_destroy_handle(h)
'@
    $tmpPy2 = Join-Path $env:TEMP ('roccreate-' + [guid]::NewGuid().ToString('N').Substring(0,8) + '.py')
    [IO.File]::WriteAllText($tmpPy2, $roctest, (New-Object System.Text.UTF8Encoding($false)))
    try {
        Write-Host '  --- БЕЗ переменной ---' -ForegroundColor White
        Remove-Item Env:\ROCBLAS_DEVICE_MEMORY_SIZE -ErrorAction SilentlyContinue
        & $venvPy $tmpPy2
        Write-Host ''
        Write-Host '  --- С ROCBLAS_DEVICE_MEMORY_SIZE=0 ---' -ForegroundColor White
        $env:ROCBLAS_DEVICE_MEMORY_SIZE = '0'
        & $venvPy $tmpPy2
    } catch {
        Warn2 ("проба упала: " + $_.Exception.Message)
    } finally {
        Remove-Item $tmpPy2 -Force -ErrorAction SilentlyContinue
        Remove-Item Env:\ROCBLAS_DEVICE_MEMORY_SIZE -ErrorAction SilentlyContinue
    }
}

# ---------- итог ----------
Header 'ИТОГ'
Write-Host '  Если в пункте 4 create_handle вернул 0 при переменной = 0,' -ForegroundColor White
Write-Host '  а без неё падал - причина в аллокации 32 MB, а не в ядрах.' -ForegroundColor White
Info2 'Тогда добавь в comfyui-user.bat перед запуском main.py:'
Info2 '  set "ROCBLAS_DEVICE_MEMORY_SIZE=0"'
Info2 ''
Info2 'Если падает в обоих случаях - нужен этап 5 (ядра gfx803):'
Info2 '  .\scripts\install\05-gfx803-rocblas.ps1 -Archive <архив> -HipVersion 6.4.2 -ListOnly'
Write-Host ''
exit 0
