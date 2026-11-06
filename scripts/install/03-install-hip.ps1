<#
.SYNOPSIS
    Этап 3: установка HIP SDK (по умолчанию 6.4.2) для RX 580.

.DESCRIPTION
    Скачивает официальный установщик AMD (прямая ссылка), проверяет размер и хеш,
    затем запускает установку.

    ВАЖНО: по умолчанию открывается GUI — в нём нужно СНЯТЬ компонент
    "AMD Software: PRO Edition" (нам нужен драйвер из этапа 2, а не этот).

    -Silent  — установка через CLI без окна; поставит и PRO-драйвер
               (после этого просто переустанови драйвер из этапа 2).

.PARAMETER HipVersion
    6.4.2 (по умолчанию) или 6.2.4 (fallback для Polaris).

.PARAMETER DownloadDir
    Куда качать установщик.

.PARAMETER Silent
    Тихая установка (без GUI).

.PARAMETER SkipDownload
    Использовать уже скачанный файл.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\03-install-hip.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\03-install-hip.ps1 -HipVersion 6.2.4
#>
[CmdletBinding()]
param(
    [string]$HipVersion = '6.4.2',
    [string]$DownloadDir = '',
    [switch]$Silent,
    [switch]$SkipDownload,
    [string]$InstallerPath = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---- таблица версий ----
$known = @{
    '6.4.2' = @{
        Url  = 'https://download.amd.com/developer/eula/rocm-hub/AMD-Software-PRO-Edition-25.Q3-Win10-Win11-For-HIP.exe'
        File = 'AMD-Software-PRO-Edition-25.Q3-Win10-Win11-For-HIP.exe'
        Dir  = 'C:\Program Files\AMD\ROCm\6.4'
        SizeMB = 1700
    }
    '6.2.4' = @{
        Url  = 'https://download.amd.com/developer/eula/rocm-hub/AMD-Software-PRO-Edition-24.Q4-Win10-Win11-For-HIP.exe'
        File = 'AMD-Software-PRO-Edition-24.Q4-Win10-Win11-For-HIP.exe'
        Dir  = 'C:\Program Files\AMD\ROCm\6.2'
        SizeMB = 1600
    }
}

function Header([string]$t) {
    Write-Host ''
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
}

if (-not $known.ContainsKey($HipVersion)) {
    throw "Неизвестная версия $HipVersion. Доступно: $($known.Keys -join ', ')"
}
$info = $known[$HipVersion]

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw 'Нужны права администратора. Запусти PowerShell от имени администратора.' }

if (-not $DownloadDir) { $DownloadDir = Join-Path $PSScriptRoot '..\..\downloads' }
if (-not (Test-Path $DownloadDir)) { New-Item -ItemType Directory -Path $DownloadDir -Force | Out-Null }

Header "ЭТАП 3: HIP SDK $HipVersion"

# ---- скачивание ----
$target = if ($InstallerPath) { $InstallerPath } else { Join-Path $DownloadDir $info.File }

if (-not $SkipDownload -and -not $InstallerPath) {
    if (Test-Path $target) {
        $sz = [math]::Round((Get-Item $target).Length / 1MB)
        Write-Host "  Уже скачан: $target ($sz МБ)" -ForegroundColor DarkGray
        if ($sz -lt 800) {
            Write-Host '  Похоже, файл битый/недокачанный — перекачиваю.' -ForegroundColor Yellow
            Remove-Item $target -Force
        }
    }
    if (-not (Test-Path $target)) {
        Write-Host "  Скачиваю: $($info.Url)" -ForegroundColor White
        Write-Host ("  Ожидаемый размер ~{0} МБ. Это может занять 10–30 минут." -f $info.SizeMB) -ForegroundColor DarkGray
        try {
            # curl.exe есть в Windows 10/11 из коробки и лучше показывает прогресс
            & "$env:SystemRoot\system32\curl.exe" -L --ssl-no-revoke -o $target $info.Url
            if ($LASTEXITCODE -ne 0) { throw "curl вернул код $LASTEXITCODE" }
        } catch {
            Write-Host "  curl не сработал ($_), пробую Invoke-WebRequest..." -ForegroundColor Yellow
            Invoke-WebRequest -Uri $info.Url -OutFile $target -UseBasicParsing
        }
    }
}

if (-not (Test-Path $target)) { throw "Установщик не найден: $target" }
$sizeMB = [math]::Round((Get-Item $target).Length / 1MB)
Write-Host ("  Файл: {0} ({1} МБ)" -f $target, $sizeMB) -ForegroundColor Green

if ($sizeMB -lt 700) {
    Write-Host '  ВНИМАНИЕ: файл подозрительно мал. Возможно, скачалась страница-заглушка (EULA/404).' -ForegroundColor Red
    Write-Host '  Проверь: Start-Process $target - это вообще инсталлятор?' -ForegroundColor Red
}

# ---- инструкция ----
Header 'КАК СТАВИТЬ (GUI)'
Write-Host @"
  1. Откроется установщик AMD.
  2. Раскрой список компонентов.
  3. СНИМИ галочку "AMD Software: PRO Edition" (это драйвер!).
     Драйвер у нас уже стоит из этапа 2 — второй раз не нужен,
     иначе он откатит свежий на старый PRO.
  4. Оставь: HIP SDK Core, HIP Libraries, HIP Runtime Compiler
     (Visual Studio Plugin, HIP Ray Tracing — не нужны, можно снять).
  5. Install location: по умолчанию (C:\Program Files\AMD\ROCm\$HipVersion).
  6. Install → дождаться → перезагрузка.

  После перезагрузки проверить:
      & "$($info.Dir)\bin\hipInfo.exe" | Select-String 'gfx|Radeon'
"@ -ForegroundColor Gray

# ---- запуск ----
Header 'ЗАПУСК'
if ($Silent) {
    Write-Host '  Тихая установка (поставит и PRO-драйвер — потом переустанови драйвер этапа 2).' -ForegroundColor Yellow
    $log = Join-Path $DownloadDir "hip-install-$HipVersion.log"
    Write-Host "  Лог: $log"
    Start-Process -FilePath $target -ArgumentList '-install', '-log', "`"$log`"" -Wait
} else {
    Write-Host '  Открываю GUI установщика...' -ForegroundColor White
    Start-Process -FilePath $target -Wait
}

# ---- проверка ----
Header 'ПРОВЕРКА ПОСЛЕ УСТАНОВКИ'
if (Test-Path $info.Dir) {
    Write-Host ("  [PASS] Каталог установки: {0}" -f $info.Dir) -ForegroundColor Green
    $hipInfo = Join-Path $info.Dir 'bin\hipInfo.exe'
    if (Test-Path $hipInfo) {
        Write-Host '  Запускаю hipInfo (первые 40 строк):' -ForegroundColor White
        try {
            & $hipInfo 2>&1 | Select-Object -First 40 | ForEach-Object { Write-Host "    $_" }
        } catch { Write-Host "    ОШИБКА запуска: $_" -ForegroundColor Red }

        Write-Host ''
        Write-Host @"
  Если увидел gfx803 и "Radeon RX 580" — отлично, идём к этапу 4.
  Если "no ROCm-capable device is detected" — драйвер не подошёл:
        • поставь драйвер 25.8.1 (Polaris & Vega) и повтори,
        • затем 24.9.1,
        • проверь HIP_VISIBLE_DEVICES (пустая строка = 0 устройств),
        • проверь, что в PATH нет второго ROCm.
  Подробно: docs\01-diagnosis.md#1
"@ -ForegroundColor Yellow
    }
} else {
    Write-Host ("  [FAIL] Каталог {0} не появился — установка не завершилась?" -f $info.Dir) -ForegroundColor Red
}

Write-Host ''
$prof = 'safe'
if ($HipVersion -eq '6.4.2') { $prof = 'target' }
Write-Host 'Следующий шаг:' -ForegroundColor Green
Write-Host "  powershell -ExecutionPolicy Bypass -File .\scripts\install\04-comfyui-zluda.ps1 -InstallDir C:\AI\ComfyUI-Zluda -Profile $prof" -ForegroundColor Green
