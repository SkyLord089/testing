<#
.SYNOPSIS
    Аккуратно подменяет CUDA-DLL в venv\Lib\site-packages\torch\lib на DLL ZLUDA.

.DESCRIPTION
    Делает бэкап оригинальных файлов (.bak-<timestamp>), проверяет разрядность,
    копирует только то, что реально есть в папке ZLUDA, и печатает итоговую таблицу.

    Повторяет проверенный порядок из ComfyUI-Zluda install-n.bat:
    ОРИГИНАЛЬНЫЙ nvrtc64_112_0.dll копируется в nvrtc_cuda.dll, и только
    потом nvrtc64_112_0.dll подменяется на ZLUDA-версию.

    Специально НЕ копирует cublasLt.dll по умолчанию: в ZLUDA 3.9.6 он линкуется
    на hipblaslt.dll, которого для gfx803 нет, из-за чего torch 2.6+ падает с
    CUBLAS_STATUS_NOT_SUPPORTED / WinError 126. Включается флагом -IncludeCublasLt.

.PARAMETER ZludaDir
    Папка с распакованным ZLUDA (где лежат cublas.dll, nvcuda.dll и т.д.)

.PARAMETER Venv
    Путь к venv ComfyUI-Zluda (внутри ожидается Lib\site-packages\torch\lib).

.PARAMETER Restore
    Откатить: взять самый свежий .bak-* и вернуть файлы на место.

.PARAMETER IncludeCublasLt
    Копировать также cublasLt.dll (только с HIP SDK, где есть hipblaslt).

.PARAMETER IncludeCudnn
    Копировать cudnn.dll -> cudnn64_9.dll (нужен ZLUDA nightly + MIOpen).

.EXAMPLE
    .\scripts\patch-torch-zluda.ps1 -ZludaDir .\zluda -Venv .\venv

.EXAMPLE
    .\scripts\patch-torch-zluda.ps1 -Venv .\venv -Restore
#>
[CmdletBinding()]
param(
    [string]$ZludaDir = '',
    [string]$Venv = '',
    [switch]$Restore,
    [switch]$IncludeCublasLt,
    [switch]$IncludeCudnn,
    [switch]$WhatIfOnly
)

$ErrorActionPreference = 'Stop'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

function Resolve-Venv([string]$p) {
    if ($p) { return (Resolve-Path -LiteralPath $p).Path }
    $cands = @((Join-Path $PSScriptRoot '..\venv'), (Join-Path (Get-Location) 'venv'))
    foreach ($c in $cands) {
        if (Test-Path (Join-Path $c 'Scripts\python.exe')) { return (Resolve-Path $c).Path }
    }
    throw 'venv не найден. Укажите -Venv "C:\ComfyUI-Zluda\venv"'
}

$Venv = Resolve-Venv $Venv
$torchLib = Join-Path $Venv 'Lib\site-packages\torch\lib'
if (-not (Test-Path -LiteralPath $torchLib)) {
    throw "Не найден $torchLib — это точно venv с установленным torch?"
}
Write-Host "torch\lib: $torchLib" -ForegroundColor Cyan

# ---------- маппинг: что во что ----------
# имя в ZLUDA            -> имя в torch\lib
$map = [ordered]@{
    'cublas.dll'   = 'cublas64_11.dll'
    'cusparse.dll' = 'cusparse64_11.dll'
    'cufft.dll'    = 'cufft64_10.dll'
    'cufftw.dll'   = 'cufftw64_10.dll'
    'nvrtc.dll'    = 'nvrtc64_112_0.dll'
}
if ($IncludeCublasLt) { $map['cublasLt.dll'] = 'cublasLt.dll' }
if ($IncludeCudnn)    { $map['cudnn.dll']    = 'cudnn64_9.dll' }

# ---------- restore ----------
if ($Restore) {
    Write-Host '=== РЕЖИМ ВОССТАНОВЛЕНИЯ ===' -ForegroundColor Yellow
    $backups = Get-ChildItem -LiteralPath $torchLib -File -Filter '*.bak-*' | Sort-Object LastWriteTime -Descending
    if (-not $backups) { Write-Host 'Бэкапов не найдено — нечего восстанавливать.' -ForegroundColor Yellow; return }
    $groups = $backups | Group-Object { $_.Name -replace '\.bak-.*$', '' }
    foreach ($g in $groups) {
        $latest = $g.Group | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $target = Join-Path $torchLib $g.Name
        Write-Host ("  {0}  <-  {1}" -f $g.Name, $latest.Name)
        if (-not $WhatIfOnly) { Copy-Item $latest.FullName $target -Force }
    }
    Write-Host 'Готово.' -ForegroundColor Green
    return
}

# ---------- zluda dir ----------
if (-not $ZludaDir) {
    $cands = @((Join-Path $PSScriptRoot '..\zluda'), (Join-Path (Get-Location) 'zluda'))
    foreach ($c in $cands) { if (Test-Path (Join-Path $c 'cublas.dll')) { $ZludaDir = (Resolve-Path $c).Path; break } }
}
if (-not $ZludaDir -or -not (Test-Path -LiteralPath $ZludaDir)) {
    throw 'Папка ZLUDA не найдена. Укажите -ZludaDir "C:\ComfyUI-Zluda\zluda" (там должен быть cublas.dll)'
}
Write-Host "ZLUDA:     $ZludaDir" -ForegroundColor Cyan
Write-Host ''

# ---------- sanity: это действительно ZLUDA, а не NVIDIA? ----------
foreach ($src in $map.Keys) {
    $p = Join-Path $ZludaDir $src
    if (-not (Test-Path $p)) { Write-Host ("  [!] нет {0} — пропускаю" -f $src) -ForegroundColor Yellow; continue }
    $len = (Get-Item $p).Length
    # ZLUDA-шим'ы маленькие (единицы МБ), настоящие NVIDIA-библиотеки — десятки/сотни МБ
    if ($len -gt 60MB) {
        Write-Host ("  [!] {0} весит {1:N0} байт — похоже на настоящую NVIDIA-библиотеку, а не на ZLUDA. Прерываю." -f $src, $len) -ForegroundColor Red
        return
    }
}

# ---------- nvrtc_cuda.dll: копия ОРИГИНАЛА, до подмены nvrtc ----------
# Так делает install-n.bat: torch иногда грузит nvrtc_cuda.dll, и там ожидается
# настоящий nvrtc, а не ZLUDA-шим. Поэтому снимаем копию ДО того, как
# nvrtc64_112_0.dll будет заменён.
$nvrtcOrig = Join-Path $torchLib 'nvrtc64_112_0.dll'
$nvrtcCuda = Join-Path $torchLib 'nvrtc_cuda.dll'
$nvrtcNote = 'нет исходного nvrtc64_112_0.dll'
if ((Test-Path $nvrtcOrig) -and -not (Test-Path $nvrtcCuda)) {
    if (-not $WhatIfOnly) { Copy-Item $nvrtcOrig $nvrtcCuda -Force }
    $nvrtcNote = 'копия оригинального nvrtc64_112_0.dll (как в install-n.bat)'
} elseif ((Test-Path $nvrtcCuda) -and (Test-Path $nvrtcOrig)) {
    # уже есть: если размеры совпадают — это остаток прошлой подмены, не годится
    $lenCuda = (Get-Item $nvrtcCuda).Length
    $lenOrig = (Get-Item $nvrtcOrig).Length
    $rest = Get-ChildItem -LiteralPath $torchLib -File -Filter 'nvrtc64_112_0.dll.bak-*' |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($lenCuda -eq $lenOrig -and $lenCuda -lt 5MB) {
        if ($rest) {
            if (-not $WhatIfOnly) { Copy-Item $rest.FullName $nvrtcCuda -Force }
            $nvrtcNote = "была ZLUDA-копия — восстановил оригинал из $($rest.Name)"
        } else {
            $nvrtcNote = 'похоже на ZLUDA-копию, но бэкапа оригинала нет — проверь вручную'
        }
    } else {
        $nvrtcNote = 'уже существует, не трогаю'
    }
}

# ---------- патч ----------
$results = @()
if ($nvrtcNote) { $results += [PSCustomObject]@{ File = 'nvrtc_cuda.dll'; Action = 'PRE'; Note = $nvrtcNote } }
foreach ($src in $map.Keys) {
    $srcPath = Join-Path $ZludaDir $src
    $dstName = $map[$src]
    $dstPath = Join-Path $torchLib $dstName

    if (-not (Test-Path $srcPath)) {
        $results += [PSCustomObject]@{ File = $dstName; Action = 'SKIP'; Note = "нет $src в ZLUDA" }
        continue
    }

    # бэкап текущего файла (только оригинального, не бэкапа бэкапа)
    if ((Test-Path $dstPath) -and -not $WhatIfOnly) {
        $existingBackup = Get-ChildItem -LiteralPath $torchLib -File -Filter "$dstName.bak-*"
        if (-not $existingBackup) {
            Copy-Item $dstPath "$dstPath.bak-$stamp" -Force
            Write-Host ("  [bak] {0} -> {1}.bak-{2}" -f $dstName, $dstName, $stamp)
        } else {
            Write-Host ("  [bak] {0}: бэкап уже есть ({1}), не перезаписываю" -f $dstName, $existingBackup[0].Name)
        }
    }

    if (-not $WhatIfOnly) {
        Copy-Item $srcPath $dstPath -Force
    }
    $results += [PSCustomObject]@{ File = $dstName; Action = 'COPIED'; Note = ("из {0} ({1:N0} байт)" -f $src, (Get-Item $srcPath).Length) }
}

# ---------- nvcuda.dll ----------
$nvcudaSrc = Join-Path $ZludaDir 'nvcuda.dll'
$nvcudaDst = Join-Path $Venv 'Scripts\nvcuda.dll'
if (Test-Path $nvcudaSrc) {
    if (Test-Path $nvcudaDst) { Copy-Item $nvcudaDst "$nvcudaDst.bak-$stamp" -Force -ErrorAction SilentlyContinue }
    if (-not $WhatIfOnly) { Copy-Item $nvcudaSrc $nvcudaDst -Force }
    $results += [PSCustomObject]@{ File = 'Scripts\nvcuda.dll'; Action = 'COPIED'; Note = 'чтобы работал запуск без zluda.exe' }
}

# ---------- итог ----------
Write-Host ''
Write-Host '=== ИТОГ ===' -ForegroundColor Cyan
$results | Format-Table -AutoSize | Out-String -Width 200 | Write-Host

Write-Host 'Дальше:' -ForegroundColor Green
Write-Host '  1) set DISABLE_ADDMM_CUDA_LT=1   (обязательно для torch > 2.4)'
Write-Host '  2) удалить кэш ZLUDA (ZLUDA_CACHE_DIR) перед первым запуском после смены DLL'
Write-Host '  3) запустить scripts\verify-torch27.py'
Write-Host '  4) откат: .\scripts\patch-torch-zluda.ps1 -Venv <venv> -Restore'
