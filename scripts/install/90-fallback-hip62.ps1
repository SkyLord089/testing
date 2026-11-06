<#
.SYNOPSIS
    План Б: перевести уже установленную систему с HIP 6.4 на 6.2.4 (где ядра gfx803 родные).

.DESCRIPTION
    Когда на этапе 5 с HIP 6.4 не удалось подружить Tensile-ядра с Polaris -
    не тратим дни на сборку rocBLAS, а быстро получаем рабочую систему:

      1. грубо проверяет, что установлено;
      2. снимает HIP 6.4 (через установщик -uninstall);
      3. ставит HIP 6.2.4;
      4. ставит ZLUDA 3.9.5 rocm6 и перепатчивает DLL;
      5. ставит ядра gfx803 (родные 6.2.4);
      6. проверяет.

    torch остаётся 2.7.0+cu118 - это важно, цель "2.7" сохраняется.

.PARAMETER InstallDir
    ComfyUI-Zluda.

.PARAMETER RocblasArchive
    Архив с ядрами gfx803 под 6.2.4 (по умолчанию подскажет ссылку).

.PARAMETER KeepHip64
    Не удалять 6.4 (останется в системе; но PATH должен указывать только на 6.2!).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\90-fallback-hip62.ps1
#>
[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\AI\ComfyUI-Zluda',
    [string]$RocblasArchive = '',
    [switch]$KeepHip64,
    [switch]$NonInteractive
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $here '..\..')).Path

function Banner([string]$t) {
    Write-Host ''
    Write-Host ('═' * 70) -ForegroundColor Magenta
    Write-Host "  $t" -ForegroundColor Magenta
    Write-Host ('═' * 70) -ForegroundColor Magenta
}
function Pause-Here([string]$what) {
    if ($NonInteractive) { return }
    Write-Host ''
    Write-Host "  >> $what" -ForegroundColor Yellow
    Read-Host '    Нажми Enter, когда готово'
}

Banner 'ПЛАН Б: переход на HIP SDK 6.2.4 (ядра gfx803 родные)'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw 'Нужны права администратора.' }

Write-Host @"
  Почему это работает:
    * HIP 6.2.4 - последняя версия, для которой существуют ГОТОВЫЕ
      Tensile-ядра gfx803 (и официальные community-сборки).
    * ZLUDA 3.9.5 (rocm6) официально собирается под 6.2.
    * torch 2.7.0+cu118 остаётся - цель по torch не меняется.
    * DISABLE_ADDMM_CUDA_LT=1 всё так же обязателен (cublasLt-путь в torch 2.7).

  Что теряем: саму "шестёрку-четыре" (HIP 6.4). Если позже появится
  рабочий набор ядер под 6.4 - вернёмся (05 + 04 -Profile target).
"@ -ForegroundColor Gray

# ---- проверки ----
Banner 'Текущее состояние'
$hip62 = 'C:\Program Files\AMD\ROCm\6.2'
$hip64 = 'C:\Program Files\AMD\ROCm\6.4'
Write-Host ("  HIP 6.2: {0}" -f (Test-Path $hip62))
Write-Host ("  HIP 6.4: {0}" -f (Test-Path $hip64))
Write-Host ("  ComfyUI: {0}" -f (Test-Path (Join-Path $InstallDir 'main.py')))

if (-not $NonInteractive) {
    $ok = Read-Host '  Начинаем? Это удалит HIP 6.4 (если не указан -KeepHip64). (y/n)'
    if ($ok -notmatch '^[yYдД]') { return }
}

# ---- удаление 6.4 ----
if ((Test-Path $hip64) -and -not $KeepHip64) {
    Banner 'Удаление HIP 6.4'
    $uninstaller = Get-ChildItem 'C:\Program Files\AMD\ROCm\6.4' -Filter '*uninstall*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $uninstaller) {
        # ищем через установленные программы
        $app = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
                                'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue |
               Where-Object { $_.DisplayName -match 'HIP SDK' } | Select-Object -First 1
        if ($app -and $app.UninstallString) {
            Write-Host "  Найден деинсталлятор: $($app.UninstallString)" -ForegroundColor White
            Write-Host '  Запусти его вручную (Programs and Features) и вернись.' -ForegroundColor Yellow
            Pause-Here 'HIP 6.4 удалён?'
        } else {
            Write-Host '  Не нашёл деинсталлятор. Удали "AMD HIP SDK" через Параметры -> Приложения.' -ForegroundColor Yellow
            Pause-Here 'HIP 6.4 удалён?'
        }
    } else {
        Write-Host "  Запускаю: $($uninstaller.FullName) -uninstall" -ForegroundColor White
        Start-Process -FilePath $uninstaller.FullName -ArgumentList '-uninstall' -Wait
    }
}

# ---- установка 6.2.4 ----
if (-not (Test-Path $hip62)) {
    Banner 'Установка HIP 6.2.4'
    & powershell -ExecutionPolicy Bypass -File (Join-Path $here '03-install-hip.ps1') -HipVersion 6.2.4
    Pause-Here 'HIP 6.2.4 установлен, перезагрузись и проверь hipInfo (gfx803).'
} else {
    Write-Host '  HIP 6.2 уже установлен - пропускаю.' -ForegroundColor DarkGray
}

# ---- ZLUDA 3.9.5 rocm6 + патч ----
Banner 'ZLUDA 3.9.5 (rocm6) + подмена DLL'
# -RestoreZluda: вернуть .bak-файлы в torch\lib перед подменой.
# Иначе от ZLUDA 3.9.6 останутся cublas64_11.dll и прочие, а бэкап
# patch-скрипт уже не сделает (он не перезаписывает существующий бэкап).
& powershell -ExecutionPolicy Bypass -File (Join-Path $here '04-comfyui-zluda.ps1') `
    -InstallDir $InstallDir -Profile safe -RestoreZluda

# ---- ядра ----
if ($RocblasArchive) {
    Banner 'Ядра gfx803 под 6.2.4'
    & powershell -ExecutionPolicy Bypass -File (Join-Path $here '05-gfx803-rocblas.ps1') `
        -Archive $RocblasArchive -HipVersion 6.2.4 -Clean
} else {
    Banner 'Ядра gfx803 - нужен архив'
    Write-Host @"
  Скачай один из вариантов под 6.2.4:

    https://github.com/advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU/releases/download/v1.0.0/rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z
        (в имени файла написано 5.7.1-and-6.2.4 - внутри есть сборка под 6.2.4)

    https://github.com/likelovewant/ROCmLibs-for-gfx1103-AMD780M-APU/releases/download/v0.6.2.4/rocBLAS-HIP6.2.4.7z
        (много архитектур в одном архиве; нужен gfx803)

  Затем:
    powershell -ExecutionPolicy Bypass -File .\scripts\install\05-gfx803-rocblas.ps1 -HipVersion 6.2.4 -Archive <путь.7z> -Clean
"@ -ForegroundColor Gray
}

# ---- проверка ----
Banner 'ПРОВЕРКА'
$venvPy = Join-Path $InstallDir 'venv\Scripts\python.exe'
$verify = Join-Path $repoRoot 'scripts\verify-torch27.py'
if ((Test-Path $venvPy) -and (Test-Path $verify)) { & $venvPy $verify }

Write-Host ''
Write-Host 'Готово. Запуск:' -ForegroundColor Green
Write-Host "  cd $InstallDir" -ForegroundColor Green
Write-Host '  .\comfyui-user.bat' -ForegroundColor Green
