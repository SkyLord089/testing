<#
.SYNOPSIS
    Этап 2: проверка/установка драйвера AMD для Polaris (RX 580) под HIP SDK.

.DESCRIPTION
    Сам драйвер не скачивает (у AMD нет стабильных прямых ссылок), но:
      * открывает нужную страницу,
      * показывает, что стоит сейчас,
      * проверяет, что после установки появились нужные DLL,
      * подсказывает, какую версию выбрать и куда откатиться.

.PARAMETER OpenPage
    Открыть в браузере страницу драйверов RX 580.

.PARAMETER Version
    Какая версия ожидается (для отчёта): 26.5.2 | 25.8.1 | 24.9.1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\02-driver.ps1 -OpenPage
#>
[CmdletBinding()]
param(
    [switch]$OpenPage,
    [string]$Version = ''
)

$ErrorActionPreference = 'Continue'

function Header([string]$t) {
    Write-Host ''
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
}

$page = 'https://www.amd.com/en/support/downloads/drivers.html/graphics/radeon-600-500-400/radeon-rx-500-series/radeon-rx-580.html'

Header 'ЭТАП 2: ДРАЙВЕР AMD ДЛЯ RX 580 (POLARIS / gfx803)'

Write-Host @"
Что важно понимать:

  * RX 580 живёт на ОТДЕЛЬНОЙ legacy-ветке драйверов (Polaris & Vega).
    Это не "старьё", а специальный пакет: AMD до сих пор выпускает для него
    точечные фиксы (например 26.5.2 в мае 2026).

  * HIP SDK ставит СВОЙ драйвер (PRO Edition) и может откатить ваш.
    Поэтому драйвер ставим ДО HIP SDK, а при установке HIP снимаем
    галочку с компонента AMD Software: PRO Edition.

  * HIP 6.4 перечисляет устройство через DXCore и требует "достаточно свежий"
    драйвер. На практике:
        26.5.2  - актуальный для Polaris, обычно ок
        25.8.1  - проверенно рабочий с HIP 6.x
        24.9.1  - самый совместимый "старый" (много отчётов "с ним работает")
    Если hipInfo не видит карту на 26.5.2 - это первое, что стоит поменять.

  * Ставить: Minimal Install или Driver Only. Adrenalin-центр не нужен.
    PRO-драйверы НЕ нужны.
"@

if ($Version) { Write-Host "Ожидаемая версия по плану: $Version" -ForegroundColor Yellow }

Header 'Что установлено сейчас'
try {
    Get-CimInstance Win32_VideoController | Where-Object Name -match 'Radeon|AMD' | ForEach-Object {
        Write-Host ("  {0}" -f $_.Name) -ForegroundColor White
        Write-Host ("     DriverVersion : {0}" -f $_.DriverVersion)
        Write-Host ("     DriverDate    : {0}" -f $_.DriverDate)
        Write-Host ("     Status        : {0}" -f $_.Status)
        # версия AMD Software из реестра
        $dv = $_.DriverVersion
        if ($dv -match '^(\d+)\.(\d+)\.(\d+)\.(\d+)$') {
            Write-Host ("     (Windows-версия {0}.{1}.{2}.{3} - это НЕ номер Adrenalin)" -f $matches[1], $matches[2], $matches[3], $matches[4]) -ForegroundColor DarkGray
        }
    }
    $amdSw = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
                              'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue |
             Where-Object { $_.DisplayName -match 'AMD Software' } |
             Select-Object -ExpandProperty DisplayName -First 3
    if ($amdSw) { Write-Host "  AMD Software: $($amdSw -join ' | ')" }
} catch { Write-Host "  не удалось прочитать WMI: $_" -ForegroundColor DarkYellow }

Header 'Ключевые DLL (появятся только после установки драйвера)'
foreach ($dll in 'amdhip64_6.dll', 'amd_comgr.dll', 'amdhip64_7.dll') {
    $p = Join-Path $env:SystemRoot "System32\$dll"
    if (Test-Path $p) {
        $vi = (Get-Item $p).VersionInfo
        Write-Host ("  [PASS] {0,-18} {1}" -f $dll, $vi.FileVersion) -ForegroundColor Green
    } else {
        Write-Host ("  [    ] {0,-18} нет" -f $dll) -ForegroundColor DarkGray
    }
}
Write-Host ''
Write-Host '  amdhip64_6.dll в System32 - обязательное условие: HIP SDK берёт runtime оттуда.' -ForegroundColor DarkGray

if ($OpenPage) {
    Header 'Открываю страницу драйверов'
    Write-Host "  $page"
    Start-Process $page
    Write-Host ''
    Write-Host '  Порядок действий в браузере:' -ForegroundColor Yellow
    Write-Host '   1. Скачать AMD Software: Adrenalin Edition (WHQL) для RX 580.'
    Write-Host '   2. Запустить установщик -> Install -> ДОПОЛНИТЕЛЬНО -> Factory Reset (если стоял другой драйвер).'
    Write-Host '   3. Тип установки: Minimal Install.'
    Write-Host '   4. Перезагрузка.'
    Write-Host '   5. Снова запустить этот скрипт и убедиться, что amdhip64_6.dll появился и DriverDate свежий.'
}

Header 'Проверка после установки драйвера'
Write-Host @"
  Запусти ещё раз:
      powershell -ExecutionPolicy Bypass -File .\scripts\install\02-driver.ps1

  Признаки "всё в порядке":
      [PASS] amdhip64_6.dll  <версия>
      DriverDate - сегодняшняя (или дата выбранного пакета)

  Если hipInfo из HIP SDK всё равно говорит "no ROCm-capable device":
      1. попробуй 25.8.1 (Polaris & Vega) вместо 26.5.2
      2. затем 24.9.1
      3. проверь HIP_VISIBLE_DEVICES - пустая строка = ноль устройств
      4. проверь, что нет нескольких HIP SDK в PATH
"@
