<#
.SYNOPSIS
    Этап 1 установки: проверка окружения перед установкой ComfyUI-ZLUDA на RX 580.

.DESCRIPTION
    Только читает. Ничего не устанавливает и не меняет.
    PASS/FAIL по каждому пункту + итоговый вердикт.

    Проверяет три диска отдельно, потому что требования разные:
      * диск ComfyUI  - torch ~3 ГБ + модели + кэш ZLUDA
      * диск C:       - HIP SDK ~4 ГБ + временная распаковка установщика ~3 ГБ
      * диск загрузок - архив ядер и установщик HIP

    VRAM читается из реестра (HardwareInformation.qwMemorySize), а не из WMI:
    Win32_VideoController.AdapterRAM - 32-битное поле и врёт при VRAM > 4 ГБ.

.PARAMETER InstallDir
    Куда планируется ставить ComfyUI-Zluda.
    По умолчанию - подпапка ComfyUI-Zluda рядом с этим репозиторием
    (то есть на том же диске, где лежит кит).

.PARAMETER MinFreeGB
    Сколько нужно свободного на диске ComfyUI.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\01-prereq-check.ps1
#>
[CmdletBinding()]
param(
    [string]$InstallDir = '',
    [int]$MinFreeGB = 35
)

$ErrorActionPreference = 'Continue'

# Безопасный запуск внешней программы с захватом stdout и без падения на stderr.
# Причина: в PowerShell 5.1 перенаправление stderr нативной команды
# (cmd 2>$null) при $ErrorActionPreference = 'Stop' превращается в
# NativeCommandError и обрывает скрипт, хотя программа просто что-то вывела
# в stderr. Именно на этом падал этап 4: "import torch" в пустом venv пишет
# traceback в stderr, и проверка "torch уже стоит" убивала установку.
function Invoke-Capture([string]$exe, [string[]]$argv) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $exe @argv 2>&1
        $txt = ($out | ForEach-Object { "$_" }) -join "`n"
        return @{ Code = $LASTEXITCODE; Text = $txt }
    } finally {
        $ErrorActionPreference = $old
    }
}

$script:verdict = @()

# диск, где лежит сам репозиторий с китом
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $InstallDir) { $InstallDir = Join-Path $repoRoot 'ComfyUI-Zluda' }

function P([string]$status, [string]$what, [string]$note = '') {
    $color = switch ($status) { 'PASS' { 'Green' } 'WARN' { 'Yellow' } 'FAIL' { 'Red' } default { 'Cyan' } }
    Write-Host ("[{0,-4}] {1}" -f $status, $what) -ForegroundColor $color
    if ($note) { Write-Host ("        $note") -ForegroundColor DarkGray }
    $script:verdict += [PSCustomObject]@{ Status = $status; Item = $what; Note = $note }
}

function Header([string]$t) {
    Write-Host ''
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
}

function Get-DriveOf([string]$path) {
    # D:\a\b -> D:
    if ($path -match '^([A-Za-z]):') { return ($matches[1].ToUpper() + ':') }
    return $null
}

function Report-Disk([string]$drive, [int]$needGB, [string]$why) {
    if (-not $drive) { P 'WARN' 'Не удалось определить диск' $why; return }
    try {
        $d = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$drive'" -ErrorAction Stop
        if (-not $d) { P 'WARN' "Диск $drive не найден" $why; return }
        $freeGB = [math]::Round($d.FreeSpace / 1GB, 1)
        $totalGB = [math]::Round($d.Size / 1GB, 1)
        $st = 'FAIL'; if ($freeGB -ge $needGB) { $st = 'PASS' }
        P $st ("{0} свободно {1} из {2} ГБ (нужно ~{3})" -f $drive, $freeGB, $totalGB, $needGB) $why
    } catch {
        P 'WARN' "Не удалось прочитать диск $drive" "$_"
    }
}

# ---------------------------------------------------------------------------
# Реальная VRAM: WMI AdapterRAM обрезан до 4 ГБ, берём QWORD из реестра
# ---------------------------------------------------------------------------
function Get-RealVramGB {
    $results = @()
    $classKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
    if (Test-Path $classKey) {
        $subs = Get-ChildItem $classKey -ErrorAction SilentlyContinue |
                Where-Object { $_.PSChildName -match '^\d{4}$' }
        foreach ($s in $subs) {
            try {
                $props = Get-ItemProperty $s.PSPath -ErrorAction Stop
                $name = $props.DriverDesc
                if (-not $name) { continue }
                if ($name -notmatch 'Radeon|AMD') { continue }
                $qw = $props.'HardwareInformation.qwMemorySize'
                if ($qw) {
                    $gb = [math]::Round([double]$qw / 1GB, 1)
                    $results += [PSCustomObject]@{ Name = $name; VRAM_GB = $gb; Source = 'реестр' }
                }
            } catch {}
        }
    }
    return $results
}

Header 'ЭТАП 1: ПРОВЕРКА ОКРУЖЕНИЯ'
Write-Host ("  Репозиторий: {0}" -f $repoRoot) -ForegroundColor DarkGray
Write-Host ("  ComfyUI будет в: {0}" -f $InstallDir) -ForegroundColor DarkGray

# ---------- ОС ----------
Header 'Операционная система'
$win = [Environment]::OSVersion.Version
$osOk = ($win.Major -eq 10 -and $win.Build -ge 22000)
$st = 'WARN'; if ($osOk) { $st = 'PASS' }
$note = 'Ожидается Windows 11; на Windows 10 тоже заработает, но тесты будут не в тех условиях'
if ($osOk) { $note = 'Windows 11' }
P $st "Windows $($win.Major).$($win.Minor) build $($win.Build)" $note

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$st = 'WARN'; if ($isAdmin) { $st = 'PASS' }
$note = ''
if (-not $isAdmin) { $note = 'Установка драйвера и HIP SDK потребует прав администратора' }
P $st "Права администратора: $isAdmin" $note

# ---------- GPU ----------
Header 'Видеокарта'
try {
    $gpus = Get-CimInstance Win32_VideoController
    foreach ($g in $gpus) {
        $isAmd = $g.Name -match 'Radeon|AMD'
        $st = 'WARN'; if ($isAmd) { $st = 'PASS' }
        P $st "$($g.Name)" "Драйвер: $($g.DriverVersion)  от $($g.DriverDate)"
    }
    $amd = $gpus | Where-Object { $_.Name -match 'Radeon|AMD' } | Select-Object -First 1
    if ($amd) {
        if ($amd.Name -match 'RX\s*(470|480|570|580|590)') { P 'PASS' 'Polaris (gfx803) определён - это наш случай' }
        elseif ($amd.Name -match 'Vega') { P 'WARN' 'Vega - тоже gfx9xx, инструкция применима с поправкой на arch' }
        elseif ($amd.Name -match 'RX\s*(6|7|9)') { P 'WARN' 'RDNA-карта: ZLUDA этой инструкцией не нужна (есть ROCm/TheRock)' }
    } else {
        P 'FAIL' 'AMD GPU не найдена' 'Без Radeon ZLUDA работать не будет'
    }
} catch { P 'FAIL' 'Не удалось прочитать WMI' "$_" }

# ---------- VRAM ----------
Header 'Память'
$os = Get-CimInstance Win32_OperatingSystem
$ramGB = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1)
$st = 'WARN'; if ($ramGB -ge 16) { $st = 'PASS' }
$note = ''
if ($ramGB -lt 32) { $note = 'Для HIP/ZLUDA компиляции ядер 32 ГБ сильно приятнее; 16 ГБ - рабочий минимум' }
P $st "RAM: $ramGB ГБ" $note

$vramInfo = @(Get-RealVramGB)
if ($vramInfo.Count -gt 0) {
    foreach ($v in $vramInfo) {
        $st = 'WARN'; if ($v.VRAM_GB -ge 8) { $st = 'PASS' }
        $note = ''
        if ($v.VRAM_GB -lt 8) {
            $note = '4 ГБ VRAM: сразу ставь --lowvram в лаунчере, SDXL пойдёт только с тайлингом VAE'
        } else {
            $note = '8 ГБ - можно SD1.5 свободно, SDXL с --reserve-vram'
        }
        P $st ("VRAM: {0} ГБ ({1})" -f $v.VRAM_GB, $v.Source) $note
    }
} else {
    # реестр не отдал - пробуем WMI с оговоркой про обрезку
    $adapterRam = (Get-CimInstance Win32_VideoController | Where-Object Name -match 'Radeon|AMD' | Select-Object -First 1).AdapterRAM
    if ($adapterRam) {
        $gb = [math]::Round($adapterRam / 1GB, 1)
        P 'WARN' ("VRAM: {0} ГБ (WMI AdapterRAM)" -f $gb) 'WMI-поле 32-битное: при VRAM > 4 ГБ оно показывает 4. Точное значение - в Диспетчере задач, вкладка Производительность, или GPU-Z'
    } else {
        P 'WARN' 'VRAM определить не удалось' 'Посмотри в Диспетчере задач: Производительность - GPU - Выделенная память'
    }
}

# ---------- диски ----------
Header 'Диски (требования разные!)'
$installDrive = Get-DriveOf $InstallDir
$repoDrive = Get-DriveOf $repoRoot

Report-Disk $installDrive $MinFreeGB 'ComfyUI + venv с torch (~3 ГБ) + модели + кэш ZLUDA'
Report-Disk 'C:' 9 'HIP SDK (~4 ГБ) + установщик распаковывается в C:\AMD (~3 ГБ), потом убирается'

if ($repoDrive -ne $installDrive) {
    Report-Disk $repoDrive 4 'Здесь лежит кит и будут качаться архивы (установщик HIP 1.7 ГБ + ядра rocBLAS)'
}

Write-Host ''
Write-Host '  Раскладка по дискам (что куда):' -ForegroundColor White
Write-Host ("    {0}  ComfyUI + venv + torch + ZLUDA + модели + кэш" -f $installDrive) -ForegroundColor Gray
Write-Host ("    {0}  downloads: установщик HIP, архивы ядер" -f $repoDrive) -ForegroundColor Gray
Write-Host '    C:   HIP SDK в C:\Program Files\AMD\ROCm (иначе никак), драйвер, python, git' -ForegroundColor Gray

# файл подкачки
try {
    $pf = Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue
    if ($pf) {
        $pfGB = [math]::Round(($pf | Measure-Object AllocatedBaseSize -Sum).Sum / 1024, 1)
        $st = 'WARN'; if ($pfGB -ge 8) { $st = 'PASS' }
        $note = ''
        if ($pfGB -lt 8) { $note = 'При 8 ГБ VRAM поставь 16+ ГБ подкачки (система - быстродействие - виртуальная память)' }
        P $st "Файл подкачки: $pfGB ГБ" $note
    } else {
        P 'WARN' 'Файл подкачки не обнаружен' 'Создай 16 ГБ - иначе модели будут падать с OOM на диске'
    }
} catch {}

# ---------- инструменты ----------
Header 'Инструменты'
function Find-RealPython {
    # 1) WindowsApps-заглушка из Microsoft Store: ничего не выводит и открывает Store
    $stub = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\python.exe'
    $stubPy3 = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\python3.exe'

    $candidates = @()
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $candidates += $cmd.Source }
    $candidates += @(
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python310\python.exe",
        'C:\Python311\python.exe',
        'C:\Python312\python.exe',
        'C:\Python310\python.exe',
        'D:\Python311\python.exe'
    )

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        if (-not (Test-Path $c)) { continue }
        if ($c -like '*\WindowsApps\*') { continue }   # заглушка Store
        try {
            $probe = Invoke-Capture $c @('-c', "import sys; print('%d.%d.%d' % sys.version_info[:3])")
            $v = if ($probe.Code -eq 0) { $probe.Text.Trim() } else { '' }
            if ($probe.Code -eq 0 -and $v -match '^\d+\.\d+\.\d+$') {
                $probeB = Invoke-Capture $c @('-c', "import struct; print(struct.calcsize('P')*8)")
                $bits = if ($probeB.Code -eq 0) { $probeB.Text.Trim() } else { '' }
                return [PSCustomObject]@{ Path = $c; Version = $v; Bits = $bits }
            }
        } catch {}
    }

    # лаунчер py -3
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) {
        try {
            $probe = Invoke-Capture 'py' @('-3', '-c', "import sys; print('%d.%d.%d' % sys.version_info[:3])")
            $v = if ($probe.Code -eq 0) { $probe.Text.Trim() } else { '' }
            $probeP = Invoke-Capture 'py' @('-3', '-c', "import sys; print(sys.executable)")
            $p = if ($probeP.Code -eq 0) { $probeP.Text.Trim() } else { '' }
            if ($v -match '^\d+\.\d+\.\d+$' -and $p -and $p -notlike '*WindowsApps*') {
                $probeB = Invoke-Capture 'py' @('-3', '-c', "import struct; print(struct.calcsize('P')*8)")
                $bits = if ($probeB.Code -eq 0) { $probeB.Text.Trim() } else { '' }
                return [PSCustomObject]@{ Path = $p; Version = $v; Bits = $bits }
            }
        } catch {}
    }
    return $null
}

$hasGit = $false
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($gitCmd) {
    $hasGit = $true
    $gv = ''
    try { $gv = (& git --version 2>&1 | Select-Object -First 1) } catch {}
    P 'PASS' 'git найден' "$gv"
} else {
    P 'FAIL' 'git не найден' 'Поставь Git for Windows: https://git-scm.com/download/win (галочка "Add to PATH")'
}

# была ли заглушка Store в PATH
$stubInPath = $false
$cmdPy = Get-Command python -ErrorAction SilentlyContinue
if ($cmdPy -and $cmdPy.Source -like '*\WindowsApps\*') { $stubInPath = $true }

$pyInfo = Find-RealPython
if ($pyInfo) {
    $okPy = ($pyInfo.Version -match '^3\.(10|11|12)\.') -and ($pyInfo.Bits -eq '64')
    $st = 'WARN'; if ($okPy) { $st = 'PASS' }
    $note = ''
    if (-not $okPy) {
        $note = 'Нужен 3.10-3.12 x64. Рекомендую 3.11.9 с python.org (НЕ из Microsoft Store)'
    } elseif ($pyInfo.Version -notmatch '^3\.11\.') {
        $note = 'Рекомендую 3.11.x - под него проверены wheels'
    }
    $detail = "$($pyInfo.Path)"
    if ($note) { $detail = "$detail  |  $note" }
    P $st ("Python {0} ({1}-bit)" -f $pyInfo.Version, $pyInfo.Bits) $detail
    if ($stubInPath) {
        P 'WARN' 'В PATH первым идёт python из Microsoft Store' 'Это заглушка: она молча открывает Store. Поставь python с python.org и проверь порядок в PATH, либо запускай скрипты -PythonExe с полным путём.'
    }
} else {
    $st = 'FAIL'
    $note = 'Поставь Python 3.11.9 x64 с python.org, галочка "Add python.exe to PATH"'
    if ($stubInPath) { $note = 'В PATH только заглушка Microsoft Store (python.exe в WindowsApps). Это НЕ настоящий Python - поставь 3.11.9 с python.org.' }
    P $st 'Рабочий Python не найден' $note
}

# 7-Zip может стоять в Program Files (обычная установка) или в профиле
# (если 00a ставил без прав администратора)
$szCandidates = @(
    'C:\Program Files\7-Zip\7z.exe',
    'C:\Program Files (x86)\7-Zip\7z.exe',
    "$env:LOCALAPPDATA\Programs\7-Zip\7z.exe",
    "$env:ProgramFiles\7-Zip\7z.exe"
)
$szPath = $null
foreach ($c in $szCandidates) { if (Test-Path $c) { $szPath = $c; break } }
if (-not $szPath) {
    $c7 = Get-Command 7z -ErrorAction SilentlyContinue
    if ($c7) { $szPath = $c7.Source }
}
# на всякий случай - из реестра (там ключ есть только при обычной установке)
if (-not $szPath) {
    try {
        $reg = Get-ItemProperty 'HKLM:\SOFTWARE\7-Zip' -ErrorAction Stop
        if ($reg.Path) {
            $cand = Join-Path $reg.Path '7z.exe'
            if (Test-Path $cand) { $szPath = $cand }
        }
    } catch {}
}

if ($szPath) {
    $szVer = ''
    try { $szVer = ((& $szPath 2>&1 | Select-String -Pattern '7-Zip' | Select-Object -First 1) -replace '\s+',' ').Trim() } catch {}
    P 'PASS' '7-Zip найден' ("{0}  {1}" -f $szPath, $szVer)
    $inPath = ($env:PATH -split ';') -contains (Split-Path $szPath -Parent)
    if (-not $inPath) {
        P 'WARN' '7-Zip не в PATH' 'Это не мешает: скрипты находят 7z.exe по известным путям. Если хочется 7z из консоли - добавь папку в PATH пользователя.'
    }
} else {
    P 'WARN' '7-Zip не найден' 'Нужен для .7z архива с ядрами rocBLAS. Не критично: 05 распакует через py7zr. https://www.7-zip.org/'
}

# ---------- следы старых версий ----------
Header 'Следы предыдущих установок (должно быть пусто)'
$hipRoot = 'C:\Program Files\AMD\ROCm'
if (Test-Path $hipRoot) {
    $vers = Get-ChildItem $hipRoot -Directory | Select-Object -ExpandProperty Name
    P 'WARN' "Найден $hipRoot : $($vers -join ', ')" 'Если это остатки старых установок - удали через "Программы и компоненты" перед установкой'
} else {
    P 'PASS' 'HIP SDK ранее не устанавливался' ''
}

$pathRocm = ($env:PATH -split ';') | Where-Object { $_ -match 'ROCm|HIP|ZLUDA' }
if ($pathRocm) {
    $realHip = Test-Path (Join-Path $hipRoot '6.4\bin')
    if ($realHip) {
        P 'PASS' ("В PATH есть ROCm: {0}" -f ($pathRocm -join ' | ')) 'HIP SDK уже стоит - этап 3 можно пропустить'
    } else {
        P 'WARN' ("В PATH есть ROCm/ZLUDA, но каталога нет: {0}" -f ($pathRocm -join ' | ')) 'Запись осталась от удалённой установки - она безвредна (каталога нет), но убери её, чтобы не путала'
        Write-Host ''
        Write-Host '        Как убрать (без прав админа, из пользовательского PATH):' -ForegroundColor Gray
        Write-Host '          $p = [Environment]::GetEnvironmentVariable("PATH","User")' -ForegroundColor DarkGray
        Write-Host '          ($p -split ";" | ? { $_ -notmatch "ROCm|ZLUDA" }) -join ";" |' -ForegroundColor DarkGray
        Write-Host '            ForEach-Object { [Environment]::SetEnvironmentVariable("PATH", $_, "User") }' -ForegroundColor DarkGray
        Write-Host '          # затем закрыть и открыть PowerShell заново' -ForegroundColor DarkGray
    }
} else {
    P 'PASS' 'PATH чист от ROCm/ZLUDA' ''
}

$badEnv = @()
foreach ($v in 'HIP_PATH', 'HIP_PATH_57', 'HIP_PATH_61', 'HIP_PATH_62', 'HIP_PATH_64', 'HIP_PATH_71', 'HIP_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES', 'ROCM_HOME', 'HIP_HOME') {
    $val = [Environment]::GetEnvironmentVariable($v, 'Machine')
    $valU = [Environment]::GetEnvironmentVariable($v, 'User')
    if ($val -or $valU) { $badEnv += "$v (Machine='$val' User='$valU')" }
}
if ($badEnv.Count -gt 0) {
    P 'WARN' ("Заданы переменные: {0}" -f $badEnv.Count) ($badEnv -join '; ')
} else {
    P 'PASS' 'Лишних HIP/ROCm-переменных нет' ''
}

# ---------- итог ----------
Header 'ИТОГ'
$fails = @($script:verdict | Where-Object Status -eq 'FAIL')
$warns = @($script:verdict | Where-Object Status -eq 'WARN')
Write-Host ("PASS: {0}   WARN: {1}   FAIL: {2}" -f @($script:verdict | Where-Object Status -eq 'PASS').Count, $warns.Count, $fails.Count)

if ($fails.Count -gt 0) {
    Write-Host ''
    Write-Host 'ЧТО ИСПРАВИТЬ ПЕРЕД УСТАНОВКОЙ:' -ForegroundColor Red
    $fails | ForEach-Object { Write-Host ("  * {0} - {1}" -f $_.Item, $_.Note) -ForegroundColor Red }
    Write-Host ''
    Write-Host 'Если не хватает места на диске - освободи его или укажи другой -InstallDir:' -ForegroundColor Yellow
    Write-Host '  -InstallDir "E:\ComfyUI-Zluda"' -ForegroundColor Yellow
} else {
    Write-Host ''
    Write-Host 'Можно переходить к этапу 2 (драйвер):' -ForegroundColor Green
    Write-Host '  powershell -ExecutionPolicy Bypass -File .\scripts\install\02-driver.ps1 -OpenPage' -ForegroundColor Green
    Write-Host ''
    Write-Host 'Дальше этапы 3-6 (мастер сам спросит путь):' -ForegroundColor DarkGray
    Write-Host ('  .\scripts\install\00-all.ps1 -Profile target -InstallDir "{0}"' -f $InstallDir) -ForegroundColor DarkGray
}
