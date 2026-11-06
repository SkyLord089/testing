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

# hipError_t имеет размер int; держим handle и делегаты между итерациями
$hMod = [IntPtr]::Zero

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
Header '1. HIP SDK на диске'

$hipRoot = 'C:\Program Files\AMD\ROCm'
if (-not (Test-Path $hipRoot)) {
    Fail "Нет каталога $hipRoot" 'HIP SDK не установлен либо стоит в другое место. Укажи -HipVersion только если знаешь путь.'
    Write-Host ''
    Write-Host ("ИТОГ: FAIL {0}, WARN {1}" -f $script:fails, $script:warns) -ForegroundColor Red
    exit 1
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

    # PATH: библиотеке нужны соседние DLL (rocblas\library и т.п.)
    $env:PATH = "$binDir;$binDir\rocblas\library;$env:PATH"

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
        Fail ("hipInit(0) = {0} [{1}]" -f $rc, (Get-HipErrorText $rc)) 'HIP runtime не инициализируется. Дальше проверять бессмысленно.'
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

Write-Host ''
Write-Host ('-' * 70) -ForegroundColor DarkCyan
if ($script:fails -gt 0) {
    Write-Host ("ИТОГ: FAIL {0}, WARN {1}" -f $script:fails, $script:warns) -ForegroundColor Red
    exit 1
} else {
    Write-Host ("ИТОГ: FAIL {0}, WARN {1}" -f $script:fails, $script:warns) -ForegroundColor Green
    exit 0
}
