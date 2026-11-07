<#
.SYNOPSIS
    Этап 6: сквозная проверка уже установленной системы (без запуска ComfyUI).

.DESCRIPTION
    Проверяет по порядку:
      1. HIP SDK: каталог, hipInfo -> gfx803 / RX 580
      2. rocBLAS: наличие файлов *gfx803* в library
      3. venv + torch 2.7.0+cu118
      4. torch\lib: какие DLL подменены на ZLUDA, какие остались оригинальными
      5. comfy/zluda.py актуален
      6. torch.cuda: доступность устройства и его имя ([ZLUDA])
      7. GEMM-тест (rocBLAS): маленький matmul, сверка с CPU
      8. addmm с bias (cublasLt-проба): должен проходить при DISABLE_ADDMM_CUDA_LT=1

    Ничего не меняет. Exit code 0, если нет FAIL.

.PARAMETER InstallDir
    Папка ComfyUI-Zluda.

.PARAMETER HipVersion
    6.4.2 | 6.2.4 (для поиска каталога HIP SDK). По умолчанию определяется автоматически.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\06-verify.ps1
#>
[CmdletBinding()]
param(
    [string]$InstallDir = '',
    [string]$HipVersion = '',
    [switch]$SkipGemm
)

$ErrorActionPreference = 'Continue'

# по умолчанию - рядом с китом
if (-not $InstallDir) {
    $kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $InstallDir = Join-Path $kitRoot 'ComfyUI-Zluda'
}

$script:fails = 0
$script:warns = 0

# HIP SDK должен быть в PATH: ZLUDA-шимы линкуются на amdhip64.dll
if ($hipRoot -eq $null) { $hipRoot = 'C:\Program Files\AMD\ROCm' }
$hipBinCandidates = @()
if (Test-Path $hipRoot) {
    foreach ($d in Get-ChildItem $hipRoot -Directory | Sort-Object Name -Descending) {
        $hipBinCandidates += (Join-Path $d.FullName 'bin')
    }
}
# ВАЖНО: bin HIP SDK добавляется в КОНЕЦ PATH.
#   * нужен: ZLUDA при init() ищет amd_comgr_3.dll / _2 / amd_comgr.dll по
#     голым именам, а в System32 их нет. Без bin ZLUDA не стартует;
#   * безопасен: в bin лежит amdhip64_6.dll, а линковка идёт на голое имя
#     amdhip64.dll - его в bin нет, поэтому Windows берёт драйверную копию
#     из System32 (она и видит RX 580).
foreach ($hb in $hipBinCandidates) { if (Test-Path $hb) { $env:PATH = "$env:PATH;$hb;$hb\rocblas\library" } }

function Header([string]$t) {
    Write-Host ''
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
}
function Ok([string]$what, [string]$note = '') {
    Write-Host ("[PASS] {0}" -f $what) -ForegroundColor Green
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

function Info2([string]$t) {
    Write-Host ("       {0}" -f $t) -ForegroundColor DarkGray
}

# Прямой опрос HIP по указанному файлу DLL: hipInit -> hipGetDeviceCount ->
# имя устройства. Возвращает имя устройства или $null.
# Нужен потому, что hipInfo.exe из SDK грузит не ту DLL (см. раздел 1).
function Test-HipDevice([string]$dllPath) {
    if (-not ('HipVerify.Native' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace HipVerify
{
    public static class Native
    {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
        public static extern IntPtr LoadLibraryA(string p);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
        public static extern IntPtr GetProcAddress(IntPtr h, string n);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool FreeLibrary(IntPtr h);
    }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int HipInit(uint flags);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int HipGetDeviceCount(IntPtr count);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int HipGetDeviceProperties(IntPtr prop, int id);
}
'@ -ErrorAction Stop
    }
    $h = [HipVerify.Native]::LoadLibraryA($dllPath)
    if ($h -eq [IntPtr]::Zero) { return $null }
    try {
        $pInit = [HipVerify.Native]::GetProcAddress($h, 'hipInit')
        $pCnt = [HipVerify.Native]::GetProcAddress($h, 'hipGetDeviceCount')
        if ($pInit -eq [IntPtr]::Zero -or $pCnt -eq [IntPtr]::Zero) { return $null }
        $fInit = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pInit, [HipVerify.HipInit])
        if ($fInit.Invoke([uint32]0) -ne 0) { return $null }
        $fCnt = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pCnt, [HipVerify.HipGetDeviceCount])
        $ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
        [Runtime.InteropServices.Marshal]::WriteInt32($ptr, 0)
        $rc = $fCnt.Invoke($ptr)
        $n = [Runtime.InteropServices.Marshal]::ReadInt32($ptr)
        [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
        if ($rc -ne 0 -or $n -le 0) { return $null }
        $pProp = [HipVerify.Native]::GetProcAddress($h, 'hipGetDevicePropertiesR0600')
        if ($pProp -eq [IntPtr]::Zero) { $pProp = [HipVerify.Native]::GetProcAddress($h, 'hipGetDeviceProperties') }
        if ($pProp -eq [IntPtr]::Zero) { return "устройств: $n" }
        $fProp = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($pProp, [HipVerify.HipGetDeviceProperties])
        $pp = [Runtime.InteropServices.Marshal]::AllocHGlobal(2048)
        $r2 = $fProp.Invoke($pp, 0)
        $nm = if ($r2 -eq 0) { [Runtime.InteropServices.Marshal]::PtrToStringAnsi($pp) } else { "" }
        [Runtime.InteropServices.Marshal]::FreeHGlobal($pp)
        if ($nm) { return $nm }
        return "устройств: $n"
    } catch {
        return $null
    } finally {
        [void][HipVerify.Native]::FreeLibrary($h)
    }
}

Header 'ЭТАП 6: ПРОВЕРКА УСТАНОВКИ'

# ---------- 1. HIP ----------
Header '1. HIP SDK'
$hipDir = $null
if (Test-Path $hipRoot) {
    $vers = Get-ChildItem $hipRoot -Directory | Sort-Object Name -Descending
    if ($HipVersion) {
        $hit = $vers | Where-Object { $_.Name -like "$HipVersion*" -or $_.Name -eq ($HipVersion -replace '\.\d+$', '') } | Select-Object -First 1
        if ($hit) { $hipDir = $hit.FullName }
    } else {
        # приоритет: 6.4, потом 6.2, потом всё остальное
        foreach ($pref in '6.4', '6.2') {
            $hit = $vers | Where-Object Name -like "$pref*" | Select-Object -First 1
            if ($hit) { $hipDir = $hit.FullName; break }
        }
        if (-not $hipDir -and $vers) { $hipDir = $vers[0].FullName }
    }
    Write-Host ("  Найдено в {0}: {1}" -f $hipRoot, (($vers | Select-Object -ExpandProperty Name) -join ', ')) -ForegroundColor DarkGray
}

if ($hipDir) {
    Ok "HIP SDK каталог: $hipDir"
    # ВАЖНО: hipInfo.exe из bin\ SDK на этой конфигурации ВСЕГДА скажет
    # "no ROCm-capable device" - он грузит amdhip64_6.dll из bin, а та карту
    # не перечисляет (hipInit = 100). Рабочий путь другой: голое имя
    # amdhip64.dll резолвится в драйверную копию из System32, и вот она
    # устройство видит (hipInit = 0, "Radeon RX 580 Series").
    #
    # Поэтому hipInfo проверяем информационно, а НАСТОЯЩИЙ критерий - прямой
    # опрос драйверной DLL, как это делает ZLUDA. Подробности: этап 7.
    $verFile = Join-Path $hipDir 'bin\hipInfo.exe'
    if (Test-Path $verFile) {
        $out = & $verFile 2>&1 | Out-String
        if ($out -match 'no ROCm-capable device') {
            Info2 'hipInfo из SDK: "no ROCm-capable device" - это ОЖИДАЕМО на этой сборке'
            Info2 'Он грузит amdhip64_6.dll из bin, а она карту не видит. Проверяем драйверную копию.'
        } elseif ($out -match 'gfx803') {
            Ok 'hipInfo (SDK): gfx803 обнаружен'
        }
        if ($out -match 'RX\s*580') { Ok 'hipInfo (SDK): Radeon RX 580 в списке' }
    } else {
        Warn 'hipInfo.exe не найден' 'HIP SDK установлен неполно (без Core) - переустанови с компонентом HIP SDK Core'
    }

    # ---- реальная проверка: драйверная amdhip64.dll из System32 ----
    $sysHip = Join-Path $env:SystemRoot 'System32\amdhip64.dll'
    if (Test-Path $sysHip) {
        $sysVer = (Get-Item $sysHip).VersionInfo.FileVersion
        Info2 ("драйверная копия: {0} ({1})" -f $sysHip, $sysVer)
        $gpuName = Test-HipDevice $sysHip
        if ($gpuName) {
            Ok ("HIP видит устройство: {0}" -f $gpuName)
        } else {
            Fail 'HIP не видит устройство даже через драйверную DLL' 'Ни ZLUDA, ни torch не заработают. Этап 7 покажет подробности.'
        }
    } else {
        Fail ("нет {0}" -f $sysHip) 'Драйвер AMD не установил HIP runtime. Этап 2: обнови драйвер.'
    }
} else {
    Fail 'HIP SDK не найден в C:\Program Files\AMD\ROCm' 'Этап 3: scripts\install\03-install-hip.ps1'
}

# ---------- 2. rocBLAS ----------
Header '2. Ядра rocBLAS для gfx803'
if ($hipDir) {
    $libDir = Join-Path $hipDir 'bin\rocblas\library'
    if (Test-Path $libDir) {
        $gfx = @(Get-ChildItem $libDir -File | Where-Object Name -match 'gfx803')
        if ($gfx.Count -gt 0) {
            Ok ("Файлов gfx803: {0}" -f $gfx.Count) (($gfx | Select-Object -First 5 -ExpandProperty Name) -join ', ')
        } else {
            Fail 'В rocblas\library нет файлов gfx803' 'Этап 5: 05-gfx803-rocblas.ps1 -Archive <архив> -Clean'
        }
        $archCount = @(Get-ChildItem $libDir -File | ForEach-Object { if ($_.Name -match '(gfx[0-9a-f]{4})') { $matches[1] } } | Sort-Object -Unique)
        Write-Host ("  Архитектуры в library: {0}" -f ($archCount -join ', ')) -ForegroundColor DarkGray
    } else {
        Fail "Нет каталога $libDir" 'rocBLAS не установлен? Проверь компоненты HIP SDK.'
    }
}

# ---------- 3. venv + torch ----------
Header '3. venv и torch'
$venvPy = Join-Path $InstallDir 'venv\Scripts\python.exe'
if (-not (Test-Path $venvPy)) {
    Fail "Нет $venvPy" 'Этап 4: 04-comfyui-zluda.ps1'
    Write-Host ''
    Write-Host ("ИТОГ: FAIL {0}, WARN {1}" -f $script:fails, $script:warns) -ForegroundColor Red
    exit 1
}
# запуск через zluda.exe, если он есть: так же, как в comfyui-user.bat
$zludaExe = Join-Path $InstallDir 'zluda\zluda.exe'
$script:runnerExe = $venvPy
$script:runnerPrefix = @()
if (Test-Path $zludaExe) {
    $script:runnerExe = $zludaExe
    $script:runnerPrefix = @('--', $venvPy)
    Write-Host "  Runner: zluda.exe (инжектор)" -ForegroundColor DarkGray
} else {
    Write-Host "  Runner: python напрямую (zluda.exe не найден)" -ForegroundColor DarkYellow
}

$tv = & $venvPy -c "import torch;print(torch.__version__ + '|' + str(torch.version.cuda))" 2>&1
if ($tv -match '^2\.7\.\d+\+cu118\|11\.8') {
    Ok "torch $tv"
} else {
    Warn "torch: $tv" 'Ожидалось 2.7.x+cu118 / cuda 11.8. Другие версии тоже могут работать, но матрица проверена на 2.7.'
}

# ---------- 4. DLL ----------
Header '4. DLL в torch\lib'
$torchLib = Join-Path $InstallDir 'venv\Lib\site-packages\torch\lib'
if (Test-Path $torchLib) {
    $zludaMap = @{
        'cublas64_11.dll'    = 15
        'cusparse64_11.dll'  = 15
        'cufft64_10.dll'     = 15
        'cufftw64_10.dll'    = 15
        'nvrtc64_112_0.dll'  = 15
    }
    foreach ($n in $zludaMap.Keys) {
        $p = Join-Path $torchLib $n
        if (-not (Test-Path $p)) { Fail "$n отсутствует" 'Перезапусти 04-comfyui-zluda.ps1'; continue }
        $mb = [math]::Round((Get-Item $p).Length / 1MB, 1)
        if ($mb -le $zludaMap[$n]) { Ok ("{0}: {1} МБ - ZLUDA" -f $n, $mb) }
        else { Fail ("{0}: {1} МБ - ОРИГИНАЛ, не подменён" -f $n, $mb) 'Перезапусти 04-comfyui-zluda.ps1 (или patch-torch-zluda.ps1)' }
    }
    $lt = @(Get-ChildItem $torchLib -File -Filter '*cublasLt*')
    if ($lt.Count -eq 0) {
        Ok 'cublasLt: отсутствует - ок для gfx803'
    } else {
        $bigLt = @($lt | Where-Object { $_.Length -gt 5MB })
        if ($bigLt.Count -eq $lt.Count) { Ok ('cublasLt: только оригинал ({0} шт.)' -f $lt.Count) 'Так и должно быть: ZLUDA-cublasLt на gfx803 ломает addmm' }
        else { Fail 'В torch\lib лежит ZLUDA-cublasLt' 'Удали файлы *cublasLt*.dll размером < 5 МБ и перезапусти проверку' }
    }
    $nc = Join-Path $torchLib 'nvrtc_cuda.dll'
    if (Test-Path $nc) {
        $mb = [math]::Round((Get-Item $nc).Length / 1MB, 1)
        if ($mb -ge 5) { Ok ("nvrtc_cuda.dll: {0} МБ - оригинал" -f $mb) } else { Warn ("nvrtc_cuda.dll: {0} МБ - похоже на ZLUDA-шим" -f $mb) 'Ожидалась копия оригинального nvrtc. Перезапусти patch-torch-zluda.ps1 после восстановления.' }
    } else {
        Warn 'nvrtc_cuda.dll отсутствует' 'Не критично, если torch не грузит его; иначе создай копию оригинального nvrtc64_112_0.dll'
    }
} else {
    Fail "Нет $torchLib" 'torch не установлен в venv'
}

# ---------- 4b. зависимости самого ZLUDA ----------
# ZLUDA при init() делает Comgr::find_and_load() и ищет по ГОЛЫМ именам:
#   amd_comgr_3.dll -> amd_comgr_2.dll -> amd_comgr.dll
# В System32 их нет, поэтому без bin HIP SDK в PATH ZLUDA не стартует, и
# torch.cuda.is_available() вернёт False без внятной причины.
Header '4b. Зависимости ZLUDA (comgr и PATH)'

$comgrFound = $false
foreach ($n in 'amd_comgr_3.dll', 'amd_comgr_2.dll', 'amd_comgr.dll') {
    $hit = $null
    foreach ($hb in $hipBinCandidates) {
        $cand = Join-Path $hb $n
        if (Test-Path $cand) { $hit = $cand; break }
    }
    if ($hit) {
        Ok ("comgr: найден {0} ({1})" -f $n, $hit)
        $comgrFound = $true
        break
    }
}
if (-not $comgrFound) {
    Fail 'comgr не найден (ни amd_comgr_3, ни _2, ни без суффикса)' 'ZLUDA не запустится. Нужен компонент HIP SDK Core; проверь bin.'
}

$pathHasBin = @($env:PATH -split ';' | Where-Object { $_ -match 'ROCm' })
if ($pathHasBin.Count -gt 0) {
    Ok ("PATH содержит HIP: {0}" -f (($pathHasBin | Select-Object -First 3) -join ', '))
} else {
    Fail 'В PATH нет каталога HIP SDK' 'ZLUDA не найдёт comgr. Лаунчер comfyui-user.bat добавляет его сам.'
}

$sysHipCheck = Join-Path $env:SystemRoot 'System32\amdhip64.dll'
if (Test-Path $sysHipCheck) {
    Ok ("драйверная amdhip64.dll на месте ({0})" -f (Get-Item $sysHipCheck).VersionInfo.FileVersion)
}

# ---------- 5. comfy/zluda.py ----------
Header '5. comfy\zluda.py'
$czDst = Join-Path $InstallDir 'comfy\zluda.py'
$czSrc = Join-Path $InstallDir 'comfy\customzluda\zluda.py'
if ((Test-Path $czDst) -and (Test-Path $czSrc)) {
    if ((Get-FileHash $czDst).Hash -eq (Get-FileHash $czSrc).Hash) {
        Ok 'zluda.py совпадает с customzluda\zluda.py'
    } else {
        Warn 'zluda.py отличается от customzluda\zluda.py' 'Обычно нужен вариант из customzluda. Скопируй его поверх (04 делает это автоматически).'
    }
} elseif (Test-Path $czDst) {
    Ok 'comfy\zluda.py на месте'
} else {
    Fail 'Нет comfy\zluda.py' 'Без него ComfyUI не сделает ZLUDA-настройки. Скопируй comfy\customzluda\zluda.py -> comfy\zluda.py'
}

# ---------- 6. torch.cuda ----------
Header '6. torch.cuda под ZLUDA'
$probe = @'
import os, sys
os.environ.setdefault("DISABLE_ADDMM_CUDA_LT", "1")
import torch
avail = torch.cuda.is_available()
name = ""
if avail:
    try:
        name = torch.cuda.get_device_name(0)
    except Exception as e:
        name = "ERR:" + str(e)
print("AVAIL=%s" % avail)
print("NAME=%s" % name)
if avail:
    try:
        p = torch.cuda.get_device_properties(0)
        print("MEM_MB=%d" % (p.total_memory // (1024*1024)))
        print("CC=%d.%d" % (p.major, p.minor))
    except Exception as e:
        print("PROPS_ERR=%s" % e)
'@
$probeFile = Join-Path $env:TEMP 'zluda-probe.py'
# без BOM: Windows PowerShell 5.1 пишет BOM, а python это переживёт, но лишние байты в stderr не нужны
[IO.File]::WriteAllText($probeFile, $probe, (New-Object System.Text.UTF8Encoding($false)))
$env:ZLUDA_COMGR_LOG_LEVEL = '0'
$runArgs = $script:runnerPrefix + $probeFile
$probeOut = & $script:runnerExe @runArgs 2>&1 | Out-String
Write-Host $probeOut.Trim() -ForegroundColor DarkGray

if ($probeOut -match 'AVAIL=True') {
    Ok 'torch.cuda.is_available() = True'
    if ($probeOut -match '\[ZLUDA\]') { Ok 'Имя устройства содержит [ZLUDA]' }
    else { Warn 'В имени устройства нет [ZLUDA]' 'Значит, torch грузит не ZLUDA-шим, либо это настоящая NVIDIA. Проверь PATH и torch\lib.' }
} else {
    Fail 'torch.cuda.is_available() = False' 'ZLUDA не подключился: проверь torch\lib (п.4), PATH без ROCm (comfy\zluda.py), запуск через zluda\zluda.exe'
}

# ---------- 7-8. GEMM и addmm ----------
if (-not $SkipGemm) {
    Header '7. GEMM (rocBLAS) и addmm с bias (cublasLt)'
    $gemm = @'
import os
os.environ["DISABLE_ADDMM_CUDA_LT"] = "1"
import torch
if not torch.cuda.is_available():
    print("SKIP: cuda недоступна")
    raise SystemExit(0)
dev = "cuda"
torch.manual_seed(0)
def rel(a, b):
    return (a - b).abs().max().item() / max(1e-9, b.abs().max().item())
# 1) mm fp32
a = torch.randn(512, 512); b = torch.randn(512, 512)
c = (a.to(dev) @ b.to(dev)).cpu()
print("MM_FP32_ERR=%.3e" % rel(c, a @ b))
# 2) mm fp16
ah = a.half().to(dev); bh = b.half().to(dev)
ch = (ah @ bh).float().cpu()
print("MM_FP16_ERR=%.3e" % rel(ch, (a @ b)))
# 3) addmm с bias - ровно та ветка, что сломана в torch>=2.4 на Windows
x = torch.randn(512, 512)
w = torch.randn(512, 512)
bias = torch.randn(512)
y = torch.addmm(bias.to(dev), x.to(dev), w.to(dev)).cpu()
expected = torch.addmm(bias, x, w)
print("ADDMM_ERR=%.3e" % rel(y, expected))
print("OK")
'@
    $gemmFile = Join-Path $env:TEMP 'zluda-gemm.py'
    [IO.File]::WriteAllText($gemmFile, $gemm, (New-Object System.Text.UTF8Encoding($false)))
    $runArgs = $script:runnerPrefix + $gemmFile
    $gemmOut = & $script:runnerExe @runArgs 2>&1 | Out-String
    Write-Host $gemmOut.Trim() -ForegroundColor DarkGray
    if ($gemmOut -match 'MM_FP32_ERR=([\d.e+-]+)') {
        $e = [double]$matches[1]
        if ($e -lt 1e-4) { Ok ("mm fp32: относительная ошибка {0:E2}" -f $e) } else { Fail ("mm fp32: ошибка {0:E2} - мусор вместо вычислений" -f $e) 'Ядра gfx803 не подходят к версии rocBLAS. Этап 5 с -Clean, либо fallback на HIP 6.2.4.' }
    } elseif ($gemmOut -match 'SKIP') {
        Warn 'GEMM пропущен: нет CUDA-устройства' ''
    } else {
        Fail 'mm fp32: тест не выполнился' 'Смотри текст ошибки выше (TensileLibrary / rocBLAS)'
    }
    if ($gemmOut -match 'MM_FP16_ERR=([\d.e+-]+)') {
        $e = [double]$matches[1]
        if ($e -lt 1e-2) { Ok ("mm fp16: относительная ошибка {0:E2}" -f $e) } else { Warn ("mm fp16: ошибка {0:E2}" -f $e) 'На gfx803 fp16 идёт через fp32; большая ошибка допустима, если картинки верные.' }
    }
    if ($gemmOut -match 'ADDMM_ERR=([\d.e+-]+)') {
        $e = [double]$matches[1]
        if ($e -lt 1e-4) { Ok ("addmm c bias: ok ({0:E2}) - cublasLt обойдён" -f $e) } else { Fail ("addmm c bias: ошибка {0:E2}" -f $e) 'Скорее всего, DISABLE_ADDMM_CUDA_LT не применён: проверь, что переменная задана ДО импорта torch.' }
    } elseif ($gemmOut -match 'CUBLAS_STATUS_NOT_SUPPORTED') {
        Fail 'addmm: CUBLAS_STATUS_NOT_SUPPORTED' 'cublasLt-путь. Убедись, что ZLUDA-cublasLt не лежит в torch\lib и DISABLE_ADDMM_CUDA_LT=1.'
    }
}

# ---------- итог ----------
Header 'ИТОГ'
if ($script:fails -gt 0) {
    Write-Host ("FAIL: {0}   WARN: {1}" -f $script:fails, $script:warns) -ForegroundColor Red
    Write-Host ''
    Write-Host 'Что делать по каждому FAIL - в docs\01-diagnosis.md и docs\05-gfx803-rocblas.md.' -ForegroundColor Yellow
    exit 1
}
Write-Host ("FAIL: 0   WARN: {0}" -f $script:warns) -ForegroundColor Green
Write-Host ''
Write-Host 'Всё критичное на месте. Запуск ComfyUI:' -ForegroundColor Green
Write-Host "  cd $InstallDir" -ForegroundColor Green
Write-Host '  .\comfyui-user.bat' -ForegroundColor Green
Write-Host ''
Write-Host 'Первый запуск компилирует ядра - 10-20 минут тишины это нормально.' -ForegroundColor DarkGray
exit 0
