<#
.SYNOPSIS
    Этап 1 установки: проверка окружения перед установкой ComfyUI-ZLUDA на RX 580.

.DESCRIPTION
    Только читает. Ничего не устанавливает и не меняет.
    PASS/FAIL по каждому пункту + итоговый вердикт.

.PARAMETER InstallDir
    Куда планируется ставить ComfyUI-Zluda (по умолчанию C:\AI\ComfyUI-Zluda).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\01-prereq-check.ps1
#>
[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\AI\ComfyUI-Zluda',
    [int]$MinFreeGB = 25
)

$ErrorActionPreference = 'Continue'
$script:verdict = @()

function P([string]$status, [string]$what, [string]$note = '') {
    $color = switch ($status) { 'PASS' { 'Green' } 'WARN' { 'Yellow' } 'FAIL' { 'Red' } default { 'Cyan' } }
    Write-Host ("[{0,-4}] {1}" -f $status, $what) -ForegroundColor $color
    if ($note) { Write-Host ("        $note") -ForegroundColor DarkGray }
    $script:verdict += [PSCustomObject]@{ Status = $status; Item = $what; Note = $note }
}

function Header([string]$t) {
    Write-Host ''
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
}

Header 'ЭТАП 1: ПРОВЕРКА ОКРУЖЕНИЯ'

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

# ---------- RAM / VRAM ----------
Header 'Память'
$os = Get-CimInstance Win32_OperatingSystem
$ramGB = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1)
$st = 'WARN'; if ($ramGB -ge 16) { $st = 'PASS' }
$note = ''
if ($ramGB -lt 32) { $note = 'Для HIP/ZLUDA компиляции ядер 32 ГБ сильно приятнее; 8 ГБ VRAM + 16 ГБ RAM - рабочий минимум' }
P $st "RAM: $ramGB ГБ" $note

try {
    $vram = (Get-CimInstance Win32_VideoController | Where-Object Name -match 'Radeon|AMD' | Select-Object -First 1).AdapterRAM
    if ($vram) {
        $vramGB = [math]::Round($vram / 1GB, 1)
        if ($vramGB -gt 0) {
            $st = 'WARN'; if ($vramGB -ge 8) { $st = 'PASS' }
            $note = ''
            if ($vramGB -lt 8) { $note = '4 ГБ: сразу готовься к --lowvram/--novram и tiled VAE' }
            P $st "VRAM: $vramGB ГБ" $note
        }
    }
} catch {}

# ---------- диск ----------
Header 'Диск'
try {
    $drive = ($InstallDir -split ':')[0] + ':'
    $d = Get-PSDrive -Name ($drive.TrimEnd(':')) -ErrorAction Stop
    $free = [math]::Round($d.Free / 1GB, 1)
    $st = 'FAIL'; if ($free -ge $MinFreeGB) { $st = 'PASS' }
    $note = ''
    if ($free -lt $MinFreeGB) { $note = 'torch cu118 ~2.7 ГБ + модели + кэш ZLUDA' }
    P $st "Свободно на $drive $free ГБ (нужно ~$MinFreeGB)" $note
} catch { P 'WARN' 'Не удалось определить свободное место' "$_" }

# файл подкачки
try {
    $pf = Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue
    if ($pf) {
        $pfGB = [math]::Round(($pf | Measure-Object AllocatedBaseSize -Sum).Sum / 1024, 1)
        $st = 'WARN'; if ($pfGB -ge 8) { $st = 'PASS' }
        $note = ''
        if ($pfGB -lt 8) { $note = 'При 8 ГБ VRAM поставь 16+ ГБ подкачки (система -> быстродействие -> виртуальная память)' }
        P $st "Файл подкачки: $pfGB ГБ" $note
    } else {
        P 'WARN' 'Файл подкачки не обнаружен' 'Создай 16 ГБ - иначе VAE/модели будут падать с OOM на диске'
    }
} catch {}

# ---------- инструменты ----------
Header 'Инструменты'
function Test-Tool([string]$exe, [string]$hint) {
    $c = Get-Command $exe -ErrorAction SilentlyContinue
    if ($c) {
        $ver = ''
        try { $ver = (& $exe --version 2>&1 | Select-Object -First 1) } catch {}
        P 'PASS' "$exe найден" "$ver"
        return $true
    }
    P 'FAIL' "$exe не найден" $hint
    return $false
}

$hasGit = Test-Tool 'git' 'Поставь Git for Windows: https://git-scm.com/download/win (галочка "Add to PATH")'
$hasPy = $false
$pyCmd = Get-Command python -ErrorAction SilentlyContinue
$pyLauncher = Get-Command py -ErrorAction SilentlyContinue
if ($pyCmd -or $pyLauncher) {
    $hasPy = $true
    try {
        $v = $null
        if ($pyCmd) {
            $v = (& python -c "import sys; print('%d.%d.%d %dbit' % (sys.version_info.major, sys.version_info.minor, sys.version_info.micro, 64 if sys.maxsize > 2**32 else 32))" 2>$null)
        } elseif ($pyLauncher) {
            $v = (& py -3 -c "import sys; print('%d.%d.%d %dbit' % (sys.version_info.major, sys.version_info.minor, sys.version_info.micro, 64 if sys.maxsize > 2**32 else 32))" 2>$null)
        }
        $okPy = ($v -match '^3\.(10|11|12)')
        $st = 'WARN'; if ($okPy) { $st = 'PASS' }
        $note = ''
        if (-not $okPy) { $note = 'Нужен 3.10-3.12 x64. Рекомендую 3.11.9 с python.org (НЕ из Microsoft Store)' }
        P $st "Python: $v" $note
    } catch { P 'WARN' 'Python есть, но не запускается' "$_" }
} else {
    P 'FAIL' 'Python не найден' 'Поставь Python 3.11.9 x64 с python.org, галочка "Add python.exe to PATH"'
}

$has7z = (Get-Command 7z -ErrorAction SilentlyContinue) -or (Test-Path 'C:\Program Files\7-Zip\7z.exe')
if (-not $has7z) {
    # встроенный tar.exe умеет .7z? нет - но умеет zip; для .7z нужен 7-Zip
    P 'WARN' '7-Zip не найден' 'Нужен для .7z архивов (ядра rocBLAS). https://www.7-zip.org/ - либо используй .zip варианты'
} else { P 'PASS' '7-Zip найден' }

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
    P 'WARN' "В PATH уже есть ROCm/ZLUDA: $($pathRocm.Count) записей" ($pathRocm -join ' | ')
} else {
    P 'PASS' 'PATH чист от ROCm/ZLUDA' ''
}

foreach ($v in 'HIP_PATH', 'HIP_PATH_57', 'HIP_PATH_61', 'HIP_PATH_62', 'HIP_PATH_64', 'HIP_PATH_71', 'HIP_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES', 'ROCM_HOME', 'HIP_HOME') {
    $val = [Environment]::GetEnvironmentVariable($v, 'Machine')
    $valU = [Environment]::GetEnvironmentVariable($v, 'User')
    if ($val -or $valU) { P 'WARN' "Переменная $v задана" "Machine='$val' User='$valU'  - старые значения мешают; удали и перезагрузись" }
}
P 'PASS' 'Проверка переменных окружения завершена' ''

# ---------- итог ----------
Header 'ИТОГ'
$fails = @($script:verdict | Where-Object Status -eq 'FAIL')
$warns = @($script:verdict | Where-Object Status -eq 'WARN')
Write-Host ("PASS: {0}   WARN: {1}   FAIL: {2}" -f @($script:verdict | Where-Object Status -eq 'PASS').Count, $warns.Count, $fails.Count)

if ($fails.Count -gt 0) {
    Write-Host ''
    Write-Host 'ЧТО ИСПРАВИТЬ ПЕРЕД УСТАНОВКОЙ:' -ForegroundColor Red
    $fails | ForEach-Object { Write-Host ("  * {0} - {1}" -f $_.Item, $_.Note) -ForegroundColor Red }
} else {
    Write-Host ''
    Write-Host 'Можно переходить к этапу 2 (драйвер):' -ForegroundColor Green
    Write-Host '  powershell -ExecutionPolicy Bypass -File .\scripts\install\02-driver.ps1 -OpenPage' -ForegroundColor Green
}
