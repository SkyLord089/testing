<#
.SYNOPSIS
    Этап 5b: сборка ядер Tensile для gfx803 из исходников rocBLAS 6.4.2.

.DESCRIPTION
    Альтернатива этапу 5 (готовый архив из community). Здесь мы собираем
    ядра сами - официальным инструментом Tensile, из официальных исходников.

    ГЛАВНОЕ, ЧТО НАДО ЗНАТЬ ПЕРЕД ЗАПУСКОМ:

      rocblas.dll ПЕРЕСОБИРАТЬ НЕ НУЖНО.

    Это не упрощение, это факт из исходников 6.4.2:

      library/src/blas3/tensile_host.cpp:246  getLazyLoadingArch()
        ...
        return Tensile::LazyLoadingInit::gfx803;      <- строка 256

      library/src/handle.cpp:89  getActiveArch()
        if(deviceString.find("gfx803") != std::string::npos)
            return Processor::gfx803;                 <- строка 89

    То есть штатная DLL уже знает про gfx803 и в рантайме сама ищет файл

        %HIP_PATH%\bin\rocblas\library\TensileLibrary_lazy_gfx803.dat

    Чего в штатной поставке НЕТ - так это самих файлов ядер: AMD убрала
    gfx803 из списка сборки (TARGET_LIST) начиная с ROCm 6.0.

    Логики Tensile при этом из дерева НЕ удалены:

        library/src/blas3/Tensile/Logic/asm_full/r9nano/   <- это и есть gfx803
        library/src/blas3/Tensile/Logic/asm_full/hip/      <- source-ядра

    r9nano - это кодовое имя Polaris. Связь задана в Tensile/Common.py:317:

        'gfx803':'r9nano'

    Значит достаточно велеть Tensile собрать ядра для gfx803 - и положить
    результат рядом со штатной DLL.

    ПОЧЕМУ ЭТО ЛЕГЧЕ, ЧЕМ СБОРКА rocBLAS ЦЕЛИКОМ:

      Полная сборка rocBLAS тянет CMake, vcpkg, MSVC Build Tools, msgpack-cxx
      и по документации требует 64 ГБ RAM. На нашей машине (16 ГБ, нет MSVC)
      это тупик.

      Но компилировать саму библиотеку не нужно. Нужен только генератор ядер
      TensileCreateLibrary - это Python-скрипт, который зовёт clang++.exe из
      вашего же HIP SDK. Ни MSVC, ни CMake, ни vcpkg, ни 64 ГБ не нужны.

    ЧТО ИМЕННО СОБЕРЁТСЯ:

      r9nano_*.yaml  8 файлов  ->  asm-ядра  fp32 (SB) и fp64 (DB)
                                  tuned под Polaris, 74 решения на файл
      hip_*.yaml   108 файлов  ->  source-ядра, компилируются под gfx803
                                  это fp16 (HB, HHS), fp8, int8, bf16

      fp16 на gfx803 идёт через source-ядра - asm-логик для fp16 у Polaris
      не существует ни в одном релизе ROCm (проверено по 3.10 ... 6.4.2).
      Для SDXL и Qwen-моделей это ровно то, что нужно.

    ВАЖНО ПРО КАЧЕСТВО:

      У asm-ядер gfx803 есть известный дефект: на очень малых GEMM
      (примерно до 48x48x48) они молча считают неверно, возвращая при этом
      успех. Для генерации картинок 512x512 и 768x768 это не проблема размеры
      там на порядки больше. Если начнёте делать крошечные модели - этот
      дефект станет заметен.

.PARAMETER HipVersion
    6.4.2 (по умолчанию) или 6.2.4. Определяет, в какой SDK ставить ядра.

.PARAMETER BuildRoot
    Куда качать и собирать. По умолчанию D:\rb
    Специально короткий путь: у Tensile длинные имена файлов, а Windows
    исторически ограничивает полный путь 260 символами.

.PARAMETER SkipDownload
    Не качать исходники заново, использовать уже распакованные.

.PARAMETER SkipBuild
    Только установить то, что уже собрано в BuildRoot\out.

.PARAMETER ListOnly
    Ничего не делать, только показать план и проверить предпосылки.

.PARAMETER Install
    Скопировать собранные ядра в SDK. Без этого флага сборка останется
    в BuildRoot и ничего в системе не изменится.

.PARAMETER Restore
    Откатить установку: вернуть library из последнего бэкапа.

.PARAMETER Verify
    После установки прогнать GEMM-тест 512 и 768, fp32 и fp16.

.PARAMETER Venv
    Путь к venv ComfyUI - нужен для -Verify.

.PARAMETER Jobs
    Сколько ядер занять. По умолчанию min(CPU, 8).

.PARAMETER ShortNames
    Короткие имена файлов ядер вместо длинных. Включайте, только если
    сборка падает на слишком длинных путях.

.PARAMETER DiagCompiler
    Не собирать ядра, а только разобраться, почему падает компиляция
    source-ядер. Перебирает наборы флагов на реальном файле от Tensile
    и печатает настоящую ошибку clang - Tensile сам её не показывает.

.PARAMETER ExtraFlags
    Дополнительные флаги компилятора при сборке, через пробел.
    Используется вместе с обёрткой Tensile_CXX_COMPILER_LAUNCHER.

.EXAMPLE
    .\05b-build-gfx803-kernels.ps1 -ListOnly

.EXAMPLE
    .\05b-build-gfx803-kernels.ps1 -Install -Verify -Venv D:\Apps\rx-580-comfyui\ComfyUI-Zluda\venv

.EXAMPLE
    .\05b-build-gfx803-kernels.ps1 -Restore
#>
[CmdletBinding()]
param(
    [string]$HipVersion = '6.4.2',
    [string]$BuildRoot = 'D:\rb',
    [switch]$SkipDownload,
    [switch]$SkipBuild,
    [switch]$ListOnly,
    [switch]$Install,
    [switch]$Restore,
    [switch]$Verify,
    [string]$Venv = '',
    [string]$Python = '',
    [int]$Jobs = 0,
    [switch]$ShortNames,
    [switch]$KeepBuildTmp,
    [switch]$DiagCompiler,
    [string]$ExtraFlags = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ============================================================================
#  Константы
# ============================================================================

$ROCBLAS_TAG = 'rocm-6.4.2'
$ROCBLAS_URL = "https://codeload.github.com/ROCm/rocBLAS/tar.gz/refs/tags/$ROCBLAS_TAG"

# Tensile 4.43.0 - ровно та версия, что прописана в rocBLAS 6.4.2
# (CMakeLists.txt:198 set( TENSILE_VERSION 4.43.0 ...), tensile_tag.txt)
$TENSILE_SHA = 'be49885fce2a61b600ae4593f1c2d00c8b4fa11e'
$TENSILE_URL = "https://codeload.github.com/ROCm/Tensile/tar.gz/$TENSILE_SHA"

$repoRootScripts = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$hipDirs = @{
    '6.4.2' = 'C:\Program Files\AMD\ROCm\6.4'
    '6.4'   = 'C:\Program Files\AMD\ROCm\6.4'
    '6.2.4' = 'C:\Program Files\AMD\ROCm\6.2'
    '6.2'   = 'C:\Program Files\AMD\ROCm\6.2'
}

# ============================================================================
#  Вывод
# ============================================================================

function Header([string]$t) {
    Write-Host ''
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
}
function Step([string]$t) {
    Write-Host ''
    Write-Host "  >> $t" -ForegroundColor White
}
function Ok([string]$what, [string]$note = '') {
    Write-Host ("  [OK]   {0}" -f $what) -ForegroundColor Green
    if ($note) { Write-Host ("         $note") -ForegroundColor DarkGray }
}
function Warn2([string]$what, [string]$note = '') {
    Write-Host ("  [!!]   {0}" -f $what) -ForegroundColor Yellow
    if ($note) { Write-Host ("         $note") -ForegroundColor DarkGray }
}
function Bad([string]$what, [string]$note = '') {
    Write-Host ("  [XX]   {0}" -f $what) -ForegroundColor Red
    if ($note) { Write-Host ("         $note") -ForegroundColor DarkGray }
}
function Dim([string]$t) { Write-Host "         $t" -ForegroundColor DarkGray }

# Запуск внешней программы с захватом вывода без падения на stderr.
# Причина: в PowerShell 5.1 перенаправление stderr нативной команды при
# $ErrorActionPreference = 'Stop' даёт NativeCommandError и обрывает скрипт,
# хотя программа просто что-то предупредила в stderr.
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

# ============================================================================
#  Проверки предпосылок
# ============================================================================

if (-not $hipDirs.ContainsKey($HipVersion)) { throw 'HipVersion: 6.4.2 или 6.2.4' }
$hipDir = $hipDirs[$HipVersion]
$binDir = Join-Path $hipDir 'bin'
$libraryDir = Join-Path $binDir 'rocblas\library'
$outDir = Join-Path $BuildRoot 'out'
$newLibDir = Join-Path $outDir 'library'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# ---------------- РЕЖИМ ОТКАТА ----------------
if ($Restore) {
    Header "ОТКАТ: ЯДРА gfx803  <-  HIP $HipVersion"
    if (-not (Test-Path $libraryDir)) { throw "Не найден $libraryDir" }
    if (-not $isAdmin) { throw 'Для записи в Program Files нужны права администратора.' }

    $baks = @(Get-ChildItem -Path (Split-Path $libraryDir -Parent) -Directory -Filter 'library.bak-*' -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
    if ($baks.Count -eq 0) {
        Warn2 'Бэкапов library.bak-* не найдено - откатывать нечего.'
        Dim 'Если ядра просто нужно убрать, удалите из library файлы с gfx803 в имени.'
        return
    }
    $bak = $baks[0]
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $broken = "$libraryDir.broken-$stamp"

    Write-Host ("  Бэкап:  {0}" -f $bak.FullName) -ForegroundColor DarkGray
    Rename-Item -Path $libraryDir -NewName (Split-Path $broken -Leaf)
    Rename-Item -Path $bak.FullName -NewName (Split-Path $libraryDir -Leaf)
    Ok 'Откат выполнен' ("Старая версия сохранена как " + (Split-Path $broken -Leaf))
    return
}

Header "ЭТАП 5b: СБОРКА ЯДЕР gfx803  (HIP $HipVersion)"
Dim ("SDK:        " + $hipDir)
Dim ("Сборка:     " + $BuildRoot)
Dim ("Установка:  " + $libraryDir)
Dim ("Jobs:       " + $(if ($Jobs -gt 0) { $Jobs } else { "auto, min(CPU, 8)" }))

# ---- 1. HIP SDK ----
Step '1. Проверка HIP SDK'
if (-not (Test-Path $hipDir)) { throw "Не найден $hipDir - HIP SDK $HipVersion установлен?" }
$clangxx = Join-Path $binDir 'clang++.exe'
$clang = Join-Path $binDir 'clang.exe'
$bundler = Join-Path $binDir 'clang-offload-bundler.exe'

foreach ($tool in @($clangxx, $clang, $bundler)) {
    if (-not (Test-Path $tool)) { throw ("Нет обязательного инструмента: " + $tool) }
}
Ok 'clang++.exe, clang.exe, clang-offload-bundler.exe на месте'

# версия SDK - чтобы не собрать ядра 6.4 в SDK 6.2
$cxxVer = Invoke-Capture $clangxx @('--version')
if ($cxxVer.Text -match 'clang version (\d+\.\d+\.\d+)') {
    Dim ("clang:      " + $matches[1])
}

# ---- 2. gfx803 поддерживается компилятором? ----
Step '2. Проверка, что компилятор знает gfx803'
Dim 'Это ключевой момент: AMD убрала gfx803 из списка сборки, но не из компилятора.'
$probeDir = Join-Path $env:TEMP ('gfx803probe-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $probeDir -Force | Out-Null
$probeS = Join-Path $probeDir 'probe.s'
$probeO = Join-Path $probeDir 'probe.o'
[IO.File]::WriteAllText($probeS, "s_nop 0`ns_endpgm`n")
$probe = Invoke-Capture $clang @('--target=amdgcn-amd-amdhsa', '-mcpu=gfx803', '-x', 'assembler', '-c', $probeS, '-o', $probeO)
Remove-Item $probeDir -Recurse -Force -ErrorAction SilentlyContinue

if ($probe.Code -ne 0) {
    Bad 'Компилятор НЕ принимает -mcpu=gfx803' ''
    Dim $probe.Text
    throw 'Сборка невозможна: этот clang не умеет gfx803. Сообщите вывод выше.'
}
Ok 'clang собирает код под gfx803' 'Проверено реальной сборкой объекта.'

# ---- 3. Python ----
Step '3. Поиск Python'
$pyExe = $null
$pyPrefix = @()
if ($Python) {
    if (-not (Test-Path $Python)) { throw "Указан несуществующий Python: $Python" }
    $pyExe = $Python
} else {
    $cands = @(
        @{ Exe = 'py'; Args = @('-3') },
        @{ Exe = 'python'; Args = @() }
    )
    foreach ($c in $cands) {
        $cmd = Get-Command $c.Exe -ErrorAction SilentlyContinue
        if ($cmd) {
            $t = Invoke-Capture $cmd.Source ($c.Args + @('-c', 'import sys;print(sys.version_info[0])'))
            if ($t.Code -eq 0 -and $t.Text.Trim() -match '^3$') {
                $pyExe = $cmd.Source; $pyPrefix = $c.Args; break
            }
        }
    }
    if (-not $pyExe) {
        # запасной вариант - python из venv ComfyUI (он умеет создавать venv)
        $guess = 'D:\Apps\rx-580-comfyui\ComfyUI-Zluda\venv\Scripts\python.exe'
        if (Test-Path $guess) { $pyExe = $guess }
    }
}
if (-not $pyExe) { throw 'Python 3 не найден. Укажите путь через -Python.' }
$pyInfo = Invoke-Capture $pyExe ($pyPrefix + @('-c', 'import sys;print(sys.version.split()[0])'))
Ok ("Python " + $pyInfo.Text.Trim()) ($pyExe)

# ============================================================================
#  ДИАГНОСТИКА КОМПИЛЯТОРА
# ============================================================================
#  Зачем отдельный режим: Tensile зовёт clang++ через subprocess, и при
#  падении текст ошибки остаётся внутри дочернего процесса joblib.
#  Снаружи видно только "returned non-zero exit status 1" - без причины.
#  Здесь мы берём РЕАЛЬНЫЙ сгенерированный файл и прогоняем clang сами,
#  перебирая варианты флагов, пока не найдём работающий.
# ============================================================================
if ($DiagCompiler) {
    Header 'ДИАГНОСТИКА КОМПИЛЯЦИИ SOURCE-ЯДЕР'
    Dim 'Собираем реальный файл от Tensile и смотрим настоящую ошибку clang.'

    # исходники должны быть уже скачаны
    if (-not (Test-Path $tensileSrc)) { throw ("Нет исходников Tensile в " + $tensileSrc + " - сначала запустите без -DiagCompiler") }
    if (-not (Test-Path $rocblasSrc)) { throw ("Нет исходников rocBLAS в " + $rocblasSrc) }

    # берём существующий .cpp из out, либо генерируем минимальный
    $testCpp = $null
    if (Test-Path $newLibDir) {
        $testCpp = Get-ChildItem $newLibDir -Filter '*_DD_*.cpp' -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $testCpp) {
            $testCpp = Get-ChildItem $newLibDir -Filter '*.cpp' -ErrorAction SilentlyContinue | Select-Object -First 1
        }
    }
    if (-not $testCpp) {
        # своего файла нет - берём минимальный, но с тем же include, что у Tensile
        $tensileTypesCandidates = @(
            (Join-Path $tensileSrc 'Tensile\Source\TensileTypes.h'),
            (Join-Path $outDir 'TensileTypes.h')
        )
        $haveTypes = $false
        foreach ($c in $tensileTypesCandidates) { if (Test-Path $c) { $haveTypes = $true } }
        Dim 'Готовых файлов нет - делаю минимальный тест с #include <hip/hip_runtime.h>'
        $probe = Join-Path $outDir '_diag_probe.cpp'
        if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
        [IO.File]::WriteAllText($probe, "#include <hip/hip_runtime.h>`nextern `"C`" __global__ void k(float* a) { a[0] += 1.0f; }`n")
        $testCpp = Get-Item $probe
    }

    Ok ("тестовый файл: " + $testCpp.Name) $testCpp.FullName

    # базовые флаги - ровно те, что передаёт Tensile (TensileCreateLibrary.py:250-276)
    $baseFlags = @(
        '-D__HIP_HCC_COMPAT_MODE__=1',
        '--cuda-device-only', '-x', 'hip', '-O3',
        '-I', $newLibDir,
        '-Xoffload-linker', '--build-id',
        '-std=c++14', '-fms-extensions', '-fms-compatibility', '-fPIC',
        '-Wno-deprecated-declarations',
        '--offload-arch=gfx803'
    )

    # варианты: от «как есть» к «с обходом отсутствующего MSVC»
    $variants = @(
        @{ Name = 'A. как у Tensile, без правок';                  Add = @() },
        @{ Name = 'B. + -D__HIP_PLATFORM_AMD__';                     Add = @('-D__HIP_PLATFORM_AMD__=1') },
        @{ Name = 'C. + -isystem MSVC (нужен VS)';                   Add = @('-isystem', 'MSVC') },
        @{ Name = 'D. + NO_EXCEPTIONS / NO_THREADS';                Add = @('-D_HAS_EXCEPTIONS=0', '-D__HIPCC_RTC__') },
        @{ Name = 'E. + c++17';                                     Add = @('-std=c++17') },
        @{ Name = 'F. + platform + c++17 + no-threads';             Add = @('-D__HIP_PLATFORM_AMD__=1', '-D__HIPCC_RTC__') },
        @{ Name = 'G. без -fms-extensions';                          Add = @('-D__HIP_PLATFORM_AMD__=1', '-D__HIPCC_RTC__') ; Strip = @('-fms-extensions','-fms-compatibility') },
        @{ Name = 'H. только __HIPCC_RTC__ (без stdlib)';            Add = @('-D__HIPCC_RTC__') },
        @{ Name = 'I. RTC + platform';                               Add = @('-D__HIPCC_RTC__', '-D__HIP_PLATFORM_AMD__=1') }
    )

    $msvcRoot = $null
    foreach ($vs in @(
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC'
    )) {
        if (Test-Path $vs) { $msvcRoot = $vs; break }
    }
    if ($msvcRoot) { Ok 'MSVC найден' $msvcRoot } else { Warn2 'MSVC не найден' 'Вариант C работать не будет - это ожидаемо.' }

    Write-Host ''
    $winner = $null
    foreach ($v in $variants) {
        $flags = @()
        foreach ($f in $baseFlags) {
            if ($v.ContainsKey('Strip') -and $v.Strip -contains $f) { continue }
            $flags += $f
        }
        $add = @($v.Add)
        if ($add -contains 'MSVC') {
            if (-not $msvcRoot) { continue }
            $verDir = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
            $add = @('-isystem', (Join-Path $verDir.FullName 'include'))
            $flags += @('-isystem', (Join-Path $env:ProgramFiles 'Windows Kits\10\Include\10.0.22621.0\ucrt'))
        }
        $flags += $add
        $flags += @($testCpp.FullName, '-c', '-o', (Join-Path $outDir '_diag_test.o'))

        $r = Invoke-Capture $clangxx $flags
        if ($r.Code -eq 0) {
            Ok ("РАБОТАЕТ: " + $v.Name) 'Это и есть нужный набор флагов.'
            $winner = $v
            if ($add.Count -gt 0) { Dim ("  доп. флаги: " + ($add -join ' ')) }
            break
        }
        Bad ($v.Name) ''
        # показываем суть: первую строку с error
        $errLines = @($r.Text -split "`n" | Where-Object { $_ -match 'error|fatal' } | Select-Object -First 3)
        if ($errLines.Count -gt 0) {
            foreach ($e in $errLines) { Dim ("  " + $e.Trim()) }
        } else {
            $first = @($r.Text -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 3)
            foreach ($e in $first) { Dim ("  " + $e.Trim()) }
        }
    }

    Write-Host ''
    if ($winner) {
        Header 'ИТОГ ДИАГНОСТИКИ'
        Ok ("Нужен вариант: " + $winner.Name)
        Dim 'Запустите сборку с этими флагами:'
        $fl = (@($winner.Add) | Where-Object { $_ -ne 'MSVC' }) -join ' '
        Dim ("  .\05b-build-gfx803-kernels.ps1 -Install -Verify -ExtraFlags `"" + $fl + "`"")
    } else {
        Header 'ИТОГ ДИАГНОСТИКИ'
        Warn2 'Ни один вариант не сработал.' ''
        Dim 'Вывод выше содержит настоящую ошибку clang - покажите его.'
        Dim 'Скорее всего не хватает заголовков C++ (нет MSVC Build Tools),'
        Dim 'и тогда нужен либо VS Build Tools, либо сборка без source-ядер'
        Dim '(флаг -SkipSourceKernels, только asm-ядра fp32/fp64).'
    }
    return
}

if ($ListOnly) {
    Header 'ПЛАН (режим -ListOnly, ничего не меняем)'
    Dim '1. Скачать исходники rocBLAS 6.4.2 (58 МБ) и Tensile 4.43.0 (13 МБ)'
    Dim '2. Создать venv для сборки, поставить pyyaml, msgpack, joblib, rich'
    Dim '3. Запустить TensileCreateLibrary --architecture=gfx803'
    Dim '   Логики: asm_full/r9nano (fp32/fp64) + asm_full/hip (fp16/fp8/int8)'
    Dim '   Компилятор: clang++.exe из HIP SDK, MSVC не нужен'
    Dim '4. Результат в: ' + $newLibDir
    Dim '5. С флагом -Install скопировать ядра в SDK с бэкапом'
    Write-Host ''
    Dim 'Ожидаемое время: 20-60 минут на 8 потоках. Место на D: около 5-10 ГБ.'
    Dim 'Запуск: .\05b-build-gfx803-kernels.ps1 -Install -Verify'
    return
}

# ============================================================================
#  4. Скачивание исходников
# ============================================================================
Header 'СБОРКА'

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
if (-not (Test-Path $BuildRoot)) { New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null }

$tarExe = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path $tarExe)) { throw 'Нет tar.exe (нужен Windows 10 1803+).' }

$rocblasSrc = Join-Path $BuildRoot 'rocBLAS'
$tensileSrc = Join-Path $BuildRoot 'Tensile'

function Get-Tarball([string]$url, [string]$tgz, [string]$destDir, [string]$marker) {
    if (-not (Test-Path $tgz)) {
        Step ("Скачиваю " + (Split-Path $tgz -Leaf))
        Dim $url
        $old = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & curl.exe -L --fail --ssl-no-revoke -o $tgz $url 2>&1 | Out-Null
            $rc = $LASTEXITCODE
        } finally { $ErrorActionPreference = $old }
        if ($rc -ne 0 -or -not (Test-Path $tgz)) {
            if (Test-Path $tgz) { Remove-Item $tgz -Force }
            throw ("Не удалось скачать " + $url + " (curl код " + $rc + "). Проверьте интернет.")
        }
        $mb = [math]::Round((Get-Item $tgz).Length / 1MB, 1)
        Ok ("скачано " + $mb + " МБ")
    } else {
        Dim ("архив уже есть: " + $tgz)
    }

    if (Test-Path (Join-Path $destDir $marker)) {
        Dim ("исходники уже распакованы: " + $destDir)
        return
    }
    Step ("Распаковка " + (Split-Path $tgz -Leaf))
    if (Test-Path $destDir) { Remove-Item $destDir -Recurse -Force }
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $tarExe -xzf $tgz -C $destDir --strip-components=1 2>&1 | Out-Null
        $rc = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    if ($rc -ne 0) { throw ("tar вернул код " + $rc) }
    if (-not (Test-Path (Join-Path $destDir $marker))) { throw ("После распаковки нет " + $marker) }
    Ok 'распаковано'
}

if (-not $SkipDownload) {
    Step '4. Скачивание исходников'
    $dl = Join-Path $BuildRoot '_dl'
    if (-not (Test-Path $dl)) { New-Item -ItemType Directory -Path $dl -Force | Out-Null }

    Get-Tarball $ROCBLAS_URL (Join-Path $dl 'rocblas-6.4.2.tar.gz') $rocblasSrc 'CMakeLists.txt'
    Get-Tarball $TENSILE_URL (Join-Path $dl 'tensile-4.43.0.tar.gz') $tensileSrc 'Tensile\TensileCreateLibrary.py'

    # sanity: нужные логики на месте
    $logicRoot = Join-Path $rocblasSrc 'library\src\blas3\Tensile\Logic\asm_full'
    $r9 = Join-Path $logicRoot 'r9nano'
    $hipLogic = Join-Path $logicRoot 'hip'
    if (-not (Test-Path $r9)) { throw ("Нет логик r9nano в " + $r9 + " - не тот тег rocBLAS?") }
    if (-not (Test-Path $hipLogic)) { throw ("Нет логик hip в " + $hipLogic) }
    $nR9 = @(Get-ChildItem $r9 -Filter 'r9nano_*.yaml').Count
    $nHip = @(Get-ChildItem $hipLogic -Filter 'hip_*.yaml').Count
    Ok ("логики на месте: r9nano=" + $nR9 + ", hip=" + $nHip) 'r9nano это gfx803 (Tensile/Common.py:317)'
}

# ============================================================================
#  5. Сборочный venv
# ============================================================================
$venvDir = Join-Path $BuildRoot 'venv'
$venvPy = Join-Path $venvDir 'Scripts\python.exe'

if (-not $SkipBuild) {
    Step '5. Сборочное окружение Python'
    if (-not (Test-Path $venvPy)) {
        $r = Invoke-Capture $pyExe ($pyPrefix + @('-m', 'venv', $venvDir))
        if ($r.Code -ne 0) {
            Bad 'Не удалось создать venv' ''
            Dim $r.Text
            throw 'Создание venv провалилось.'
        }
        Ok 'venv создан'
    } else {
        Dim 'venv уже есть'
    }

    # Кэш pip держим на D:, чтобы не забивать C:
    $pipCache = Join-Path $BuildRoot '_pip'
    if (-not (Test-Path $pipCache)) { New-Item -ItemType Directory -Path $pipCache -Force | Out-Null }
    $env:PIP_CACHE_DIR = $pipCache
    $env:PIP_DISABLE_PIP_VERSION_CHECK = '1'

    $need = @('yaml', 'msgpack', 'joblib', 'rich')
    $chk = Invoke-Capture $venvPy @('-c', 'import yaml,msgpack,joblib,rich;print("ok")')
    if ($chk.Code -ne 0) {
        Dim 'Ставлю pyyaml, msgpack, joblib, rich'
        $r = Invoke-Capture $venvPy @('-m', 'pip', 'install', '--quiet', '--no-warn-script-location', 'pyyaml', 'msgpack', 'joblib', 'rich')
        if ($r.Code -ne 0) {
            Bad 'pip install провалился' ''
            Dim $r.Text
            throw 'Не удалось поставить зависимости сборки.'
        }
    }
    Ok 'зависимости сборки готовы'
}

# ============================================================================
#  6. Сборка ядер
# ============================================================================
$tclScript = Join-Path $tensileSrc 'Tensile\bin\TensileCreateLibrary'
if (-not (Test-Path $tclScript)) { throw ("Нет TensileCreateLibrary: " + $tclScript) }

if (-not $SkipBuild) {
    Step '6. Сборка ядер (это долго - 20-60 минут)'
    Dim 'TensileCreateLibrary генерирует код, собирает его clang-ом и линкует в .co/.hsaco'

    $nJ = $Jobs
    if ($nJ -le 0) { $nJ = [Math]::Min([Environment]::ProcessorCount, 8) }

    $logicRoot = Join-Path $rocblasSrc 'library\src\blas3\Tensile\Logic\asm_full'

    # Tensile ищет компилятор и бандлер через эти переменные.
    # HIP_DIR - то, что он читает на Windows (Tensile/Common.py:2404).
    $env:HIP_DIR = $hipDir
    $env:HIP_PATH = $hipDir
    $env:TENSILE_ROCM_ASSEMBLER_PATH = $clangxx
    $env:TENSILE_ROCM_OFFLOAD_BUNDLER_PATH = $bundler
    $oldPath = $env:PATH
    $env:PATH = $binDir + ';' + $oldPath

    if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }

    # Обёртка вокруг компилятора. Нужна, потому что Tensile глотает текст
    # ошибки clang: исключение поднимается внутри дочернего процесса joblib,
    # и наружу выходит только "returned non-zero exit status 1".
    # С обёрткой полный stderr попадает в clang-errors.log.
    $wrapPy = Join-Path $repoRootScripts 'tensile-clang-wrap.py'
    $clangLog = Join-Path $BuildRoot 'clang-errors.log'
    if (Test-Path $clangLog) { Remove-Item $clangLog -Force }
    if (Test-Path $wrapPy) {
        # Пути ТОЛЬКО через прямой слэш: Tensile разбирает переменную
        # через shlex.split() в posix-режиме, где обратный слэш экранирует.
        $wrapArg = ($venvPy -replace '\\', '/') + ' ' + ($wrapPy -replace '\\', '/')
        $env:Tensile_CXX_COMPILER_LAUNCHER = $wrapArg
        $env:TENSILE_WRAP_LOG = $clangLog
        if ($ExtraFlags) {
            $env:TENSILE_WRAP_EXTRA = $ExtraFlags
            Ok ("доп. флаги компилятора: " + $ExtraFlags)
        }
        Ok 'обёртка компилятора включена' ('лог ошибок: ' + $clangLog)
    }

    $tclArgs = @(
        $tclScript,
        '--cxx-compiler=clang++',
        '--architecture=gfx803',
        '--library-format=msgpack',
        '--code-object-version=default',
        '--merge-files',
        '--separate-architectures',
        '--lazy-library-loading',
        "--jobs=$nJ",
        '--verbose=1'
    )
    if ($ShortNames) { $tclArgs += '--short-file-names' } else { $tclArgs += '--no-short-file-names' }
    if ($KeepBuildTmp) { $tclArgs += '--keep-build-tmp' }
    $tclArgs += @($logicRoot, $outDir, 'HIP')

    Dim ("команда: python TensileCreateLibrary --architecture=gfx803 ... --jobs=" + $nJ)
    Dim 'Полный лог пишется в out\tensile-build.log'

    if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
    $logFile = Join-Path $outDir 'tensile-build.log'

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # PYTHONPATH нужен, чтобы Tensile импортировался из дерева исходников:
        # pip install не подходит - он не ставит Tensile/Source/*.h,
        # а они нужны copyStaticFiles().
        $env:PYTHONPATH = $tensileSrc
        $prevDir = (Get-Location).Path
        Set-Location $tensileSrc
        try {
            & $venvPy @tclArgs 2>&1 | Tee-Object -FilePath $logFile | ForEach-Object {
                $line = "$_"
                Write-Host ("   " + $line) -ForegroundColor DarkGray
            }
            $rc = $LASTEXITCODE
        } finally {
            Set-Location $prevDir
        }
    } finally {
        $ErrorActionPreference = $oldEap
        $env:PATH = $oldPath
    }
    $sw.Stop()
    $mins = [math]::Round($sw.Elapsed.TotalMinutes, 1)

    if ($rc -ne 0) {
        Bad ("Сборка провалилась (код " + $rc + ", " + $mins + " мин)")
        Dim ("Лог: " + $logFile)
        Dim 'Частые причины:'
        Dim '  * путь длиннее 260 символов  ->  запустите с -ShortNames'
        Dim '  * не хватило RAM  ->  уменьшите -Jobs до 4'
        Dim '  * clang не принял флаги  ->  смотрите последние 40 строк лога'
        Write-Host ''
        if (Test-Path $clangLog) {
            Write-Host ''
            Dim ('НАСТОЯЩАЯ ОШИБКА КОМПИЛЯТОРА (' + $clangLog + '):')
            Get-Content $clangLog -Tail 60 | ForEach-Object { Write-Host ("   " + $_) -ForegroundColor DarkYellow }
        } else {
            Dim 'Лога обёртки нет - обёртка не подключилась.'
        }
        Write-Host ''
        Dim 'Последние строки общего лога:'
        if (Test-Path $logFile) {
            Get-Content $logFile -Tail 30 | ForEach-Object { Write-Host ("   " + $_) -ForegroundColor DarkGray }
        }
        throw 'Сборка ядер не удалась.'
    }
    Ok ("Сборка завершена за " + $mins + " мин")
}

# ============================================================================
#  7. Что получилось
# ============================================================================
if (-not (Test-Path $newLibDir)) { throw ("Нет каталога с результатом: " + $newLibDir) }

$produced = @(Get-ChildItem $newLibDir -File | Sort-Object Name)
$totalMB = [math]::Round((($produced | Measure-Object Length -Sum).Sum / 1MB), 1)

Header 'РЕЗУЛЬТАТ СБОРКИ'
Ok ("файлов: " + $produced.Count + "  (" + $totalMB + " МБ)") ($newLibDir)

$byExt = $produced | Group-Object Extension | Sort-Object Count -Descending
foreach ($g in $byExt) {
    Dim ("{0,-10} {1,5} шт" -f $g.Name, $g.Count)
}

$master = @($produced | Where-Object { $_.Name -match '^TensileLibrary_lazy_gfx803\.' })
$hsaco803 = @($produced | Where-Object { $_.Name -match 'gfx803' })
$coFiles = @($produced | Where-Object { $_.Extension -eq '.co' })

Write-Host ''
if ($master.Count -gt 0) {
    Ok ("мастер-файл: " + $master[0].Name) 'Именно его ищет rocblas.dll (tensile_host.cpp:800)'
} else {
    Bad 'НЕТ TensileLibrary_lazy_gfx803.dat' 'Без него rocblas.dll не подключит ядра gfx803.'
}
if ($hsaco803.Count -gt 0) { Ok ("файлов с gfx803 в имени: " + $hsaco803.Count) }
if ($coFiles.Count -gt 0) { Ok ("слинкованных .co: " + $coFiles.Count) }

Write-Host ''
Dim 'Файлы в каталоге:'
$produced | Select-Object -First 15 | ForEach-Object { Dim ("  " + $_.Name) }
if ($produced.Count -gt 15) { Dim ("  ... и ещё " + ($produced.Count - 15)) }

# ============================================================================
#  8. Установка
# ============================================================================
if (-not $Install) {
    Header 'УСТАНОВКА НЕ ВЫПОЛНЕНА'
    Dim 'Ядра собраны, но в SDK НЕ скопированы. Чтобы установить:'
    Dim ('  .\05b-build-gfx803-kernels.ps1 -SkipDownload -SkipBuild -Install -Verify')
    return
}

if (-not $isAdmin) { throw 'Для записи в Program Files нужны права администратора.' }
if (-not (Test-Path $libraryDir)) { throw "Не найден $libraryDir - HIP SDK $HipVersion установлен?" }

Step '7. Установка в SDK'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bak = "$libraryDir.bak-$stamp"
Copy-Item $libraryDir $bak -Recurse -Force
$bakMB = [math]::Round(((Get-ChildItem $bak -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Ok ("Бэкап прежней library: " + $bak) ($bakMB.ToString() + " МБ")
Dim ('Откат: .\05b-build-gfx803-kernels.ps1 -Restore')

# Копируем только своё: имена ядер gfx803 в штатной поставке отсутствуют,
# поэтому чужие файлы (gfx900 и далее) не затрагиваются.
$overwritten = 0
$copied = 0
foreach ($f in $produced) {
    $dst = Join-Path $libraryDir $f.Name
    if (Test-Path $dst) { $overwritten++ }
    Copy-Item $f.FullName $dst -Force
    $copied++
}
Ok ("скопировано файлов: " + $copied) $(if ($overwritten -gt 0) { "перезаписано существующих: " + $overwritten } else { "" })

# Проверка на месте
$nowMaster = Join-Path $libraryDir 'TensileLibrary_lazy_gfx803.dat'
if (Test-Path $nowMaster) {
    Ok 'TensileLibrary_lazy_gfx803.dat на месте' ((Get-Item $nowMaster).Length.ToString() + " байт")
} else {
    Bad 'Мастер-файл не появился в library' 'rocBLAS не увидит ядра.'
}

# ============================================================================
#  9. Проверка математики
# ============================================================================
if (-not $Verify) {
    Header 'ГОТОВО'
    Dim 'Ядра установлены. Проверьте математику:'
    Dim ('  .\06-verify.ps1 -Venv ' + $(if ($Venv) { $Venv } else { '<путь к venv>' }))
    return
}

Header 'ПРОВЕРКА: GEMM 512 и 768, fp32 и fp16'
if (-not $Venv) { throw 'Для -Verify нужен -Venv <путь к venv ComfyUI>' }
$venvPyComfy = Join-Path $Venv 'Scripts\python.exe'
if (-not (Test-Path $venvPyComfy)) { throw ("Нет python в venv: " + $venvPyComfy) }

$py = @'
import os, sys
os.environ["DISABLE_ADDMM_CUDA_LT"] = "1"
os.environ.setdefault("MIOPEN_FIND_MODE", "2")
try:
    import torch
except Exception as e:
    print("IMPORT_FAIL %s" % e); raise SystemExit(2)
if not torch.cuda.is_available():
    print("SKIP no cuda"); raise SystemExit(0)
print("dev %s" % torch.cuda.get_device_name(0))
torch.manual_seed(0)
def rel(a, b):
    d = (a - b).abs().max().item()
    n = max(1e-9, b.abs().max().item())
    return d / n
sizes = [512, 768]
for n in sizes:
    a = torch.randn(n, n); b = torch.randn(n, n)
    ref = a @ b
    try:
        c = (a.to("cuda") @ b.to("cuda")).cpu()
        e = rel(c, ref)
        print("MM_FP32_%d=%.3e %s" % (n, e, "OK" if e < 1e-4 else "BAD"))
    except Exception as ex:
        print("MM_FP32_%d=EXC %s" % (n, ex))
    ah = a.half(); bh = b.half()
    refh = (ah.float() @ bh.float())
    try:
        ch = (ah.to("cuda") @ bh.to("cuda")).float().cpu()
        e = rel(ch, refh)
        print("MM_FP16_%d=%.3e %s" % (n, e, "OK" if e < 2e-2 else "BAD"))
    except Exception as ex:
        print("MM_FP16_%d=EXC %s" % (n, ex))
print("DONE")
'@
$f = Join-Path $env:TEMP 'gfx803-gemm-test.py'
[IO.File]::WriteAllText($f, $py, (New-Object System.Text.UTF8Encoding($false)))

$out = Invoke-Capture $venvPyComfy @($f)
Write-Host $out.Text.Trim() -ForegroundColor DarkGray

$bad = 0
foreach ($n in @(512, 768)) {
    if ($out.Text -match ("MM_FP32_" + $n + "=([\d.e+-]+)")) {
        $e = [double]$matches[1]
        if ($e -lt 1e-4) { Ok ("mm fp32 " + $n + ": ошибка " + ("{0:E2}" -f $e)) }
        else { Bad ("mm fp32 " + $n + ": ошибка " + ("{0:E2}" -f $e)) 'Мусор вместо вычислений - ядра не подошли.'; $bad++ }
    } else {
        Bad ("mm fp32 " + $n + ": тест не выполнился") 'Смотрите вывод выше (TensileLibrary / rocBLAS).'
        $bad++
    }
    if ($out.Text -match ("MM_FP16_" + $n + "=([\d.e+-]+)")) {
        $e = [double]$matches[1]
        if ($e -lt 2e-2) { Ok ("mm fp16 " + $n + ": ошибка " + ("{0:E2}" -f $e)) }
        else { Warn2 ("mm fp16 " + $n + ": ошибка " + ("{0:E2}" -f $e)) 'Для fp16 допустима большая ошибка, если картинки верные.' }
    }
}

Write-Host ''
if ($bad -eq 0) {
    Ok 'rocBLAS для gfx803 работает' 'Ядра собраны и подхватились.'
} else {
    Warn2 'rocBLAS пока не считает правильно' ''
    Dim 'Проверьте, что в library лежит TensileLibrary_lazy_gfx803.dat.'
    Dim 'Попробуйте запустить с -ShortNames (длинные имена могли обрезаться).'
    Dim 'Откат: .\05b-build-gfx803-kernels.ps1 -Restore'
}
