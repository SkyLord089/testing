<#
.SYNOPSIS
    Этап 5: установка ядер rocBLAS для gfx803 (Polaris) — главный блокер.

.DESCRIPTION
    Работает с архивами .7z / .zip. Сначала -ListOnly, чтобы увидеть содержимое.
    При установке:
      * делает бэкап C:\Program Files\AMD\ROCm\<ver>\bin\rocblas,
      * копирует все файлы с gfx803 в имени,
      * по флагу -WithRocblasDll кладёт и rocblas.dll из архива,
      * проверяет, что появились ядра,
      * по флагу -Verify запускает GEMM-тест из verify-torch27.py.

.PARAMETER Archive
    Путь к .7z (например rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z) или .zip.

.PARAMETER HipVersion
    6.4.2 | 6.2.4

.PARAMETER ListOnly
    Только показать содержимое архива.

.PARAMETER Clean
    Перед установкой очистить library от всех TensileLibrary*/Kernels* (полная замена, а не поверх).

.PARAMETER WithRocblasDll
    Копировать rocblas.dll из архива (по умолчанию НЕТ — родной DLL остаётся).

.PARAMETER Venv
    Путь к venv для -Verify.

.EXAMPLE
    .\05-gfx803-rocblas.ps1 -HipVersion 6.4.2 -Archive .\downloads\rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z -ListOnly

.EXAMPLE
    .\05-gfx803-rocblas.ps1 -HipVersion 6.4.2 -Archive .\downloads\rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z -Verify
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [string]$HipVersion = '6.4.2',
    [switch]$ListOnly,
    [switch]$Clean,
    [switch]$WithRocblasDll,
    [string]$Venv = '',
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$hipDirs = @{
    '6.4.2' = 'C:\Program Files\AMD\ROCm\6.4'
    '6.4'   = 'C:\Program Files\AMD\ROCm\6.4'
    '6.2.4' = 'C:\Program Files\AMD\ROCm\6.2'
    '6.2'   = 'C:\Program Files\AMD\ROCm\6.2'
}

function Header([string]$t) {
    Write-Host ''
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
}

if (-not $hipDirs.ContainsKey($HipVersion)) { throw "HipVersion: 6.4.2 или 6.2.4" }
$hipDir = $hipDirs[$HipVersion]
$binDir = Join-Path $hipDir 'bin'
$libraryDir = Join-Path $binDir 'rocblas\library'

if (-not (Test-Path $libraryDir)) { throw "Не найден $libraryDir — HIP SDK $HipVersion установлен?" }
if (-not (Test-Path $Archive)) { throw "Нет файла $Archive" }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $ListOnly) { throw 'Для записи в Program Files нужны права администратора.' }

Header "ЭТАП 5: ЯДРА rocBLAS gfx803  →  HIP $HipVersion"

# ---- распаковка во временную папку ----
$tmp = Join-Path $env:TEMP ("rocblas-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
Write-Host "  Распаковка $Archive`n  →  $tmp" -ForegroundColor White

$is7z = $Archive -match '\.7z$'
if ($is7z) {
    $sevenZip = $null
    $cmd7z = Get-Command 7z -ErrorAction SilentlyContinue
    if ($cmd7z) { $sevenZip = $cmd7z.Source }
    if (-not $sevenZip) {
        foreach ($p in 'C:\Program Files\7-Zip\7z.exe', 'C:\Program Files (x86)\7-Zip\7z.exe') {
            if (Test-Path $p) { $sevenZip = $p; break }
        }
    }
    if (-not $sevenZip) {
        throw 'Нужен 7-Zip для .7z. Поставь https://www.7-zip.org/ или перепакуй архив в .zip'
    }
    & "$sevenZip" x "$Archive" "-o$tmp" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z вернул код $LASTEXITCODE" }
} else {
    Expand-Archive -LiteralPath $Archive -DestinationPath $tmp -Force
}

$allFiles = Get-ChildItem $tmp -Recurse -File
Write-Host ("  Файлов в архиве: {0}" -f $allFiles.Count) -ForegroundColor DarkGray

# ---- показать структуру ----
Header 'СОДЕРЖИМОЕ АРХИВА'
$allFiles | Group-Object { Split-Path $_.DirectoryName -Leaf } | Sort-Object Name | ForEach-Object {
    Write-Host ("  [{0}]  {1} файлов" -f $_.Name, $_.Count) -ForegroundColor White
    $_.Group | Select-Object -First 12 | ForEach-Object { Write-Host ("     {0,-55} {1,10:N0}" -f $_.Name, $_.Length) -ForegroundColor DarkGray }
    if ($_.Count -gt 12) { Write-Host ("     ... ещё {0}" -f ($_.Count - 12)) -ForegroundColor DarkGray }
}

$gfxFiles = $allFiles | Where-Object { $_.Name -match 'gfx803' -or $_.FullName -match '\\gfx803\\' }
Write-Host ''
if ($gfxFiles) {
    Write-Host ("  Найдено файлов gfx803: {0}" -f $gfxFiles.Count) -ForegroundColor Green
} else {
    Write-Host '  В архиве НЕТ файлов gfx803 — это не тот архив.' -ForegroundColor Red
    Write-Host '  Нужен: rocm.gfx800-gfx900-for.hip.sdk.*.7z (advanced-lvl-up) или rocBLAS-HIP6.2.4.7z (likelovewant).' -ForegroundColor Red
    if ($ListOnly) {
        Write-Host ''
        Write-Host '  Это не тот архив. Скачать правильный:' -ForegroundColor Yellow
        Write-Host '    https://github.com/advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU/releases' -ForegroundColor Yellow
        Remove-Item $tmp -Recurse -Force
        return
    }
}

if ($ListOnly) {
    Write-Host ''
    Write-Host '  ПОДСКАЗКА' -ForegroundColor Cyan
    Write-Host @"
  Дальше запусти без -ListOnly, чтобы установить:

    .\05-gfx803-rocblas.ps1 -HipVersion $HipVersion -Archive $Archive

  Флаги:
    -Clean          — полностью заменить TensileLibrary*/Kernels* (рекомендую для чистой установки)
    -WithRocblasDll — положить rocblas.dll из архива (только если HIP той же версии, что и архив!)
    -Verify -Venv C:\AI\ComfyUI-Zluda\venv — сразу прогнать GEMM-тест
"@ -ForegroundColor Gray
    Remove-Item $tmp -Recurse -Force
    return
}

# ---- бэкап ----
Header 'БЭКАП И УСТАНОВКА'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$rocDir = Join-Path $binDir 'rocblas'
$bak = "$rocDir.bak-$stamp"
Copy-Item $rocDir $bak -Recurse -Force
Write-Host "  Бэкап: $bak" -ForegroundColor Green

if ($Clean) {
    $old = Get-ChildItem $libraryDir -File | Where-Object { $_.Name -match 'TensileLibrary|Kernels|\.hsaco$|\.co$' }
    Write-Host ("  -Clean: удаляю {0} старых файлов ядер" -f $old.Count) -ForegroundColor Yellow
    $old | Remove-Item -Force
}

# ---- копирование ----
$copied = 0
foreach ($f in $gfxFiles) {
    Copy-Item $f.FullName (Join-Path $libraryDir $f.Name) -Force
    $copied++
}
Write-Host ("  Скопировано файлов gfx803: {0}" -f $copied) -ForegroundColor Green

# прочие полезные файлы (TensileLibrary.dat общего вида) — по желанию
$generic = $allFiles | Where-Object { $_.Name -match '^TensileLibrary(_lazy)?\.(dat|yaml)$' }
if ($generic) {
    Write-Host ("  Общие TensileLibrary: {0} — НЕ копирую (могут конфликтовать с 6.4)" -f $generic.Count) -ForegroundColor DarkGray
}

if ($WithRocblasDll) {
    $dll = $allFiles | Where-Object { $_.Name -eq 'rocblas.dll' } | Select-Object -First 1
    if ($dll) {
        $orig = Join-Path $binDir 'rocblas.dll'
        if (Test-Path $orig) { Copy-Item $orig (Join-Path $binDir "rocblas.dll.bak-$stamp") -Force }
        Copy-Item $dll.FullName $orig -Force
        Write-Host "  rocblas.dll из архива установлен (бэкап рядом)." -ForegroundColor Yellow
        Write-Host "  ВНИМАНИЕ: версия DLL из архива должна соответствовать версии HIP SDK!" -ForegroundColor Red
    }
}

Remove-Item $tmp -Recurse -Force

# ---- проверка ----
Header 'ПРОВЕРКА'
$now = Get-ChildItem $libraryDir -File | Where-Object Name -match 'gfx803'
$color = 'Red'
if ($now.Count -gt 0) { $color = 'Green' }
Write-Host ("  В {0} файлов с gfx803: {1}" -f $libraryDir, $now.Count) -ForegroundColor $color
$now | Select-Object -First 20 | ForEach-Object { Write-Host ("     {0,-55} {1,12:N0}" -f $_.Name, $_.Length) -ForegroundColor DarkGray }

$archs = Get-ChildItem $libraryDir -File | ForEach-Object { if ($_.Name -match '(gfx[0-9a-f]{4})') { $matches[1] } } | Sort-Object -Unique
Write-Host ("  Архитектуры в library: {0}" -f ($archs -join ', ')) -ForegroundColor DarkGray

Write-Host ''
Write-Host 'ВАЖНО: перезагрузись, чтобы rocBLAS подхватил новые ядра (DLL держится процессом).' -ForegroundColor Yellow

if ($Verify -and $Venv) {
    Header 'GEMM-ТЕСТ'
    $py = Join-Path $Venv 'Scripts\python.exe'
    $verify = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'scripts\verify-torch27.py'
    if ((Test-Path $py) -and (Test-Path $verify)) {
        & $py $verify --sections env,gemm
    } else {
        Write-Host "  Не найден python ($py) или verify-скрипт ($verify)" -ForegroundColor Yellow
    }
} else {
    Write-Host ''
    Write-Host 'Проверить GEMM вручную:' -ForegroundColor Green
    Write-Host '  C:\AI\ComfyUI-Zluda\venv\Scripts\python.exe scripts\verify-torch27.py --sections env,gemm' -ForegroundColor Green
}

Write-Host ''
Write-Host 'Если ошибка "Cannot read TensileLibrary ... gfx803" осталась:' -ForegroundColor Yellow
Write-Host '  → ядра не подошли к версии rocBLAS. Варианты: -Clean, другой архив,' -ForegroundColor Gray
Write-Host '    или fallback на HIP 6.2.4 (там ядра родные). См. docs\05-gfx803-rocblas.md' -ForegroundColor Gray
