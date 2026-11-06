<#
.SYNOPSIS
    Этап 7 (диагностика): прямой опрос HIP SDK без torch и без ZLUDA.

.DESCRIPTION
    Отвечает на один вопрос: устройство не видит САМ HIP или его теряют
    ZLUDA/torch по дороге.

    Скрипт вызывает через P/Invoke ровно ту последовательность, которую
    выполняет ZLUDA при первом обращении к CUDA (zluda/src/impl/mod.rs):

        Comgr::find_and_load()      - загрузка comgr*.dll
        hipInit(0)
        hipGetDeviceCount(&count)
        далее по одному устройству: hipGetDeviceProperties

    Если здесь count = 0 с ошибкой 100 - виноват HIP/драйвер, и никакая
    подмена DLL в torch\lib этого не изменит. Если count > 0 - проблема
    дальше по цепочке (ZLUDA, подмена DLL, PATH), и её уже имеет смысл
    искать в этапе 6.

    Дополнительно проверяет, какие символы экспортирует amdhip64.dll:
      * hipGetDevicePropertiesR0600 - нужен сборке ZLUDA "windows-amd64"
      * hipGetDeviceProperties       - нужен сборке "windows-rocm5-amd64"
    Это ровно то, чем отличаются две сборки (hip_runtime-sys/src/lib.rs:
    макрос hipGetDeviceProperties раскрывается в R0600 только без feature
    "rocm5"). Если старая форма не экспортируется, rocm5-сборка ZLUDA
    не загрузится с WinError 127 - и "взять nvcuda от rocm5" не сработает.

    Ничего не меняет в системе. Только читает.

.PARAMETER HipVersion
    6.4.2 | 6.2.4. По умолчанию - все найденные версии по порядку.

.PARAMETER AllVersions
    Обойти все версии HIP SDK (старшая первой), а не только первую.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\07-hip-probe.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\07-hip-probe.ps1 -AllVersions
#>
[CmdletBinding()]
param(
    [string]$HipVersion = '',
    [switch]$AllVersions
)

$ErrorActionPreference = 'Continue'

$script:fails = 0
$script:warns = 0

function Header([string]$t) {
    Write-Host ''
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
}
function Ok([string]$what, [string]$note = '') {
    Write-Host ("[OK]   {0}" -f $what) -ForegroundColor Green
    if ($note) { Write-Host ("       $note") -ForegroundColor DarkGray }
}
function Warn([string]$what, [string]$note = '') {
    $script:warns++
    Write-Host ("[WARN] {0}" -f $what) -ForegroundColor Yellow
    if ($note) { Write-Host ("       $note") -ForegroundColor DarkGray }
}
function Fail([string]$what, [string]$note = '') {
    $script:fails++
    Write-Host ("[FAIL] {0}" -f $what) -ForegroundColor Red
    if ($note) { Write-Host ("       $note") -ForegroundColor DarkGray }
}
function Info([string]$t) {
    Write-Host "       $t" -ForegroundColor DarkGray
}

# ---------- типы для P/Invoke ----------
# Одним куском исходника (-TypeDefinition), а не -MemberDefinition:
# так делегаты объявлены на уровне namespace и не нужно ссылаться на
# вложенные типы через '+'. Плюс это одна компиляция вместо двух.
#
# Почему делегаты объявлены явно, а не через [Func[int,int]]:
# PowerShell не умеет передать в GetDelegateForFunctionPointer открытый
# generic-тип - падает с "The specified Type must not be a generic type
# definition". Сигнатуры приходится описывать самому.
if (-not ('HipProbe.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace HipProbe
{
    public static class Native
    {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
        public static extern IntPtr LoadLibraryA(string lpFileName);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
        public static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool FreeLibrary(IntPtr hModule);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
        public static extern uint GetModuleFileNameA(IntPtr hModule,
            System.Text.StringBuilder lpFilename, uint nSize);
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int HipInit(uint flags);

    // count передаём указателем: так не зависим от обработки out-параметров
    // в PowerShell.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int HipGetDeviceCount(IntPtr count);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int HipGetDeviceProperties(IntPtr prop, int deviceId);
}
'@ -ErrorAction Stop
}

# hipError_t имеет размер int; держим handle между итерациями
$hMod = [IntPtr]::Zero
# путь к amdhip64*.dll из HIP SDK - для сравнения с тем, что отдаёт голое имя
$script:firstHipDllPath = $null

function Get-HipErrorText([int]$code) {
    switch ($code) {
        0     { return 'hipSuccess' }
        100   { return 'hipErrorNoDevice (no ROCm-capable device is detected)' }
        101   { return 'hipErrorInvalidDevice' }
        201   { return 'hipErrorInvalidContext' }
        3     { return 'hipErrorNotInitialized' }
        35    { return 'hipErrorInsufficientDriver' }
        209   { return 'hipErrorNoBinaryForGpu' }
        default { return "hipError $code" }
    }
}

function Get-FirstHipDir([string]$root, [string]$want) {
    if (-not (Test-Path $root)) { return @() }
    $dirs = Get-ChildItem $root -Directory | Sort-Object Name -Descending
    if ($want) {
        $dirs = $dirs | Where-Object { $_.Name -like "$want*" }
    }
    return $dirs
}

Header 'ЭТАП 7: ПРЯМОЙ ОПРОС HIP (без torch и ZLUDA)'

# ---------- 0. Переменные, которые чаще всего дают ошибку 100 ----------
Header '0. Переменные окружения'

$visVars = @('HIP_VISIBLE_DEVICES', 'ROCR_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES', 'HSA_OVERRIDE_GFX_VERSION')
$anyVis = $false
foreach ($v in $visVars) {
    $val = [Environment]::GetEnvironmentVariable($v, 'Process')
    if ($null -eq $val -or $val -eq '') {
        $val = [Environment]::GetEnvironmentVariable($v, 'User')
        if ($null -eq $val -or $val -eq '') {
            $val = [Environment]::GetEnvironmentVariable($v, 'Machine')
        }
    }
    if ($null -ne $val -and $val -ne '') {
        $anyVis = $true
        Info ("{0} = {1}" -f $v, $val)
    }
}
if (-not $anyVis) {
    Ok 'Ни одна из HIP_VISIBLE_DEVICES / ROCR_VISIBLE_DEVICES / CUDA_VISIBLE_DEVICES не задана'
} else {
    Warn 'Заданы переменные видимости устройств' 'При одноголовой машине HIP_VISIBLE_DEVICES=1 даёт ровно ошибку 100. Должно быть 0 или пусто.'
}

# ---------- 1. Где лежит HIP ----------
# ---------- 0b. Копия HIP в System32 ----------
# Драйвер AMD кладёт свой amdhip64 в System32, и ИМЕННО он грузится первым,
# если приложение не положило свою копию рядом с exe. Версия в bin\ и версия
# в System32 могут расходиться: тогда HIP из SDK не находит устройств, хотя
# драйвер и OpenCL карту видят. Это ровно случай lemonade-sdk#3213.
Header '0b. Копия HIP в System32 (её грузит драйвер)'

$sys32 = Join-Path $env:SystemRoot 'System32'
$sysDlls = Get-ChildItem $sys32 -Filter 'amdhip64*.dll' -ErrorAction SilentlyContinue
if ($sysDlls -and $sysDlls.Count -gt 0) {
    foreach ($sd in $sysDlls) {
        $sv = $sd.VersionInfo.FileVersion
        if (-not $sv) { $sv = $sd.VersionInfo.ProductVersion }
        if (-not $sv) { $sv = '(версия не читается)' }
        Info ("{0} - {1}, {2:0.0} МБ" -f $sd.Name, $sv, ($sd.Length / 1MB))
    }
    $newest = ($sysDlls | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
    Info ("самый свежий по дате: {0} ({1:yyyy-MM-dd})" -f $newest.Name, $newest.LastWriteTime)
} else {
    Warn 'В System32 нет amdhip64*.dll' 'Значит HIP будет грузиться из PATH - из каталога HIP SDK. Так тоже бывает, но тогда версия SDK обязана совпадать с драйвером.'
}

Header '1. HIP SDK на диске'

$hipRoot = 'C:\Program Files\AMD\ROCm'
if (-not (Test-Path $hipRoot)) {
    Fail "Нет каталога $hipRoot" 'HIP SDK не установлен либо стоит в другое место. Укажи -HipVersion только если знаешь путь.'
    Write-Host ''
    Write-Host ("ИТОГ: FAIL {0}, WARN {1}" -f $script:fails, $script:warns) -ForegroundColor Red
    exit 1
}

$allDirs = Get-ChildItem $hipRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
if ($allDirs) {
    Info ("Установленные версии HIP: {0}" -f (($allDirs | Select-Object -ExpandProperty Name) -join ', '))
} else {
    Warn 'В каталоге HIP нет ни одной подпапки версии'
}

$cand = Get-FirstHipDir $hipRoot $HipVersion
if ($cand.Count -eq 0) {
    Fail "В $hipRoot нет каталога версии '$HipVersion'" 'Проверь список каталогов и повтори без -HipVersion.'
    exit 1
}
if (-not $AllVersions) {
    $cand = @($cand[0])
}

foreach ($d in $cand) {
    $binDir = Join-Path $d.FullName 'bin'
    Header ("2. Проверка HIP {0}" -f $d.Name)
    Info ("каталог: {0}" -f $d.FullName)

    $dllPath = Join-Path $binDir 'amdhip64.dll'
    if (-not (Test-Path $dllPath)) {
        # 6.x может называть DLL с суффиксом версии
        $alt = Get-ChildItem $binDir -Filter 'amdhip64*.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($alt) { $dllPath = $alt.FullName }
    }
    if (-not (Test-Path $dllPath)) {
        Fail "Нет amdhip64.dll в $binDir" 'Установка HIP SDK неполная: нужен компонент HIP SDK Core.'
        continue
    }
    Ok ("amdhip64.dll: {0}" -f $dllPath)
    if (-not $script:firstHipDllPath) { $script:firstHipDllPath = $dllPath }

    # ВАЖНО: PATH здесь НЕ трогаем.
    # Раньше тут bin\ и bin\rocblas\library добавлялись в PATH до вызова
    # hipInit - и это искажало картину: по голому имени amdhip64.dll начинал
    # находиться файл из SDK вместо драйверного. Для загрузки по полному пути
    # PATH не нужен вовсе (зависимости ищутся рядом с DLL и в System32).

    $verStr = (Get-Item $dllPath).VersionInfo.FileVersion
    if (-not $verStr) { $verStr = (Get-Item $dllPath).VersionInfo.ProductVersion }
    Info ("версия файла: {0}" -f $verStr)

    # ---------- загрузка ----------
    if ($hMod -ne [IntPtr]::Zero) {
        [void][HipProbe.Native]::FreeLibrary($hMod)
        $hMod = [IntPtr]::Zero
        $fnHipInit = $null
    }

    $hMod = [HipProbe.Native]::LoadLibraryA($dllPath)
    if ($hMod -eq [IntPtr]::Zero) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Fail ("LoadLibrary не смог загрузить amdhip64.dll (WinError {0})" -f $err) 'Обычно это отсутствующая зависимость. Проверь, что PATH содержит bin и bin\rocblas\library этой же версии.'
        continue
    }
    Ok 'amdhip64.dll загружена'

    # ---------- какие символы есть ----------
    $pR0600 = [HipProbe.Native]::GetProcAddress($hMod, 'hipGetDevicePropertiesR0600')
    $pOld = [HipProbe.Native]::GetProcAddress($hMod, 'hipGetDeviceProperties')
    $pCount = [HipProbe.Native]::GetProcAddress($hMod, 'hipGetDeviceCount')
    $pInit = [HipProbe.Native]::GetProcAddress($hMod, 'hipInit')

    if ($pInit -eq [IntPtr]::Zero) { Fail 'Нет экспорта hipInit' 'DLL загрузилась, но это не HIP runtime.'; continue }
    if ($pCount -eq [IntPtr]::Zero) { Fail 'Нет экспорта hipGetDeviceCount' ''; continue }

    if ($pR0600 -ne [IntPtr]::Zero) { Ok 'экспорт hipGetDevicePropertiesR0600 есть (сборка ZLUDA windows-amd64)' }
    else { Warn 'экспорта hipGetDevicePropertiesR0600 нет' 'Сборка ZLUDA windows-amd64 не сможет прочитать свойства устройства.' }

    if ($pOld -ne [IntPtr]::Zero) { Ok 'экспорт hipGetDeviceProperties есть (сборка ZLUDA windows-rocm5-amd64)' }
    else { Warn 'экспорта hipGetDeviceProperties (без R0600) нет' 'Сборка ZLUDA windows-rocm5-amd64 упадёт при загрузке с WinError 127. Взять nvcuda от rocm5 на этой HIP не получится.' }

    # ---------- hipInit ----------
    $fnHipInit = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pInit, [HipProbe.HipInit])
    $rc = -1
    try { $rc = $fnHipInit.Invoke([uint32]0) }
    catch { Fail ("hipInit: вызов упал - {0}" -f $_.Exception.Message); continue }
    if ($rc -eq 0) {
        Ok 'hipInit(0) = 0'
    } else {
        Fail ("hipInit(0) = {0} [{1}]" -f $rc, (Get-HipErrorText $rc))
        Info 'ВАЖНО: это не "hipGetDeviceCount не нашёл карту", а отказ инициализации.'
        Info 'Внутри hipInit вызывается hip::init() -> amd::Runtime::init() -> amd::Device::getDevices().'
        Info 'Ошибка 100 здесь означает: список amd::Device пуст, то есть ROCr не отдал ни одного агента.'
        Info 'Ни подмена nvcuda, ни свой rocBLAS этого не меняют - до них дело не доходит.'
        Write-Host ''
        Info 'Собираю лог HIP (AMD_LOG_LEVEL=3) - он покажет, на каком шаге отвалился агент:'

        $logFile = Join-Path $env:TEMP 'hip-probe-log.txt'
        $env:AMD_LOG_LEVEL = '3'
        $env:AMD_LOG_LEVEL_FILE = $logFile
        if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

        # дочерний процесс: свой hipInit уже отработал (call_once), в этом
        # процессе второй заход ничего нового не даст
        # Дочерний процесс должен быть python, а не powershell: 'import' -
        # это синтаксис python, в PowerShell такой команды нет.
        $pyExe = $null
        $pyCmd = Get-Command python -ErrorAction SilentlyContinue
        if ($pyCmd) { $pyExe = $pyCmd.Source }
        if (-not $pyExe) {
            $pyExe = "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe"
            if (-not (Test-Path $pyExe)) { $pyExe = $null }
        }
        if ($pyExe) {
            $pyFile = Join-Path $env:TEMP 'hip-probe-child.py'
            $pyBody = "import ctypes, sys`r`nh = ctypes.CDLL(r'" + $dllPath + "')`r`nsys.exit(h.hipInit(0))`r`n"
            Set-Content -Path $pyFile -Value $pyBody -Encoding ASCII
            $logOut = $null
            try { $logOut = & $pyExe $pyFile 2>&1 } catch { $logOut = @("$($_.Exception.Message)") }
            Info ("python: {0}, exit code {1}" -f $pyExe, $LASTEXITCODE)
            if ($logOut) { foreach ($ln in ($logOut | Select-Object -First 6)) { Info ("  $ln") } }
        } else {
            Info 'python не найден - дочерний процесс с логом пропущен.'
        }

        if (Test-Path $logFile) {
            $important = Select-String -Path $logFile -Pattern 'agent|Agent|NoDevice|hsa_|ROCr|ROCclr|unsupported|gfx|error|ERROR' -ErrorAction SilentlyContinue |
                         Select-Object -First 25
            if ($important) {
                Write-Host ''
                Info 'существенные строки лога:'
                foreach ($ln in $important) { Info ("  " + $ln.Line.Trim()) }
            }
            Info ("полный лог: {0}" -f $logFile)
        } else {
            Warn 'лог HIP не создался' 'Переменная AMD_LOG_LEVEL_FILE не поддерживается этой сборкой. Тогда: $env:AMD_LOG_LEVEL=3 и запусти hipInfo.exe вручную.'
        }
        Remove-Item Env:\AMD_LOG_LEVEL -ErrorAction SilentlyContinue
        Remove-Item Env:\AMD_LOG_LEVEL_FILE -ErrorAction SilentlyContinue
        continue
    }

    # ---------- hipGetDeviceCount ----------
    $fnCount = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pCount, [HipProbe.HipGetDeviceCount])
    $pCountPtr = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
    [Runtime.InteropServices.Marshal]::WriteInt32($pCountPtr, 0)
    $devCount = 0
    $rc = -1
    $callFailed = $false
    try {
        $rc = $fnCount.Invoke($pCountPtr)
        $devCount = [Runtime.InteropServices.Marshal]::ReadInt32($pCountPtr)
    } catch {
        $rc = -1
        $callFailed = $true
        Fail ("hipGetDeviceCount: вызов упал - {0}" -f $_.Exception.Message)
    }
    [Runtime.InteropServices.Marshal]::FreeHGlobal($pCountPtr)

    Write-Host ''
    if ($callFailed) {
        Info 'Вызов не состоялся - это ошибка скрипта или разрядности, не ответ HIP.'
    } elseif ($rc -ne 0) {
        Fail ("hipGetDeviceCount = {0} [{1}]" -f $rc, (Get-HipErrorText $rc))
        Info 'Это ТА ЖЕ ошибка, что в hipInfo.exe и в zluda/src/impl/mod.rs::init().'
        Info 'Значит устройство не доходит до HIP - виноват драйвер или окружение, а не torch и не подмена DLL.'
    } elseif ($devCount -le 0) {
        Fail ("hipGetDeviceCount = {0}, устройств = {1}" -f $rc, $devCount)
        Info 'HIP инициализировался, но список устройств пуст. Смотри пункт 0 (переменные видимости) и драйвер.'
    } else {
        Ok ("hipGetDeviceCount = 0, устройств = {0}" -f $devCount)
        Info 'Устройство видно на уровне HIP. Значит проблема НЕ в HIP, а дальше: ZLUDA, torch\lib, PATH.'
        Info 'В этом случае этап 6 должен проходить, а подмена nvcuda на rocm5 - не нужна.'
    }

    # ---------- свойства устройства ----------
    if ($rc -eq 0 -and $devCount -gt 0 -and $pR0600 -ne [IntPtr]::Zero) {
        # hipDeviceProp_t в 6.x большой (~1.5 КБ); берём с запасом и читаем только начало.
        # Первое поле структуры - char name[256], gcnArchName идёт дальше; смещения
        # между версиями менялись, поэтому имя читаем, а остальное не разбираем.
        $sz = 2048
        $propPtr = [Runtime.InteropServices.Marshal]::AllocHGlobal($sz)
        $fnProp = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pR0600, [HipProbe.HipGetDeviceProperties])
        $rc2 = -1
        try { $rc2 = $fnProp.Invoke($propPtr, 0) } catch { }
        if ($rc2 -eq 0) {
            $name = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($propPtr)
            if ($name) { Ok ("устройство 0: {0}" -f $name) }
            else { Warn 'структура свойств получена, но имя пустое' 'Раскладка hipDeviceProp_t не совпала с ожидаемой - не критично.' }
        } else {
            Warn ("hipGetDevicePropertiesR0600(0) = {0} [{1}]" -f $rc2, (Get-HipErrorText $rc2))
        }
        [Runtime.InteropServices.Marshal]::FreeHGlobal($propPtr)
    }

    # ---------- comgr ----------
    $comgr = Get-ChildItem $binDir -Filter 'comgr*.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($comgr) { Ok ("comgr: {0}" -f $comgr.Name) }
    else { Warn 'comgr*.dll не найден в bin' 'ZLUDA грузит его при init() первым делом и без него не стартует.' }

    # ---------- rocBLAS ----------
    $libDir = Join-Path $binDir 'rocblas\library'
    if (Test-Path $libDir) {
        $gfx = Get-ChildItem $libDir -Filter '*gfx803*' -ErrorAction SilentlyContinue
        $fb = Get-ChildItem $libDir -Filter '*fallback*' -ErrorAction SilentlyContinue
        if ($gfx.Count -gt 0) { Ok ("rocBLAS: {0} файлов gfx803" -f $gfx.Count) }
        else { Warn 'rocBLAS: файлов *gfx803* нет' 'Ядра для Polaris не установлены - выполни этап 5.' }
        if ($fb.Count -gt 0) { Ok ("rocBLAS: {0} fallback-файлов" -f $fb.Count) }
        elseif ($gfx.Count -gt 0) { Warn 'rocBLAS: gfx803 есть, fallback-файлов нет' 'Без *_fallback.dat rocBLAS не находит решения Tensile.' }
    } else {
        Warn ("нет каталога {0}" -f $libDir)
    }
}

if ($hMod -ne [IntPtr]::Zero) { [void][HipProbe.Native]::FreeLibrary($hMod) }

# ---------- 0c. Что загрузится по голому имени amdhip64 ----------
# ZLUDA линкуется на "dylib=amdhip64" - БЕЗ суффикса версии (build.rs, одинаково
# для сборок rocm5 и rocm6). А HIP SDK 6.x кладёт файл как amdhip64_6.dll
# (CMakeLists.txt: RUNTIME_OUTPUT_NAME "amdhip64_${HIP_VERSION_MAJOR}"), тогда
# как HIP 5.7 назывался просто amdhip64.dll.
#
# Отсюда важное: сборка ZLUDA windows-rocm5-amd64 грузит не файл из SDK, а то,
# что система отдаст по голому имени - обычно драйверную копию. Проверяем
# фактически, какой файл получится.
Header '0c. Что система отдаст по голому имени amdhip64'

$sdkDllForCmp = $null
$newestHip = Get-ChildItem 'C:\Program Files\AMD\ROCm' -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | Select-Object -First 1
if ($newestHip) {
    $f = Get-ChildItem (Join-Path $newestHip.FullName 'bin') -Filter 'amdhip64*.dll' -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($f) { $sdkDllForCmp = $f.FullName }
}

$bare = [HipProbe.Native]::LoadLibraryA('amdhip64.dll')
if ($bare -eq [IntPtr]::Zero) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Warn ("по голому имени amdhip64.dll не грузится (WinError {0})" -f $err) 'Тогда сборка ZLUDA windows-rocm5-amd64 не запустится вовсе.'
} else {
    $sb = New-Object System.Text.StringBuilder 520
    [void][HipProbe.Native]::GetModuleFileNameA($bare, $sb, 520)
    $barePath = $sb.ToString()
    Ok ("загружен: {0}" -f $barePath)

    $bareVer = (Get-Item $barePath -ErrorAction SilentlyContinue).VersionInfo.FileVersion
    Info ("версия: {0}" -f $bareVer)

    $bareDir = Split-Path $barePath -Parent
    if ($bareDir -like '*\System32' -or $bareDir -like '*\SysWOW64') {
        Info 'Это драйверная копия из System32, а не файл из HIP SDK.'
        Info 'Именно её и будет использовать сборка ZLUDA windows-rocm5-amd64.'
    } else {
        Info 'Файл не из System32 - подсунут через PATH или лежит рядом с процессом.'
    }

    if ($sdkDllForCmp -and (Test-Path $sdkDllForCmp)) {
        $av = (Get-Item $barePath).VersionInfo.FileVersion
        $bv = (Get-Item $sdkDllForCmp).VersionInfo.FileVersion
        if ($av -ne $bv) {
            Warn ("версии расходятся: голое имя {0} против bin {1}" -f $av, $bv) 'Это разные бинарники. Сборка rocm5 работает не с тем HIP, что лежит в SDK.'
        } else {
            Ok ("версия совпадает с файлом в HIP SDK ({0})" -f $av)
        }
    }

    $bp = [HipProbe.Native]::GetProcAddress($bare, 'hipInit')
    $bc = [HipProbe.Native]::GetProcAddress($bare, 'hipGetDeviceCount')
    if ($bp -ne [IntPtr]::Zero -and $bc -ne [IntPtr]::Zero) {
        $fInit = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($bp, [HipProbe.HipInit])
        $rcBare = -1
        try { $rcBare = $fInit.Invoke([uint32]0) } catch { }
        if ($rcBare -eq 0) {
            Ok 'hipInit(0) на этом модуле = 0'
            Info 'Путь через драйверную DLL рабочий: hipInit проходит.'
            Write-Host ''

            # Полная цепочка ZLUDA: hipInit -> hipGetDeviceCount -> свойства.
            # Именно её проходит zluda/src/impl/mod.rs::init() при старте.
            Info 'Полная цепочка ZLUDA на этом модуле:'
            $cntPtr = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
            [Runtime.InteropServices.Marshal]::WriteInt32($cntPtr, 0)
            $fCount = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($bc, [HipProbe.HipGetDeviceCount])
            $rcCnt = -1
            $nDev = 0
            try {
                $rcCnt = $fCount.Invoke($cntPtr)
                $nDev = [Runtime.InteropServices.Marshal]::ReadInt32($cntPtr)
            } catch { $rcCnt = -1 }
            [Runtime.InteropServices.Marshal]::FreeHGlobal($cntPtr)

            if ($rcCnt -eq 0 -and $nDev -gt 0) {
                Ok ("hipGetDeviceCount = 0, устройств = {0}" -f $nDev)
                Info 'Это ровно то, чего требует ZLUDA. Устройство видно ДРАЙВЕРНОЙ копией HIP.'
                Info 'Вывод: сборка ZLUDA windows-rocm5-amd64 - рабочий путь на этой машине.'

                # имя устройства через R0600, если он есть в драйверной копии
                $dp = [HipProbe.Native]::GetProcAddress($bare, 'hipGetDevicePropertiesR0600')
                if ($dp -eq [IntPtr]::Zero) {
                    $dp = [HipProbe.Native]::GetProcAddress($bare, 'hipGetDeviceProperties')
                }
                if ($dp -ne [IntPtr]::Zero) {
                    $pp = [Runtime.InteropServices.Marshal]::AllocHGlobal(2048)
                    $fProp2 = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($dp, [HipProbe.HipGetDeviceProperties])
                    $r2 = -1
                    try { $r2 = $fProp2.Invoke($pp, 0) } catch { }
                    if ($r2 -eq 0) {
                        $nm = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($pp)
                        if ($nm) { Ok ("устройство 0: {0}" -f $nm) }
                    } else {
                        Warn ("чтение свойств устройства = {0} [{1}]" -f $r2, (Get-HipErrorText $r2)) 'Не критично: torch берёт свойства через другие вызовы.'
                    }
                    [Runtime.InteropServices.Marshal]::FreeHGlobal($pp)
                }
            } elseif ($rcCnt -eq 0) {
                Fail ("hipGetDeviceCount = 0, но устройств = {0}" -f $nDev) 'hipInit прошёл, а список пуст. ZLUDA получит пустой набор устройств.'
            } else {
                Fail ("hipGetDeviceCount = {0} [{1}]" -f $rcCnt, (Get-HipErrorText $rcCnt)) 'hipInit прошёл, но перечисление устройств падает. ZLUDA не стартует.'
            }
        } else {
            Fail ("hipInit(0) на этом модуле = {0} [{1}]" -f $rcBare, (Get-HipErrorText $rcBare)) 'Если и здесь 100 - дело не в выборе сборки ZLUDA и не в имени файла, а в драйвере/ROCr.'
        }
    }
    [void][HipProbe.Native]::FreeLibrary($bare)
}

Write-Host ''
Write-Host ('-' * 70) -ForegroundColor DarkCyan
if ($script:fails -gt 0) {
    Write-Host ("ИТОГ: FAIL {0}, WARN {1}" -f $script:fails, $script:warns) -ForegroundColor Red
    exit 1
} else {
    Write-Host ("ИТОГ: FAIL {0}, WARN {1}" -f $script:fails, $script:warns) -ForegroundColor Green
    exit 0
}
