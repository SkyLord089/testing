<#
.SYNOPSIS
    Оркестратор установки: гоняет этапы 1-6 по порядку, с остановками на ручные шаги.

.DESCRIPTION
    Полностью автоматического "одного клика" здесь быть не может:
    установщик AMD HIP - GUI, драйвер - тоже. Но скрипт:
      * проверяет, что можно начинать,
      * прогоняет то, что можно автоматизировать,
      * в нужных местах останавливается и говорит, что нажать.

.PARAMETER Stage
    1..6, или all (по умолчанию) - начнёт с первого незавершённого.

.PARAMETER Profile
    target (HIP 6.4.2) | safe (HIP 6.2.4)

.PARAMETER InstallDir
    Куда ставить ComfyUI-Zluda.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\00-all.ps1 -Profile target
#>
[CmdletBinding()]
param(
    [string]$Stage = 'all',
    [ValidateSet('target', 'safe')][string]$Profile = 'target',
    [string]$InstallDir = '',
    [string]$RocblasArchive = '',
    [switch]$NonInteractive
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

if (-not $InstallDir) {
    $kitRoot = (Resolve-Path (Join-Path $here '..\..')).Path
    $InstallDir = Join-Path $kitRoot 'ComfyUI-Zluda'
}
$hipVer = if ($Profile -eq 'target') { '6.4.2' } else { '6.2.4' }

function Banner([string]$t) {
    Write-Host ''
    Write-Host ('═' * 70) -ForegroundColor Magenta
    Write-Host "  $t" -ForegroundColor Magenta
    Write-Host ('═' * 70) -ForegroundColor Magenta
}
function Pause-Here([string]$what) {
    if ($NonInteractive) { return }
    Write-Host ''
    Write-Host "  >> $what" -ForegroundColor Yellow
    Read-Host '    Нажми Enter, когда готово (или Ctrl+C для выхода)'
}
function Run([string]$script, [hashtable]$argsTbl = @{}) {
    $path = Join-Path $here $script
    if (-not (Test-Path $path)) { throw "Нет скрипта $path" }
    $argList = @('-ExecutionPolicy', 'Bypass', '-File', $path)
    foreach ($k in $argsTbl.Keys) {
        $v = $argsTbl[$k]
        if ($v -is [switch] -or $v -eq $true) { $argList += "-$k" }
        elseif ($v -ne $null -and "$v" -ne '') { $argList += @("-$k", "$v") }
    }
    & powershell @argList
}

Banner "УСТАНОВКА ComfyUI-ZLUDА НА RX 580  (профиль: $Profile, HIP $hipVer)"
Write-Host @"
  Раскладка:
    ComfyUI + torch + ZLUDA : $InstallDir
    Архивы (setup/ядра)     : $((Resolve-Path (Join-Path $here '..\..')).Path)\downloads
    HIP SDK (без выбора)    : C:\Program Files\AMD\ROCm

  Этапы:
    0. Python 3.11.9 + 7-Zip      (авто, тихая установка)
    1. Проверка окружения         (авто)
    2. Драйвер AMD                (ручной, скрипт открывает страницу)
    3. HIP SDK $hipVer          (полуавто: GUI установщика)
    4. ComfyUI + torch 2.7 + ZLUDA (авто)
    5. Ядра rocBLAS gfx803        (авто, нужен архив)
    6. Проверка                   (авто)
"@ -ForegroundColor Gray

if ($Stage -eq 'all') { $Stage = '1' }

switch ([string]$Stage) {
    '0' { Run '00a-prereq-install.ps1' @{} }
    '1' {
        Banner 'ЭТАП 0-1: предпосылки и проверка'
        Run '00a-prereq-install.ps1' @{}
        Pause-Here 'Python и 7-Zip поставлены? Закрой это окно и открой НОВОЕ, чтобы PATH обновился.'
        Run '01-prereq-check.ps1' @{ InstallDir = $InstallDir }
        Pause-Here 'Проверь вывод выше. Если FAIL - исправь, потом продолжаем.'
        if (-not $NonInteractive) {
            $next = Read-Host '  Продолжить к этапу 2? (y/n)'
            if ($next -notmatch '^[yYдД]') { return }
        }
        Run '02-driver.ps1' @{}
        Pause-Here 'Установи драйвер (Minimal Install), перезагрузись, запусти скрипт 02 ещё раз для проверки.'
        Run '02-driver.ps1' @{}
        Pause-Here 'Драйвер стоит и amdhip64_6.dll на месте?'
        Run '03-install-hip.ps1' @{ HipVersion = $hipVer }
        Pause-Here 'HIP SDK установлен, перезагрузись и убедись, что hipInfo видит gfx803.'
        Run '04-comfyui-zluda.ps1' @{ InstallDir = $InstallDir; Profile = $Profile }
        if ($RocblasArchive) {
            Run '05-gfx803-rocblas.ps1' @{ Archive = $RocblasArchive; HipVersion = $hipVer; Clean = $true }
        } else {
            Write-Host '  Архив ядер rocBLAS не указан - этап 5 пропущен.' -ForegroundColor Yellow
            Write-Host '  Запусти позже: .\05-gfx803-rocblas.ps1 -Archive <файл.7z> -HipVersion ' -NoNewline -ForegroundColor Yellow
            Write-Host $hipVer -ForegroundColor Yellow
        }
        Run '06-verify.ps1' @{ InstallDir = $InstallDir }
        Banner 'ГОТОВО. Запускай ComfyUI:'
        Write-Host "  cd $InstallDir" -ForegroundColor Green
        Write-Host '  .\comfyui-user.bat' -ForegroundColor Green
    }
    '2' { Run '02-driver.ps1' @{} }
    '3' { Run '03-install-hip.ps1' @{ HipVersion = $hipVer } }
    '4' { Run '04-comfyui-zluda.ps1' @{ InstallDir = $InstallDir; Profile = $Profile } }
    '5' {
        if (-not $RocblasArchive) { throw 'Укажи -RocblasArchive <путь к .7z>' }
        Run '05-gfx803-rocblas.ps1' @{ Archive = $RocblasArchive; HipVersion = $hipVer; Clean = $true }
    }
    '6' { Run '06-verify.ps1' @{ InstallDir = $InstallDir } }
    default { throw 'Stage: 0..6 или all' }
}
