<#
.SYNOPSIS
    Вторая ступень диагностики rocBLAS: почему не грузится rocblas.dll.

.DESCRIPTION
    Первая ступень (diagnose-rocblas.ps1) показала:
      * ядер gfx803 нет (это отдельная проблема, решается этапом 5);
      * rocblas.dll в bin весит 42 МБ - это меньше полной сборки с Tensile;
      * ctypes.WinDLL("rocblas.dll") падает с "or one of its dependencies",
        хотя файл на месте.

    Этот скрипт выясняет, ЧЕГО не хватает. Ничего не меняет, только читает.

      1. Все rocblas*.dll в SDK и в System32, с версиями и размерами.
      2. Реальный список зависимостей rocblas.dll (через dumpbin, если есть,
         иначе - перебором известных имён).
      3. Пробует загрузить её по полному пути в дочернем процессе python,
         чтобы поймать настоящий текст ошибки.
      4. Проверяет, есть ли rocblas.dll в %HIP_PATH%\lib - именно оттуда
         ZLUDA линкует символ (rocblas-sys/build.rs).

.PARAMETER HipVersion
    6.4.2 (по умолчанию) или 6.2.4.

.PARAMETER Venv
    Путь к venv ComfyUI.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\diagnose-rocblas-deps.ps1
#>
[CmdletBinding()]
param(
    [string]$HipVersion = '6.4.2',
    [string]$Venv = ''
)

$ErrorActionPreference = 'Continue'

$kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Venv) { $Venv = Join-Path $kitRoot 'ComfyUI-Zluda\venv' }
$venvPy = Join-Path $Venv 'Scripts\python.exe'

$hipDirs = @{
    '6.4.2' = 'C:\Program Files\AMD\ROCm\6.4'
    '6.4'   = 'C:\Program Files\AMD\ROCm\6.4'
    '6.2.4' = 'C:\Program Files\AMD\ROCm\6.2'
    '6.2'   = 'C:\Program Files\AMD\ROCm\6.2'
}
if (-not $hipDirs.ContainsKey($HipVersion)) { throw 'HipVersion: 6.4.2 или 6.2.4' }
$hipDir = $hipDirs[$HipVersion]

function Header([string]$t) {
    Write-Host ''
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
}
function Ok([string]$t)    { Write-Host "[OK]   $t" -ForegroundColor Green }
function Warn2([string]$t) { Write-Host "[WARN] $t" -ForegroundColor Yellow }
function Fail2([string]$t) { Write-Host "[FAIL] $t" -ForegroundColor Red }
function Info2([string]$t) { Write-Host "       $t" -ForegroundColor DarkGray }

Header 'ДИАГНОСТИКА 2: зависимости rocblas.dll'

# ---------- 1. где какие rocblas ----------
Header '1. Все rocblas*.dll на диске'

$roots = @(
    $hipDir,
    (Join-Path $env:WINDIR 'System32'),
    $kitRoot
)
$found = @()
foreach ($r in $roots) {
    if (-not (Test-Path $r)) { continue }
    $found += @(Get-ChildItem $r -Recurse -Filter 'rocblas*.dll' -File -ErrorAction SilentlyContinue)
}

if ($found.Count -eq 0) {
    Fail2 'rocblas*.dll не найдена вообще'
} else {
    foreach ($f in ($found | Sort-Object Length -Descending)) {
        $mb = [math]::Round($f.Length / 1MB, 2)
        $ver = $f.VersionInfo.FileVersion
        Write-Host ("  {0,7} MB  {1,-16} {2}" -f $mb, $ver, $f.FullName) -ForegroundColor Gray
    }
    Write-Host ''
    Info2 'Для справки: полная rocBLAS 6.2.4 с Tensile-ядрами ~138 MB.'
    Info2 'Заметно меньше - сборка без Tensile: cublasCreate пройдёт,'
    Info2 'но первый же GEMM упадёт на отсутствии решений.'
}

# ---------- 2. %HIP_PATH%\lib ----------
Header '2. rocblas.dll там, откуда её линкует ZLUDA'

Info2 'rocblas-sys/build.rs: cargo:rustc-link-search=%HIP_PATH%\lib'
$libDir = Join-Path $hipDir 'lib'
$libRoc = Join-Path $libDir 'rocblas.dll'
if (Test-Path $libRoc) {
    $i = Get-Item $libRoc
    Ok ("есть: {0} ({1:N2} MB)" -f $libRoc, ($i.Length / 1MB))
} else {
    Warn2 "нет $libRoc"
    Info2 'Это нормально для Windows-сборки rocBLAS в bin, но проверь:'
    Info2 'если ZLUDA при загрузке cublas.dll ищет rocblas рядом с собой'
    Info2 'или в PATH, а в PATH первым идёт bin - возьмётся bin-версия.'
}

Write-Host ''
Info2 'Фактический порядок поиска DLL в процессе - какой из них победит:'
$cands = @(
    (Join-Path $libDir 'rocblas.dll'),
    (Join-Path $hipDir 'bin\rocblas.dll'),
    (Join-Path $env:WINDIR 'System32\rocblas.dll')
)
$n = 0
foreach ($c in $cands) {
    if (Test-Path $c) {
        $n++
        $i = Get-Item $c
        Write-Host ("  {0}. {1}  ({2:N2} MB, {3})" -f $n, $c, ($i.Length / 1MB), $i.VersionInfo.FileVersion) -ForegroundColor Gray
    }
}

# ---------- 3. зависимости ----------
Header '3. Чего не хватает rocblas.dll'

$rocBin = Join-Path $hipDir 'bin\rocblas.dll'
$rocTarget = if (Test-Path $rocBin) { $rocBin } elseif (Test-Path $libRoc) { $libRoc } else { $null }

if (-not $rocTarget) {
    Fail2 'не нашёл ни bin, ни lib версию - пропускаю'
} else {
    $dumpbin = $null
    $cands = @(
        (Get-Command dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    )
    # ищем в VS Build Tools
    $vsPaths = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
    )
    foreach ($vp in $vsPaths) {
        if (Test-Path $vp) {
            $d = Get-ChildItem $vp -Recurse -Filter 'dumpbin.exe' -File -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($d) { $dumpbin = $d.FullName; break }
        }
    }

    if ($dumpbin) {
        Write-Host ("  dumpbin: {0}" -f $dumpbin) -ForegroundColor DarkGray
        $out = & $dumpbin /dependents $rocTarget 2>&1 | Out-String
        $deps = [regex]::Matches($out, '(?m)^\s+([A-Za-z0-9_.-]+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value }
        if ($deps.Count -gt 0) {
            Write-Host '  Зависимости:' -ForegroundColor White
            foreach ($d in ($deps | Sort-Object -Unique)) {
                $resolved = $null
                foreach ($p in @((Join-Path $hipDir 'bin'), $env:PATH -split ';', (Join-Path $env:WINDIR 'System32'))) {
                    if (-not $p) { continue }
                    $try = Join-Path $p $d
                    if (Test-Path $try) { $resolved = $try; break }
                }
                if ($resolved) {
                    Write-Host ("    [есть]  {0}" -f $d) -ForegroundColor Green
                } else {
                    Write-Host ("    [НЕТ]   {0}  <- возможно, вот причина" -f $d) -ForegroundColor Red
                }
            }
        } else {
            Warn2 'dumpbin не вернул список - смотри вывод выше'
        }
    } else {
        Warn2 'dumpbin не найден (нет MSVC Build Tools) - проверяю перебором'
        Info2 'Кандидаты, без которых rocBLAS обычно не грузится:'
        $need = @('amdhip64.dll','amdhip64_6.dll','amd_comgr_2.dll','amd_comgr_3.dll','hipblaslt.dll','TensileLibrary.dll','rocsolver.dll','rocsparse.dll')
        foreach ($d in $need) {
            $p1 = Join-Path $hipDir "bin\$d"
            $p2 = Join-Path $env:WINDIR "System32\$d"
            if ((Test-Path $p1) -or (Test-Path $p2)) {
                $where = if (Test-Path $p1) { 'bin' } else { 'System32' }
                Write-Host ("    [есть]  {0,-22} ({1})" -f $d, $where) -ForegroundColor Green
            } else {
                Write-Host ("    [НЕТ]   {0,-22} <- проверь, нужна ли" -f $d) -ForegroundColor Yellow
            }
        }
    }

    # ---------- 4. реальная попытка загрузки ----------
    Header '4. Реальная загрузка по полному пути'

    if (-not (Test-Path $venvPy)) {
        Warn2 "нет $venvPy - пропускаю"
    } else {
        $env:PATH = "$(Split-Path $rocTarget -Parent);$env:PATH"
        $probe = @'
import ctypes, os, sys, traceback
target = sys.argv[1]
print("файл:", target)
print("существует:", os.path.exists(target))
print("размер, MB: %.2f" % (os.path.getsize(target) / 1048576.0))
try:
    dll = ctypes.WinDLL(target)
    print("ЗАГРУЗИЛАСЬ")
    fn = getattr(dll, "rocblas_create_handle", None)
    print("символ rocblas_create_handle:", "есть" if fn else "НЕТ")
except OSError as e:
    print("НЕ ЗАГРУЗИЛАСЬ:", e)
    print("winerror:", getattr(e, "winerror", None))
    # 126 = ERROR_MOD_NOT_FOUND (нет зависимости), 193 = неверная архитектура
    code = getattr(e, "winerror", None)
    if code == 126:
        print("  -> не найдена зависимая DLL")
    elif code == 193:
        print("  -> файл не является корректной DLL для этой платформы")
    elif code == 1114:
        print("  -> DllMain вернул ошибку")
except Exception:
    traceback.print_exc()
'@
        $tmpPy = Join-Path $env:TEMP ('rocdep-' + [guid]::NewGuid().ToString('N').Substring(0,8) + '.py')
        [IO.File]::WriteAllText($tmpPy, $probe, (New-Object System.Text.UTF8Encoding($false)))
        try {
            & $venvPy $tmpPy $rocTarget
        } catch {
            Warn2 ("проба упала: " + $_.Exception.Message)
        } finally {
            Remove-Item $tmpPy -Force -ErrorAction SilentlyContinue
        }
    }
}

# ---------- итог ----------
Header 'ИТОГ'
Write-Host '  Как читать результат:' -ForegroundColor White
Info2 'winerror 126 в пункте 4 - не хватает зависимой DLL, смотри пункт 3.'
Info2 'Если в списке зависимостей есть hipblaslt.dll, а её нет -'
Info2 'rocBLAS 6.4 собрана с BUILD_WITH_HIPBLASLT, и на gfx803 это лишнее:'
Info2 'hipBLASLt поддерживает только CDNA/RDNA, для Polaris нужен вариант'
Info2 'rocBLAS без hipblaslt (как в 6.2.4).'
Write-Host ''
Info2 'Независимо от результата, ядра gfx803 всё равно нужны для GEMM:'
Info2 '  .\scripts\install\05-gfx803-rocblas.ps1 -Archive <архив> -HipVersion 6.4.2 -ListOnly'
Write-Host ''
exit 0
