<#
.SYNOPSIS
    Этап 5: установка ядер rocBLAS для gfx803 (Polaris) - главный блокер.

.DESCRIPTION
    Работает с .7z / .zip из community-сборок gfx803.

    ЧТО КОПИРУЕТСЯ (проверено на rocm.gfx800-gfx900-...-5.7.1-and-6.2.4.7z):

      1) файлы с 'gfx803' в имени        - 47 .hsaco + 5 .dat + 4 .co
      2) файлы 'fallback' БЕЗ архитектуры - 46 .dat
         ЭТО КРИТИЧНО: это индексы ядер Tensile. Без них rocBLAS не
         находит ни одного решения, даже когда .hsaco на месте.
         Ошибка выглядит как "Cannot read TensileLibrary" или мусор.
      3) TensileManifest.txt (если есть и если -WithManifest)
      4) rocblas.dll - ТОЛЬКО по флагу -WithRocblasDll (см. предупреждение)

    7-Zip НЕ обязателен: если его нет, архив распакуется через python-модуль
    py7zr (поставится автоматически). Нужен любой рабочий Python.

    ЧЕГО НЕ ДЕЛАЕТ:
      * не копирует .hsaco/.dat для gfx900 (Vega) - они не наши
      * не трогает rocblas64.dll и прочие dll
      * не перезаписывает TensileLibrary_lazy.dat, если он уже есть от 6.4

    ВАЖНО ПРО -WithRocblasDll:
      В архиве лежит rocblas.dll версии 6.2.4 (проверяется по строкам PE).
      Если у вас HIP SDK 6.4, эта DLL старше остального SDK и может
      конфликтовать с amdhip64.dll 6.4. На 6.4 сначала пробуйте БЕЗ него.
      С ним - только если без него rocBLAS падает на загрузке.

.PARAMETER Archive
    Путь к .7z или .zip.

.PARAMETER HipVersion
    6.4.2 | 6.2.4 - в какой SDK ставить.

.PARAMETER ListOnly
    Только показать содержимое и классификацию файлов.

.PARAMETER Clean
    Перед установкой удалить все TensileLibrary*/Kernels*/.hsaco/.co/.dat
    из library - полная замена набора, а не поверх.

.PARAMETER WithRocblasDll
    Копировать rocblas.dll из архива (с бэкапом родного).

.PARAMETER WithManifest
    Копировать TensileManifest.txt (по умолчанию да, если файл есть).

.PARAMETER Venv
    Путь к venv - для -Verify.

.PARAMETER Verify
    После установки прогнать GEMM-тест.

.EXAMPLE
    .\05-gfx803-rocblas.ps1 -Archive .\downloads\rocm.gfx800.7z -HipVersion 6.4.2 -ListOnly

.EXAMPLE
    .\05-gfx803-rocblas.ps1 -Archive .\downloads\rocm.gfx800.7z -HipVersion 6.4.2 -Clean

.EXAMPLE
    .\05-gfx803-rocblas.ps1 -Archive .\downloads\rocm.gfx800.7z -HipVersion 6.2.4 -Clean -WithRocblasDll
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [string]$HipVersion = '6.4.2',
    [switch]$ListOnly,
    [switch]$Clean,
    [switch]$WithRocblasDll,
    [switch]$WithManifest,
    [string]$Venv = '',
    [string]$Python = '',
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
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
}

if (-not $hipDirs.ContainsKey($HipVersion)) { throw 'HipVersion: 6.4.2 или 6.2.4' }
$hipDir = $hipDirs[$HipVersion]
$binDir = Join-Path $hipDir 'bin'
$libraryDir = Join-Path $binDir 'rocblas\library'
$rocDir = Join-Path $binDir 'rocblas'

if (-not (Test-Path $libraryDir)) { throw "Не найден $libraryDir - HIP SDK $HipVersion установлен?" }
if (-not (Test-Path $Archive)) { throw "Нет файла $Archive" }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $ListOnly) { throw 'Для записи в Program Files нужны права администратора.' }

Header "ЭТАП 5: ЯДРА rocBLAS gfx803  ->  HIP $HipVersion"
Write-Host ("  Архив:   {0}" -f (Resolve-Path $Archive).Path) -ForegroundColor DarkGray
Write-Host ("  Цель:    {0}" -f $libraryDir) -ForegroundColor DarkGray

# ---------- распаковка ----------
$tmp = Join-Path $env:TEMP ("rocblas-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
Write-Host ("  Распаковка во временный каталог: {0}" -f $tmp) -ForegroundColor White

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
    if ($sevenZip) {
        & "$sevenZip" x "$Archive" "-o$tmp" -y | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7z вернул код $LASTEXITCODE" }
    } else {
        # 7-Zip нет - распаковываем через python-модуль py7zr.
        # Ищем python: аргумент -Python, потом venv ComfyUI, потом PATH.
        Write-Host '  7-Zip не найден - распаковываю через py7zr (python)' -ForegroundColor Yellow

        $pyForExtract = $Python
        if (-not $pyForExtract) {
            $venvGuess = @()
            if ($Venv) { $venvGuess += (Join-Path $Venv 'Scripts\python.exe') }
            $venvGuess += Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'ComfyUI-Zluda\venv\Scripts\python.exe'
            $venvGuess += 'C:\AI\ComfyUI-Zluda\venv\Scripts\python.exe'
            foreach ($g in $venvGuess) { if ($g -and (Test-Path $g)) { $pyForExtract = $g; break } }
        }
        if (-not $pyForExtract) {
            $c = Get-Command python -ErrorAction SilentlyContinue
            if ($c -and $c.Source -notlike '*WindowsApps*') { $pyForExtract = $c.Source }
        }
        if (-not $pyForExtract) {
            throw 'Нет ни 7-Zip, ни рабочего Python. Запусти 00a-prereq-install.ps1 (ставит Python), либо поставь 7-Zip.'
        }

        Write-Host ("  python: {0}" -f $pyForExtract) -ForegroundColor DarkGray
        & $pyForExtract -m pip install --quiet --disable-pip-version-check py7zr
        if ($LASTEXITCODE -ne 0) {
            throw 'Не удалось поставить py7zr (нет сети?). Альтернатива: поставь 7-Zip с https://www.7-zip.org/'
        }

        # py7zr распаковывает в текущий каталог - переходим в $tmp
        Push-Location $tmp
        try {
            & $pyForExtract -c "import py7zr,sys; py7zr.SevenZipFile(sys.argv[1],'r').extractall(path='.')" "$Archive"
            if ($LASTEXITCODE -ne 0) { throw "py7zr вернул код $LASTEXITCODE" }
        } finally { Pop-Location }
    }
} else {
    Expand-Archive -LiteralPath $Archive -DestinationPath $tmp -Force
}

$allFiles = @(Get-ChildItem $tmp -Recurse -File)
Write-Host ("  Файлов в архиве: {0}" -f $allFiles.Count) -ForegroundColor DarkGray

# ---------- классификация ----------
# 1) наши архитектуры
$gfx803 = @($allFiles | Where-Object { $_.Name -match 'gfx803' })
# 2) общие индексы Tensile (fallback) - БЕЗ архитектуры в имени, но нужны
$genericFallback = @($allFiles | Where-Object { $_.Name -match '_fallback\.dat$' -and $_.Name -notmatch 'gfx803|gfx900' })
# 3) чужие архитектуры - не берём
$otherArch = @($allFiles | Where-Object { $_.Name -match 'gfx(?!803)\d{3}' })
# 4) DLL
$dlls = @($allFiles | Where-Object { $_.Extension -eq '.dll' })
# 5) манифест
$manifest = @($allFiles | Where-Object { $_.Name -eq 'TensileManifest.txt' })

$toCopy = @($gfx803 + $genericFallback)

# ---------- показать ----------
Header 'СОДЕРЖИМОЕ АРХИВА'

Write-Host ("  Всего файлов:                {0}" -f $allFiles.Count) -ForegroundColor White
Write-Host ("  [1] gfx803 (наши):           {0}   <- копируем" -f $gfx803.Count) -ForegroundColor Green
Write-Host ("  [2] fallback .dat (общие):   {0}   <- копируем ОБЯЗАТЕЛЬНО" -f $genericFallback.Count) -ForegroundColor Green
Write-Host ("  [3] другие архитектуры:      {0}   <- пропускаем" -f $otherArch.Count) -ForegroundColor DarkGray
Write-Host ("  [4] DLL в архиве:            {0}" -f $dlls.Count) -ForegroundColor Yellow
Write-Host ("  [5] TensileManifest.txt:     {0}" -f $manifest.Count) -ForegroundColor DarkGray

Write-Host ''
Write-Host '  Наши файлы (первые 12):' -ForegroundColor White
$gfx803 | Select-Object -First 12 | ForEach-Object { Write-Host ("     {0,-72} {1,10:N0}" -f $_.Name, $_.Length) -ForegroundColor DarkGray }
if ($gfx803.Count -gt 12) { Write-Host ("     ... ещё {0}" -f ($gfx803.Count - 12)) -ForegroundColor DarkGray }

Write-Host ''
Write-Host '  Общие индексы Tensile (первые 6 из нужных):' -ForegroundColor White
$genericFallback | Select-Object -First 6 | ForEach-Object { Write-Host ("     {0,-72} {1,10:N0}" -f $_.Name, $_.Length) -ForegroundColor DarkGray }
if ($genericFallback.Count -gt 6) { Write-Host ("     ... ещё {0}" -f ($genericFallback.Count - 6)) -ForegroundColor DarkGray }

if ($dlls.Count -gt 0) {
    Write-Host ''
    Write-Host '  DLL в архиве:' -ForegroundColor Yellow
    foreach ($d in $dlls) {
        Write-Host ("     {0,-72} {1,10:N0}" -f $d.Name, $d.Length) -ForegroundColor Yellow
    }
    Write-Host '     -> по умолчанию НЕ копируется: версия может быть старше вашего SDK.' -ForegroundColor DarkGray
    Write-Host '     -> -WithRocblasDll, если без неё rocBLAS не грузится.' -ForegroundColor DarkGray
}

$gfxDats = @($gfx803 | Where-Object { $_.Extension -eq '.dat' -or $_.Extension -eq '.co' })
if ($gfxDats.Count -eq 0) {
    Write-Host ''
    Write-Host '  ВНИМАНИЕ: в архиве нет .dat/.co для gfx803 - такой набор не заработает.' -ForegroundColor Red
    if ($ListOnly) { Remove-Item $tmp -Recurse -Force; return }
}

if ($ListOnly) {
    Header 'ПОДСКАЗКА'
    Write-Host @"
  Установка (с бэкапом, полная замена набора):

    .\05-gfx803-rocblas.ps1 -HipVersion $HipVersion -Archive "$Archive" -Clean

  Если после этого rocBLAS не грузится (ошибка загрузки DLL, а не ядер):

    .\05-gfx803-rocblas.ps1 -HipVersion $HipVersion -Archive "$Archive" -Clean -WithRocblasDll

  Проверка GEMM:

    .\05-gfx803-rocblas.ps1 -HipVersion $HipVersion -Archive "$Archive" -Verify -Venv <путь к venv>

  Помни: rocblas.dll в архиве - версии 6.2.4. На HIP 6.4 сначала пробуй без неё.
"@ -ForegroundColor Gray
    Remove-Item $tmp -Recurse -Force
    return
}

# ---------- бэкап ----------
Header 'БЭКАП И УСТАНОВКА'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bak = "$rocDir.bak-$stamp"
Copy-Item $rocDir $bak -Recurse -Force
$bakSizeMB = [math]::Round(((Get-ChildItem $bak -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host ("  Бэкап: {0}  ({1} МБ)" -f $bak, $bakSizeMB) -ForegroundColor Green
Write-Host '  Откат: Rename-Item (новый rocblas) -NewName rocblas.broken; Rename-Item (бэкап) -NewName rocblas' -ForegroundColor DarkGray

if ($Clean) {
    $old = @(Get-ChildItem $libraryDir -File | Where-Object { $_.Name -match 'TensileLibrary|Kernels|^rocblas-bench' -or $_.Extension -in '.dat', '.hsaco', '.co' })
    Write-Host ("  -Clean: удаляю {0} файлов старых ядер" -f $old.Count) -ForegroundColor Yellow
    $old | Remove-Item -Force
}

# ---------- копирование ----------
$copied = 0
$skippedLazy = 0
foreach ($f in $toCopy) {
    # TensileLibrary_lazy.dat - общий для всех архитектур; не перезаписываем чужой от 6.4
    if ($f.Name -eq 'TensileLibrary_lazy.dat') {
        $existing = Join-Path $libraryDir $f.Name
        if (Test-Path $existing) {
            $skippedLazy++
            Write-Host ("  [skip] {0} - уже есть ({1:N0} байт), не перезаписываю" -f $f.Name, (Get-Item $existing).Length) -ForegroundColor DarkYellow
            continue
        }
    }
    Copy-Item $f.FullName (Join-Path $libraryDir $f.Name) -Force
    $copied++
}
Write-Host ("  Скопировано: {0} файлов" -f $copied) -ForegroundColor Green
if ($skippedLazy -gt 0) { Write-Host ("  Пропущено (уже было): {0}" -f $skippedLazy) -ForegroundColor DarkGray }

if ($WithManifest -and $manifest.Count -gt 0) {
    $m = $manifest[0]
    Copy-Item $m.FullName (Join-Path $libraryDir 'TensileManifest.txt') -Force
    Write-Host '  TensileManifest.txt скопирован' -ForegroundColor Green
}

if ($WithRocblasDll) {
    $dll = $dlls | Where-Object { $_.Name -eq 'rocblas.dll' } | Select-Object -First 1
    if ($dll) {
        $orig = Join-Path $binDir 'rocblas.dll'
        if (Test-Path $orig) { Copy-Item $orig (Join-Path $binDir "rocblas.dll.bak-$stamp") -Force }
        Copy-Item $dll.FullName $orig -Force
        Write-Host ("  rocblas.dll из архива установлен ({0:N0} байт, бэкап рядом)" -f $dll.Length) -ForegroundColor Yellow
        Write-Host '  ВНИМАНИЕ: это DLL от 6.2.4. Проверь, что rocBLAS грузится.' -ForegroundColor Red
    } else {
        Write-Host '  rocblas.dll в архиве не найдена - пропускаю' -ForegroundColor Yellow
    }
}

Remove-Item $tmp -Recurse -Force

# ---------- проверка ----------
Header 'ПРОВЕРКА'
$nowGfx = @(Get-ChildItem $libraryDir -File | Where-Object Name -match 'gfx803')
$nowFallback = @(Get-ChildItem $libraryDir -File | Where-Object { $_.Name -match '_fallback\.dat$' -and $_.Name -notmatch 'gfx803|gfx900' })

$color = 'Red'
if ($nowGfx.Count -gt 0) { $color = 'Green' }
Write-Host ("  Файлов с gfx803 в library:            {0}" -f $nowGfx.Count) -ForegroundColor $color

$color2 = 'Red'
if ($nowFallback.Count -gt 0) { $color2 = 'Green' }
Write-Host ("  Общих fallback .dat (индексы ядер):  {0}" -f $nowFallback.Count) -ForegroundColor $color2

if ($nowGfx.Count -eq 0) {
    Write-Host ''
    Write-Host '  ПРОБЛЕМА: ядра gfx803 не появились. Скопируй вручную из архива:' -ForegroundColor Red
    Write-Host ('    x файлов в {0}' -f $libraryDir) -ForegroundColor Red
}
if ($nowFallback.Count -eq 0) {
    Write-Host ''
    Write-Host '  ПРОБЛЕМА: нет ни одного *_fallback.dat' -ForegroundColor Red
    Write-Host '  Без них rocBLAS не найдёт ядра, даже если .hsaco на месте.' -ForegroundColor Red
    Write-Host '  Возьми другой архив (в правильном их 46).' -ForegroundColor Red
}

$archs = @(Get-ChildItem $libraryDir -File | ForEach-Object { if ($_.Name -match '(gfx[0-9a-f]{4})') { $matches[1] } } | Sort-Object -Unique)
Write-Host ("  Архитектуры в library: {0}" -f ($archs -join ', ')) -ForegroundColor DarkGray

Write-Host ''
Write-Host '  ВАЖНО: закрой все процессы, которые держат rocBLAS (ComfyUI, python),' -ForegroundColor Yellow
Write-Host '  иначе DLL останется старой в памяти. Новый набор подхватится после перезапуска.' -ForegroundColor Yellow

# ---------- GEMM ----------
if ($Verify) {
    Header 'GEMM-ТЕСТ'
    if (-not $Venv) {
        $guess = @()
        $guess += Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'ComfyUI-Zluda\venv'
        $guess += 'C:\AI\ComfyUI-Zluda\venv'
        foreach ($g in $guess) { if (Test-Path (Join-Path $g 'Scripts\python.exe')) { $Venv = $g; break } }
    }
    if (-not $Venv) {
        Write-Host '  Не нашёл venv. Укажи -Venv "D:\Apps\rx-580-comfyui\ComfyUI-Zluda\venv"' -ForegroundColor Yellow
    } else {
        $py = Join-Path $Venv 'Scripts\python.exe'
        $verifyScript = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'scripts\verify-torch27.py'
        Write-Host ("  venv: {0}" -f $Venv) -ForegroundColor DarkGray
        if ((Test-Path $py) -and (Test-Path $verifyScript)) {
            # HIP в PATH
            $env:PATH = "$binDir;$libraryDir;$env:PATH"
            & $py $verifyScript --sections env,gemm
        } else {
            Write-Host '  Нет python в venv или нет verify-torch27.py' -ForegroundColor Yellow
        }
    }
} else {
    Write-Host ''
    Write-Host '  Проверить GEMM вручную:' -ForegroundColor Green
    Write-Host '    .\scripts\install\06-verify.ps1 -InstallDir <путь к ComfyUI-Zluda>' -ForegroundColor Green
}

Write-Host ''
Write-Host '  Если ошибка "Cannot read TensileLibrary" осталась:' -ForegroundColor Yellow
Write-Host '    1) проверь, что fallback .dat скопированы (см. проверку выше)' -ForegroundColor Gray
Write-Host '    2) -Clean и повтори' -ForegroundColor Gray
Write-Host '    3) другой архив' -ForegroundColor Gray
Write-Host '    4) fallback на HIP 6.2.4: .\90-fallback-hip62.ps1 (там ядра родные)' -ForegroundColor Gray
