<#
.SYNOPSIS
    Собирает диагностику стека ZLUDA + HIP SDK + PyTorch для RX 580 / gfx803.

.DESCRIPTION
    Пишет отчёт в preflight-report.txt рядом со скриптом (или в -OutFile).
    Ничего не меняет в системе: только читает и запускает hipInfo/torch на проверку.

.PARAMETER HipRoot
    Корень HIP SDK, по умолчанию C:\Program Files\AMD\ROCm

.PARAMETER Venv
    Путь к venv ComfyUI-Zluda. По умолчанию ищет .\venv рядом с репозиторием,
    затем .\ venv рядом с текущим каталогом.

.PARAMETER OutFile
    Куда писать отчёт.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\preflight.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\preflight.ps1 -HipRoot 'C:\Program Files\AMD\ROCm' -Venv 'C:\ComfyUI-Zluda\venv'
#>
[CmdletBinding()]
param(
    [string]$HipRoot = 'C:\Program Files\AMD\ROCm',
    [string]$Venv = '',
    [string]$OutFile = ''
)

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

if (-not $OutFile) {
    $OutFile = Join-Path $PSScriptRoot 'preflight-report.txt'
}

$lines = New-Object System.Collections.Generic.List[string]
function Add-Line([string]$text = '') { $script:lines.Add($text); Write-Host $text }
function Add-Section([string]$name) {
    Add-Line ''
    Add-Line ('=' * 78)
    Add-Line "== $name"
    Add-Line ('=' * 78)
}

# ---------------- helpers ----------------
function Get-FileVersionSafe([string]$path) {
    try {
        $vi = (Get-Item -LiteralPath $path -ErrorAction Stop).VersionInfo
        return "{0} (file {1})" -f $vi.ProductVersion, $vi.FileVersion
    } catch { return $null }
}

function Format-FileInfo([string]$path) {
    if (Test-Path -LiteralPath $path) { return (Get-FileVersionSafe $path) }
    return 'НЕТ'
}

function Test-Is64BitPe([string]$path) {
    try {
        $fs = [IO.File]::OpenRead($path)
        try {
            $br = New-Object IO.BinaryReader($fs)
            $fs.Position = 0x3C
            $peOffset = $br.ReadInt32()
            $fs.Position = $peOffset + 4
            $machine = $br.ReadUInt16()
            return [PSCustomObject]@{ Machine = ('0x{0:X4}' -f $machine); Is64 = ($machine -eq 0x8664) }
        } finally { $fs.Dispose() }
    } catch { return $null }
}

# ---------------- header ----------------
Add-Section 'ОТЧЁТ PREFLIGHT: ZLUDA / HIP SDK / PyTorch / gfx803'
Add-Line ("Дата:        {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))
Add-Line ("Компьютер:   {0}" -f $env:COMPUTERNAME)
Add-Line ("ОС:          {0} (build {1})" -f ([Environment]::OSVersion.VersionString), ([Environment]::OSVersion.Version.Build))
Add-Line ("PowerShell:  {0}" -f $PSVersionTable.PSVersion)
Add-Line ("Пользователь:{0}" -f $env:USERNAME)
Add-Line ("Admin:       {0}" -f ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))

# ---------------- GPU ----------------
Add-Section 'GPU (WMI)'
try {
    Get-CimInstance Win32_VideoController | ForEach-Object {
        Add-Line ("{0}" -f $_.Name)
        Add-Line ("   DriverVersion: {0}   DriverDate: {1}   Status: {2}" -f $_.DriverVersion, $_.DriverDate, $_.Status)
        Add-Line ("   PNPDeviceID:   {0}" -f $_.PNPDeviceID)
    }
} catch { Add-Line "ОШИБКА WMI: $_" }

# ---------------- HIP SDK ----------------
Add-Section "HIP SDK (просмотр $HipRoot)"
$hipVersions = @()
if (Test-Path -LiteralPath $HipRoot) {
    $hipVersions = Get-ChildItem -LiteralPath $HipRoot -Directory | Sort-Object Name
    foreach ($v in $hipVersions) {
        $amdhip = Join-Path $v.FullName 'bin\amdhip64_6.dll'
        $amdhip7 = Join-Path $v.FullName 'bin\amdhip64_7.dll'
        $comgr = Join-Path $v.FullName 'bin\amd_comgr.dll'
        Add-Line ("[{0}]" -f $v.Name)
        Add-Line ("   amdhip64_6.dll : {0}" -f (Format-FileInfo $amdhip))
        Add-Line ("   amdhip64_7.dll : {0}" -f (Format-FileInfo $amdhip7))
        Add-Line ("   amd_comgr.dll  : {0}" -f (Format-FileInfo $comgr))

        $rocblas = Join-Path $v.FullName 'bin\rocblas.dll'
        $rocblasLib = Join-Path $v.FullName 'bin\rocblas\library'
        Add-Line ("   rocblas.dll    : {0}" -f (Format-FileInfo $rocblas))
        if (Test-Path -LiteralPath $rocblasLib) {
            $gfx803 = Get-ChildItem -LiteralPath $rocblasLib -File | Where-Object { $_.Name -match 'gfx803' }
            $all = Get-ChildItem -LiteralPath $rocblasLib -File
            Add-Line ("   rocblas\library: файлов всего {0}, с gfx803 в имени: {1}" -f $all.Count, $gfx803.Count)
            if ($gfx803.Count -gt 0) {
                $gfx803 | ForEach-Object { Add-Line ("      {0}  ({1:N0} байт)" -f $_.Name, $_.Length) }
            } else {
                Add-Line '      !!! нет ни одного файла gfx803 - rocBLAS не сможет считать GEMM на Polaris'
                $archs = $all | ForEach-Object {
                    if ($_.Name -match '(gfx[0-9a-f]{4})') { $Matches[1] }
                } | Sort-Object -Unique
                if ($archs) { Add-Line ("      доступные архитектуры: {0}" -f ($archs -join ', ')) }
            }
        } else {
            Add-Line '   rocblas\library: НЕТ (нет каталога)'
        }

        $hipblaslt = Join-Path $v.FullName 'bin\hipblaslt.dll'
        $hipblasltLib = Join-Path $v.FullName 'bin\hipblaslt\library'
        Add-Line ("   hipblaslt.dll  : {0}" -f (Format-FileInfo $hipblaslt))
        if (Test-Path -LiteralPath $hipblasltLib) {
            $cnt = (Get-ChildItem -LiteralPath $hipblasltLib -File -Recurse -ErrorAction SilentlyContinue).Count
            Add-Line ("   hipblaslt\library: файлов {0}" -f $cnt)
        }

        $miopen = Join-Path $v.FullName 'bin\miopen.dll'
        Add-Line ("   miopen.dll     : {0}" -f (Format-FileInfo $miopen))
    }
} else {
    Add-Line "Каталог $HipRoot не существует."
}

Add-Section 'HIP: hipInfo.exe (по каждой версии)'
foreach ($v in $hipVersions) {
    $hipInfo = Join-Path $v.FullName 'bin\hipInfo.exe'
    if (Test-Path -LiteralPath $hipInfo) {
        Add-Line ("--- HIP {0}: {1}" -f $v.Name, $hipInfo)
        try {
            $env:AMD_LOG_LEVEL = '0'
            $out = & $hipInfo 2>&1 | Select-Object -First 60
            $out | ForEach-Object { Add-Line ("   {0}" -f $_) }
        } catch {
            Add-Line ("   ОШИБКА запуска: {0}" -f $_)
        }
    } else {
        Add-Line ("--- HIP {0}: hipInfo.exe отсутствует" -f $v.Name)
    }
}

# ---------------- PATH / DLL shadowing ----------------
Add-Section 'Переменные окружения (HIP/ROCM/HSA/CUDA/ZLUDA/MIOPEN/TORCH)'
Get-ChildItem Env: | Where-Object {
    $_.Name -match '^(HIP|ROCM|HSA|CUDA|ZLUDA|MIOPEN|AMD_|TORCH|PYTORCH|DISABLE_ADDMM)'
} | Sort-Object Name | ForEach-Object {
    Add-Line ("   {0} = {1}" -f $_.Name, $_.Value)
}

Add-Section 'PATH: записи, содержащие ROCm/HIP/ZLUDA'
$env:PATH -split ';' | Where-Object { $_ -match 'ROCm|HIP|ZLUDA|AMD' } | ForEach-Object { Add-Line ("   {0}" -f $_) }

Add-Section 'Где реально находятся ключевые DLL (where.exe)'
foreach ($dll in 'amdhip64_6.dll', 'amdhip64_7.dll', 'amd_comgr.dll', 'rocblas.dll', 'hipblaslt.dll', 'miopen.dll', 'cublas64_11.dll', 'cublasLt.dll', 'nvcuda.dll') {
    Add-Line ("--- {0}" -f $dll)
    try {
        $res = & where.exe $dll 2>$null
        if ($res) { $res | ForEach-Object { Add-Line ("   {0}" -f $_) } }
        else { Add-Line '   не найдено в PATH' }
    } catch { Add-Line '   where.exe недоступен' }
}

Add-Section 'System32: ключевые DLL (то, что грузится по умолчанию)'
foreach ($dll in 'amdhip64_6.dll', 'amdhip64_7.dll', 'amd_comgr.dll') {
    $p = Join-Path $env:SystemRoot "System32\$dll"
    Add-Line ("   {0}: {1}" -f $dll, (Format-FileInfo $p))
}

# ---------------- venv / torch ----------------
Add-Section 'venv и PyTorch'

if (-not $Venv) {
    $candidates = @(
        (Join-Path $PSScriptRoot '..\venv'),
        (Join-Path (Get-Location) 'venv')
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'Scripts\python.exe')) { $Venv = (Resolve-Path $c).Path; break }
    }
}

if ($Venv -and (Test-Path (Join-Path $Venv 'Scripts\python.exe'))) {
    $py = Join-Path $Venv 'Scripts\python.exe'
    Add-Line ("venv: {0}" -f $Venv)
    try {
        $pyInfo = & $py -c "import sys, platform; print(sys.version.replace(chr(10),' ')); print(platform.architecture()[0]); print(sys.executable)" 2>&1
        $pyInfo | ForEach-Object { Add-Line ("   {0}" -f $_) }
    } catch { Add-Line "   не удалось запустить python: $_" }

    try {
        $torchInfo = & $py -c "import torch; print('torch', torch.__version__); print('cuda', torch.version.cuda); print('hip', torch.version.hip); print('is_available', torch.cuda.is_available()); print('device_count', torch.cuda.device_count()); print('name', torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'n/a')" 2>&1
        Add-Line '--- torch:'
        $torchInfo | ForEach-Object { Add-Line ("   {0}" -f $_) }
    } catch { Add-Line "   torch не импортируется: $_" }

    $torchLib = Join-Path $Venv 'Lib\site-packages\torch\lib'
    if (Test-Path -LiteralPath $torchLib) {
        Add-Line ("--- torch\lib: {0}" -f $torchLib)
        foreach ($dll in 'cublas64_11.dll', 'cublasLt.dll', 'cusparse64_11.dll', 'cufft64_10.dll', 'cufftw64_10.dll', 'nvrtc64_112_0.dll', 'nvrtc_cuda.dll', 'cudnn64_9.dll', 'cudnn_ops_infer64_9.dll') {
            $p = Join-Path $torchLib $dll
            if (Test-Path $p) {
                $sz = '{0:N0}' -f (Get-Item $p).Length
                $bit = Test-Is64BitPe $p
                Add-Line ("   {0,-26} {1,12} байт  {2}  {3}" -f $dll, $sz, $bit.Machine, (Get-FileVersionSafe $p))
            } else {
                Add-Line ("   {0,-26} отсутствует" -f $dll)
            }
        }
        Add-Line '   (файлы ZLUDA в torch\lib обычно 3-6 МБ; если cublas64_11.dll ~100+ МБ - это настоящая NVIDIA-библиотека)'
    } else {
        Add-Line 'torch\lib не найден - venv не тот?'
    }

    $zludaDir = Join-Path (Split-Path $Venv -Parent) 'zluda'
    if (Test-Path -LiteralPath $zludaDir) {
        Add-Line ("--- zluda dir: {0}" -f $zludaDir)
        Get-ChildItem -LiteralPath $zludaDir -File | Sort-Object Name | ForEach-Object {
            Add-Line ("   {0,-24} {1,12:N0} байт" -f $_.Name, $_.Length)
        }
        $zludaExe = Join-Path $zludaDir 'zluda.exe'
        if (Test-Path $zludaExe) {
            try { ( & $zludaExe --version 2>&1 ) | ForEach-Object { Add-Line ("   version: {0}" -f $_) } } catch {}
        }
    } else {
        Add-Line ("--- zluda dir не найден рядом с venv: {0}" -f $zludaDir)
    }
} else {
    Add-Line 'venv не найден. Укажите -Venv "путь\к\ComfyUI-Zluda\venv"'
}

# ---------------- итог ----------------
Add-Section 'ИТОГ (быстрые выводы)'
$hip64 = $hipVersions | Where-Object { $_.Name -like '6.4*' } | Select-Object -First 1
if ($hip64) {
    $lib = Join-Path $hip64.FullName 'bin\rocblas\library'
    $hasGfx803 = $false
    if (Test-Path $lib) {
        $hasGfx803 = [bool](Get-ChildItem -LiteralPath $lib -File | Where-Object Name -match 'gfx803')
    }
    Add-Line ("HIP 6.4 найден: {0}" -f $hip64.FullName)
    $gfxNote = 'НЕТ  <-- главный блокер, см. docs/05-gfx803-rocblas.md'
    if ($hasGfx803) { $gfxNote = 'ЕСТЬ' }
    Add-Line ("Ядра gfx803 в rocblas\library: {0}" -f $gfxNote)
} else {
    Add-Line 'HIP 6.4 в указанном корне не найден.'
}
Add-Line ''
Add-Line 'Готово. Пришлите этот файл целиком.'

$lines -join "`r`n" | Set-Content -LiteralPath $OutFile -Encoding UTF8
Write-Host ''
Write-Host ("Отчёт сохранён: {0}" -f $OutFile) -ForegroundColor Green
