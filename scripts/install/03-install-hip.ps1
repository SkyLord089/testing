<#
.SYNOPSIS
    Этап 3: установка HIP SDK (по умолчанию 6.4.2) для RX 580.

.DESCRIPTION
    Скачивает официальный установщик AMD (прямая ссылка), проверяет размер и хеш,
    затем запускает установку.

    ВАЖНО: по умолчанию открывается GUI - в нём нужно СНЯТЬ компонент
    "AMD Software: PRO Edition" (нам нужен драйвер из этапа 2, а не этот).

    -Silent  - установка через CLI без окна; поставит и PRO-драйвер
               (после этого просто переустанови драйвер из этапа 2).

.PARAMETER HipVersion
    6.4.2 (по умолчанию) или 6.2.4 (fallback для Polaris).

.PARAMETER DownloadDir
    Куда качать установщик (по умолчанию <кит>\downloads, то есть на диск кита, а не C:).

.NOTES
    Установщик HIP ВСЕГДА ставит SDK в C:\Program Files\AMD\ROCm - выбора нет.
    Но он распаковывает пакеты в C:\AMD (около 3 ГБ) на время установки и убирает
    их потом. Плюс сам .exe (1.7 ГБ) мы качаем на диск кита.
    Итого на C: нужно ~4 ГБ свободных, а не 9.
    В GUI установщика поле "Install location" меняет путь только для
    компонента драйвера, не для HIP SDK.

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
        # 25.Q3 = ROCm 6.4.2. Перебираем варианты имени: в 2026 AMD
        # перешла на схему "<квартал>-Win11-For-HIP.exe" без Win10.
        Urls = @(
            'https://download.amd.com/developer/eula/rocm-hub/AMD-Software-PRO-Edition-25.Q3-Win10-Win11-For-HIP.exe',
            'https://download.amd.com/developer/eula/rocm-hub/AMD-Software-PRO-Edition-25.Q3-Win11-For-HIP.exe'
        )
        File   = 'AMD-Software-PRO-Edition-25.Q3-Win10-Win11-For-HIP.exe'
        Dir    = 'C:\Program Files\AMD\ROCm\6.4'
        SizeMB = 1700
        Page   = 'https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html'
    }
    '6.2.4' = @{
        # 24.Q4 = ROCm 6.2.4. Это последняя версия, для которой есть
        # ГОТОВЫЕ ядра gfx803 - отсюда профиль safe.
        Urls = @(
            'https://download.amd.com/developer/eula/rocm-hub/AMD-Software-PRO-Edition-24.Q4-Win10-Win11-For-HIP.exe',
            'https://download.amd.com/developer/eula/rocm-hub/AMD-Software-PRO-Edition-24.Q4-Win11-For-HIP.exe'
        )
        File   = 'AMD-Software-PRO-Edition-24.Q4-Win10-Win11-For-HIP.exe'
        Dir    = 'C:\Program Files\AMD\ROCm\6.2'
        SizeMB = 1600
        Page   = 'https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html'
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

Write-Host ("  Установщик качаем в: {0}" -f $DownloadDir) -ForegroundColor DarkGray
Write-Host '  Сам SDK встанет в C:\Program Files\AMD\ROCm (выбора нет),' -ForegroundColor DarkGray
Write-Host '  временно распакуется в C:\AMD (~3 ГБ) и уберётся после установки.' -ForegroundColor DarkGray

Header "ЭТАП 3: HIP SDK $HipVersion"

# ---- скачивание ----
$target = if ($InstallerPath) { $InstallerPath } else { Join-Path $DownloadDir $info.File }

if (-not $SkipDownload -and -not $InstallerPath) {
    if (Test-Path $target) {
        $sz = [math]::Round((Get-Item $target).Length / 1MB)
        Write-Host "  Уже скачан: $target ($sz МБ)" -ForegroundColor DarkGray
        if ($sz -lt 800) {
            Write-Host '  Похоже, файл битый/недокачанный - перекачиваю.' -ForegroundColor Yellow
            Remove-Item $target -Force
        }
    }
    if (-not (Test-Path $target)) {
        Write-Host ("  Ожидаемый размер ~{0} МБ. Это может занять 10-30 минут." -f $info.SizeMB) -ForegroundColor DarkGray
        $ok = $false
        foreach ($u in $info.Urls) {
            Write-Host ("  Скачиваю: {0}" -f $u) -ForegroundColor White
            # -f: 404 должен быть ошибкой, а не пустым файлом
            & "$env:SystemRoot\system32\curl.exe" -L --ssl-no-revoke -f -o $target $u
            $rc = $LASTEXITCODE
            if ($rc -eq 0 -and (Test-Path $target)) {
                $mb = [math]::Round((Get-Item $target).Length / 1MB)
                if ($mb -ge 800) { $ok = $true; break }
                Write-Host ("  Файл всего {0} МБ - похоже на страницу-заглушку, пробую дальше" -f $mb) -ForegroundColor Yellow
                Remove-Item $target -Force -ErrorAction SilentlyContinue
            } else {
                Write-Host ("  Не вышло (curl код {0}), пробую следующий вариант" -f $rc) -ForegroundColor DarkYellow
                Remove-Item $target -Force -ErrorAction SilentlyContinue
            }
        }
        if (-not $ok) {
            # последняя попытка - Invoke-WebRequest (другой TLS-стек)
            try {
                Write-Host '  Последняя попытка через Invoke-WebRequest...' -ForegroundColor Yellow
                Invoke-WebRequest -Uri $info.Urls[0] -OutFile $target -UseBasicParsing
                if ((Test-Path $target) -and ((Get-Item $target).Length / 1MB) -ge 800) { $ok = $true }
            } catch {
                Write-Host ("  Invoke-WebRequest тоже не смог: {0}" -f $_.Exception.Message) -ForegroundColor Red
            }
        }
        if (-not $ok) {
            Remove-Item $target -Force -ErrorAction SilentlyContinue
            Write-Host ''
            Write-Host '  СКАЧАТЬ НЕ УДАЛОСЬ. Что делать:' -ForegroundColor Red
            Write-Host ("    1. Открой страницу и скачай вручную: {0}" -f $info.Page) -ForegroundColor Yellow
            Write-Host ("    2. Нужен файл: {0}" -f $info.File) -ForegroundColor Yellow
            Write-Host ("    3. Положи его в: {0}" -f $DownloadDir) -ForegroundColor Yellow
            Write-Host ("    4. Запусти снова с -SkipDownload: .\03-install-hip.ps1 -HipVersion $HipVersion -SkipDownload") -ForegroundColor Yellow
            throw 'Установщик HIP не скачан'
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
     Драйвер у нас уже стоит из этапа 2 - второй раз не нужен,
     иначе он откатит свежий на старый PRO.
  4. Оставь: HIP SDK Core, HIP Libraries, HIP Runtime Compiler
     (Visual Studio Plugin, HIP Ray Tracing - не нужны, можно снять).
  5. Install location: по умолчанию (C:\Program Files\AMD\ROCm\$HipVersion).
  6. Install -> дождаться -> перезагрузка.

  После перезагрузки проверить:
      & "$($info.Dir)\bin\hipInfo.exe" | Select-String 'gfx|Radeon'
"@ -ForegroundColor Gray

# ---- запуск ----
Header 'ЗАПУСК'
if ($Silent) {
    Write-Host '  Тихая установка (поставит и PRO-драйвер - потом переустанови драйвер этапа 2).' -ForegroundColor Yellow
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
  Если увидел gfx803 и "Radeon RX 580" - отлично, идём к этапу 4.
  Если "no ROCm-capable device is detected" - драйвер не подошёл:
        * поставь драйвер 25.8.1 (Polaris & Vega) и повтори,
        * затем 24.9.1,
        * проверь HIP_VISIBLE_DEVICES (пустая строка = 0 устройств),
        * проверь, что в PATH нет второго ROCm.
  Подробно: docs\01-diagnosis.md#1
"@ -ForegroundColor Yellow
    }
} else {
    Write-Host ("  [FAIL] Каталог {0} не появился - установка не завершилась?" -f $info.Dir) -ForegroundColor Red
}

Write-Host ''
$prof = 'safe'
if ($HipVersion -eq '6.4.2') { $prof = 'target' }
Write-Host 'Следующий шаг:' -ForegroundColor Green
Write-Host "  powershell -ExecutionPolicy Bypass -File .\scripts\install\04-comfyui-zluda.ps1 -InstallDir C:\AI\ComfyUI-Zluda -Profile $prof" -ForegroundColor Green
