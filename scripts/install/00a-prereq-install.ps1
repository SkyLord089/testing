<#
.SYNOPSIS
    Этап 0: скачать и установить Python 3.11.9 и 7-Zip. Без них установка не пойдёт.

.DESCRIPTION
    Ставит ровно то, чего не хватает по результатам 01-prereq-check.ps1:

      * Python 3.11.9 x64 с python.org - тихая установка в профиль пользователя
        (не требует админа), с Add to PATH. Ставится ПЕРЕД заглушкой
        Microsoft Store по приоритету, но заглушку не удаляет (она системная).
      * 7-Zip - на C:, ~1.5 МБ. Нужен только для .7z с ядрами rocBLAS;
        если ставить не хочется, скрипт 05 умеет через python-модуль py7zr.

    Оба установщика качаются на диск кита (D:), а не на C:.

    Идемпотентно: если рабочий Python или 7-Zip уже есть - пропускает.

.PARAMETER SkipPython
.PARAMETER Skip7Zip

.PARAMETER PythonVersion
    По умолчанию 3.11.9 (под него проверены wheels torch).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\00a-prereq-install.ps1

.EXAMPLE
    # без 7-Zip, только Python
    .\scripts\install\00a-prereq-install.ps1 -Skip7Zip
#>
[CmdletBinding()]
param(
    [switch]$SkipPython,
    [switch]$Skip7Zip,
    [string]$PythonVersion = '3.11.9',
    [string]$DownloadDir = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $DownloadDir) { $DownloadDir = Join-Path $repoRoot 'downloads' }
if (-not (Test-Path $DownloadDir)) { New-Item -ItemType Directory -Path $DownloadDir -Force | Out-Null }

function Header([string]$t) {
    Write-Host ''
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
}
function Step([string]$t) { Write-Host "`n>>> $t" -ForegroundColor White }

function Get-File([string[]]$urls, [string]$dest) {
    if (Test-Path $dest) {
        $mb = [math]::Round((Get-Item $dest).Length / 1MB, 1)
        Write-Host ("  Уже скачан: {0} ({1} МБ)" -f $dest, $mb) -ForegroundColor DarkGray
        if ($mb -gt 0.1) { return $true }
        Remove-Item $dest -Force
    }
    foreach ($url in $urls) {
        Write-Host ("  Скачиваю: {0}" -f $url) -ForegroundColor White
        try {
            & "$env:SystemRoot\system32\curl.exe" -L --ssl-no-revoke -f -o $dest $url
            if ($LASTEXITCODE -eq 0 -and (Test-Path $dest)) { break }
        } catch {}
        try {
            # -f у curl не сработал (404) - пробуем Invoke-WebRequest
            if (-not (Test-Path $dest) -or (Get-Item $dest).Length -lt 100KB) {
                Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
                if (Test-Path $dest) { break }
            }
        } catch {
            Write-Host ("  не вышло: {0}" -f $_.Exception.Message) -ForegroundColor DarkYellow
        }
    }
    if (-not (Test-Path $dest)) {
        Write-Host '  Все источники недоступны.' -ForegroundColor Red
        return $false
    }
    $sz = (Get-Item $dest).Length
    if ($sz -lt 100KB) {
        Write-Host ("  Файл подозрительно мал: {0} байт - вероятно, страница-заглушка" -f $sz) -ForegroundColor Red
        return $false
    }
    Write-Host ("  Готово: {0:N0} байт" -f $sz) -ForegroundColor Green
    return $true
}

Header 'ЭТАП 0: ПРЕДПОСЫЛКИ (Python, 7-Zip)'
Write-Host ("  Качаем в: {0}" -f $DownloadDir) -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# Python
# ---------------------------------------------------------------------------
if (-not $SkipPython) {
    Step 'Python'

    # уже есть рабочий?
    $found = $null
    $cands = @()
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source -notlike '*WindowsApps*') { $cands += $cmd.Source }
    $cands += @(
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        'C:\Python311\python.exe', 'C:\Python312\python.exe'
    )
    foreach ($c in $cands) {
        if ($c -and (Test-Path $c) -and $c -notlike '*WindowsApps*') {
            try {
                $v = (& $c -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null | Out-String).Trim()
                if ($v -match '^3\.(10|11|12)$') { $found = [PSCustomObject]@{ Path = $c; Version = $v }; break }
            } catch {}
        }
    }

    if ($found) {
        Write-Host ("  Рабочий Python уже есть: {0} ({1})" -f $found.Path, $found.Version) -ForegroundColor Green
        $ans = 'n'
        if ($Host.Name -eq 'ConsoleHost') {
            $ans = Read-Host '  Переустановить принудительно? (y/N)'
        }
        if ($ans -notmatch '^[yY]') {
            Write-Host '  Пропускаю.' -ForegroundColor DarkGray
            $found = $null
            $SkipPython = $true
        } else {
            $SkipPython = $false
        }
    }

    if (-not $SkipPython) {
        $urls = @(
            "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-amd64.exe",
            "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion.exe"
        )
        $exe = Join-Path $DownloadDir "python-$PythonVersion-amd64.exe"

        if (Get-File $urls $exe) {
            Write-Host ''
            Write-Host '  Установка (тихая, в профиль пользователя, без админа):' -ForegroundColor White
            Write-Host '    InstallAllUsers=0  PrependPath=1  Include_pip=1  Include_test=0' -ForegroundColor DarkGray

            $argv = @(
                '/quiet',
                'InstallAllUsers=0',
                'PrependPath=1',
                'Include_pip=1',
                'Include_launcher=1',
                'InstallLauncherAllUsers=0',
                'Include_test=0',
                'Include_doc=0',
                'Include_tcltk=1',
                'Shortcuts=0'
            )
            Write-Host '  Ждём завершения (1-3 минуты, окна не будет)...' -ForegroundColor DarkGray
            $p = Start-Process -FilePath $exe -ArgumentList $argv -Wait -PassThru
            $rcColor = 'Yellow'
            if ($p.ExitCode -eq 0) { $rcColor = 'Green' }
            Write-Host ("  Код возврата: {0}" -f $p.ExitCode) -ForegroundColor $rcColor

            # проверить, куда встал
            $installed = @(
                "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
                "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
                'C:\Python311\python.exe'
            ) | Where-Object { Test-Path $_ } | Select-Object -First 1

            if ($installed) {
                Write-Host ("  [PASS] Python установлен: {0}" -f $installed) -ForegroundColor Green
                $v = (& $installed -c "import sys; print(sys.version)" 2>&1 | Out-String).Trim()
                Write-Host ("         {0}" -f $v) -ForegroundColor DarkGray
            } else {
                Write-Host '  [FAIL] Не нашёл python.exe после установки.' -ForegroundColor Red
                Write-Host '         Установи вручную: запусти установщик из downloads и поставь' -ForegroundColor Yellow
                Write-Host '         галочку "Add python.exe to PATH".' -ForegroundColor Yellow
            }

            Write-Host ''
            Write-Host '  ВАЖНО: в текущем окне PowerShell PATH ещё старый.' -ForegroundColor Yellow
            Write-Host '  Закрой это окно и открой новое, иначе python не найдётся.' -ForegroundColor Yellow
            Write-Host '  Проверка в новом окне:  python --version' -ForegroundColor Gray
            Write-Host '  А также: Заглушка Microsoft Store останется в PATH, но python.org' -ForegroundColor DarkGray
            Write-Host '  поставит себя выше по приоритету. Если всё равно открывается Store -' -ForegroundColor DarkGray
            Write-Host '  Параметры - Приложения - Дополнительные компоненты - Управление' -ForegroundColor DarkGray
            Write-Host '  псевдонимами выполнения приложений - выключи python.exe и python3.exe.' -ForegroundColor DarkGray
        }
    }
}

# ---------------------------------------------------------------------------
# 7-Zip
# ---------------------------------------------------------------------------
if (-not $Skip7Zip) {
    Step '7-Zip'

    $has7z = $false
    $cmd7z = Get-Command 7z -ErrorAction SilentlyContinue
    if ($cmd7z) { $has7z = $true }
    if (Test-Path 'C:\Program Files\7-Zip\7z.exe') { $has7z = $true }

    if ($has7z) {
        Write-Host '  7-Zip уже есть - пропускаю.' -ForegroundColor Green
    } else {
        # 7-Zip 26.05 (окт 2026). Ссылки переехали на GitHub; старые
        # www.7-zip.org/a/7z2408-x64.exe больше не отдаются.
        $urls = @(
            'https://github.com/ip7z/7zip/releases/download/26.05/7z2605-x64.exe',
            'https://www.7-zip.org/a/7z2605-x64.exe'
        )
        $exe = Join-Path $DownloadDir '7z2605-x64.exe'

        if (Get-File $urls $exe) {
            $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
            $dir = 'C:\Program Files\7-Zip'
            if (-not $isAdmin) { $dir = "$env:LOCALAPPDATA\Programs\7-Zip" }

            Write-Host ("  Тихая установка в: {0}" -f $dir) -ForegroundColor White
            $p = Start-Process -FilePath $exe -ArgumentList @('/S', "/D=$dir") -Wait -PassThru
            Write-Host ("  Код возврата: {0}" -f $p.ExitCode)

            if (Test-Path (Join-Path $dir '7z.exe')) {
                Write-Host ("  [PASS] 7-Zip: {0}" -f (Join-Path $dir '7z.exe')) -ForegroundColor Green
            } elseif (Test-Path 'C:\Program Files\7-Zip\7z.exe') {
                Write-Host '  [PASS] 7-Zip установлен в C:\Program Files\7-Zip' -ForegroundColor Green
            } else {
                Write-Host '  [FAIL] 7z.exe не найден после установки.' -ForegroundColor Red
                Write-Host '         Не критично: скрипт 05 умеет распаковывать .7z через py7zr.' -ForegroundColor Yellow
            }
        } else {
            Write-Host '  7-Zip скачать не удалось. Это не блокер:' -ForegroundColor Yellow
            Write-Host '  скрипт 05 распакует .7z через python-модуль py7zr автоматически.' -ForegroundColor Yellow
        }
    }
}

# ---------------------------------------------------------------------------
# итог
# ---------------------------------------------------------------------------
Header 'ИТОГ'

# что фактически установилось
$pyFinal = @(
    "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
    "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
    'C:\Python311\python.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1

$szFinal = @(
    'C:\Program Files\7-Zip\7z.exe',
    "$env:LOCALAPPDATA\Programs\7-Zip\7z.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($pyFinal) { Write-Host ("  Python: {0}" -f $pyFinal) -ForegroundColor Green }
if ($szFinal) { Write-Host ("  7-Zip:  {0}" -f $szFinal) -ForegroundColor Green }
Write-Host ''

Write-Host '  Что проверить в НОВОМ окне PowerShell:' -ForegroundColor White
Write-Host '    python --version          -> Python 3.11.9' -ForegroundColor Gray
Write-Host '    Get-Command python        -> путь в ...\Programs\Python\Python311\, НЕ WindowsApps' -ForegroundColor Gray
Write-Host '      (не "where python": в PowerShell where - это алиас Where-Object,' -ForegroundColor DarkGray
Write-Host '       он молча ничего не выведет)' -ForegroundColor DarkGray
Write-Host '    7z                        -> если ставили' -ForegroundColor Gray

if ($szFinal -and $szFinal -like '*AppData*') {
    Write-Host ''
    Write-Host '  7-Zip поставлен в профиль пользователя (без прав админа), поэтому' -ForegroundColor DarkGray
    Write-Host '  команда 7z в консоли может не находиться - это НЕ мешает скриптам:' -ForegroundColor DarkGray
    Write-Host '  они ищут его по этому пути напрямую. Хочется 7z из консоли - добавь' -ForegroundColor DarkGray
    Write-Host ("  {0} в PATH пользователя." -f (Split-Path $szFinal -Parent)) -ForegroundColor DarkGray
}
Write-Host ''
Write-Host '  Затем:' -ForegroundColor Green
Write-Host '    powershell -ExecutionPolicy Bypass -File .\scripts\install\01-prereq-check.ps1' -ForegroundColor Green
