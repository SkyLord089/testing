<#
.SYNOPSIS
    Этап 4: ComfyUI-Zluda + venv + torch 2.7+cu118 + ZLUDA + подмена DLL.

.DESCRIPTION
    Идемпотентно ставит всё, кроме HIP SDK и ядер rocBLAS:
      1. git clone patientx/ComfyUI-Zluda
      2. venv + pip + requirements
      3. torch/torchvision/torchaudio cu118 (2.7.0)
      4. numpy 1.26.4, onnxruntime
      5. ZLUDA (3.9.6 для target / 3.9.5-rocm6 для safe)
      6. подмена DLL через scripts\patch-torch-zluda.ps1 (без cublasLt!)
      7. лаунчер comfyui-user.bat

    Triton / sageattention / flash-attn НЕ ставятся: на gfx803 они бесполезны,
    а без MSVC Build Tools просто не соберутся.

.PARAMETER InstallDir
    Куда ставить.

.PARAMETER Profile
    target (HIP 6.4.2 + ZLUDA 3.9.6) | safe (HIP 6.2.4 + ZLUDA 3.9.5-rocm6)

.PARAMETER PythonExe
    Полный путь к python.exe. По умолчанию ищет в PATH.

.PARAMETER SkipRequirements
    Не ставить requirements.txt (если уже стоит).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\install\04-comfyui-zluda.ps1 -Profile target
#>
[CmdletBinding()]
param(
    # по умолчанию - рядом с китом, то есть на том же диске (C: часто занят)
    [string]$InstallDir = '',
    [ValidateSet('rocm5', 'target', 'safe')]
    [string]$Profile = 'rocm5',
    [string]$PythonExe = '',
    [string]$ZludaArchive = '',
    [switch]$SkipRequirements,
    [switch]$NoPatch,
    [switch]$RestoreZluda
)

$ErrorActionPreference = 'Stop'

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

$ProgressPreference = 'SilentlyContinue'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $InstallDir) { $InstallDir = Join-Path $repoRoot 'ComfyUI-Zluda' }
$patchScript = Join-Path $repoRoot 'scripts\patch-torch-zluda.ps1'
$launcher = Join-Path $repoRoot 'scripts\comfyui-user.bat'

# кэши pip и HF - рядом с установкой, чтобы не забивать C:
$cacheRoot = Join-Path (Split-Path $InstallDir -Parent) '_cache'
if (-not (Test-Path $cacheRoot)) { New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null }

function Header([string]$t) {
    Write-Host ''
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
}
function Step([string]$t) { Write-Host "`n>>> $t" -ForegroundColor White }

# ВАЖНО, как выбирается DLL HIP.
# ZLUDA линкуется на ГОЛОЕ имя "dylib=amdhip64" (hip_runtime-sys/build.rs), а
# HIP SDK 6.x ставит файл в bin\ как amdhip64_6.dll
# (CMakeLists.txt: RUNTIME_OUTPUT_NAME "amdhip64_${HIP_VERSION_MAJOR}").
# Значит голое имя amdhip64.dll в bin\ SDK НЕ находится и резолвится по
# системному порядку поиска: System32 (драйверная копия) идёт раньше PATH.
#
# Проверено на RX 580 / драйвер 31.0.21925.1001:
#   SDK  bin\amdhip64_6.dll   (10.0.3652.0) -> hipInit = 100, устройств нет
#   System32\amdhip64.dll      (10.0.3584.0) -> hipInit = 0, 1 устройство
#                                              "Radeon RX 580 Series"
# То есть работать будет ДРАЙВЕРНАЯ копия. Поэтому:
#   * не добавляем bin\ SDK в PATH в лаунчере - это может перебить её;
#   * сборку rocm5 берём по умолчанию: у неё API v5 (hipGetDeviceProperties
#     без суффикса R0600), она и рассчитана на связку с драйверной DLL.
$zluda = @{
    # основной путь для gfx803 на драйвере 31.0.21925.x
    'rocm5' = @{
        Url  = 'https://github.com/lshqqytiger/ZLUDA/releases/download/rel.854c58e1565c3597e17046d7cc2b1eab96fcdf55/ZLUDA-windows-rocm5-amd64.zip'
        File = 'ZLUDA-windows-rocm5-amd64.zip'
        Name = 'ZLUDA 3.9.6 rocm5 (драйверная amdhip64.dll из System32)'
    }
    # HIP 6.4 из SDK: на этой машине hipInit не проходит, оставлено для опытов
    'target' = @{
        Url  = 'https://github.com/lshqqytiger/ZLUDA/releases/download/rel.854c58e1565c3597e17046d7cc2b1eab96fcdf55/ZLUDA-windows-amd64.zip'
        File = 'ZLUDA-windows-amd64.zip'
        Name = 'ZLUDA 3.9.6 (HIP 6.4/7.1)'
    }
    'safe' = @{
        Url  = 'https://github.com/lshqqytiger/ZLUDA/releases/download/rel.5e717459179dc272b7d7d23391f0fad66c7459cf/ZLUDA-windows-rocm6-amd64.zip'
        File = 'ZLUDA-windows-rocm6-amd64.zip'
        Name = 'ZLUDA 3.9.5 rocm6 (HIP 6.2)'
    }
}

Header "ЭТАП 4: ComfyUI-Zluda  [профиль: $Profile]"

# ---- python ----
Step 'Поиск Python'
if (-not $PythonExe) {
    $cand = @()
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $cand += $cmd.Source }
    $cand += @(
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        'C:\Python311\python.exe',
        'C:\Python312\python.exe'
    )
    foreach ($c in $cand) { if ($c -and (Test-Path $c)) { $PythonExe = $c; break } }
}
if (-not $PythonExe -or -not (Test-Path $PythonExe)) {
    throw 'Python не найден. Поставь Python 3.11.9 x64 с python.org (Add to PATH) и повтори, либо укажи -PythonExe "C:\...\python.exe"'
}
$pyVer = & $PythonExe -c "import sys;print('.'.join(map(str,sys.version_info[:3])))"
$pyBits = & $PythonExe -c "import struct;print(struct.calcsize('P')*8)"
Write-Host ("  Python {0} ({1}-bit): {2}" -f $pyVer, $pyBits, $PythonExe) -ForegroundColor Green
if ($pyBits -ne '64') { throw 'Нужен 64-битный Python.' }
if ($pyVer -notmatch '^3\.(10|11|12)\.') { Write-Host "  ВНИМАНИЕ: рекомендован 3.11.x, у тебя $pyVer" -ForegroundColor Yellow }

# ---- каталог и репозиторий ----
Step 'Репозиторий ComfyUI-Zluda'
if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null }
if (-not (Test-Path (Join-Path $InstallDir 'main.py'))) {
    if ((Get-ChildItem $InstallDir -Force | Measure-Object).Count -gt 0) {
        throw "Каталог $InstallDir не пуст и не похож на ComfyUI. Укажи другой -InstallDir."
    }
    Write-Host '  git clone https://github.com/patientx/ComfyUI-Zluda ...' -ForegroundColor White
    & git clone --depth 1 https://github.com/patientx/ComfyUI-Zluda $InstallDir
    if ($LASTEXITCODE -ne 0) { throw 'git clone не удался (нет git или сети)' }
} else {
    Write-Host '  Уже есть main.py - пропускаю clone.' -ForegroundColor DarkGray
}

# ---- venv ----
Step 'Виртуальное окружение'
$venvDir = Join-Path $InstallDir 'venv'
$venvPy = Join-Path $venvDir 'Scripts\python.exe'
if (-not (Test-Path $venvPy)) {
    & $PythonExe -m venv $venvDir
    if ($LASTEXITCODE -ne 0) { throw 'Не удалось создать venv' }
} else {
    Write-Host '  venv уже существует.' -ForegroundColor DarkGray
}
# pip-кэш на диск установки: иначе ~3 ГБ колес осядут в C:\Users\...\AppData\Local\pip
$pipCache = Join-Path $cacheRoot 'pip'
if (-not (Test-Path $pipCache)) { New-Item -ItemType Directory -Path $pipCache -Force | Out-Null }
Write-Host ("  pip cache: {0}" -f $pipCache) -ForegroundColor DarkGray

& $venvPy -m pip install --upgrade pip --quiet --cache-dir "$pipCache"
Write-Host ("  pip: " + (& $venvPy -m pip --version)) -ForegroundColor Green

# ---- torch ----
Step 'PyTorch 2.7.0 + cu118 (~2.7 ГБ, это долго)'
$probe = Invoke-Capture $venvPy @('-c', 'import torch;print(torch.__version__)')
if ($probe.Code -eq 0 -and $probe.Text -match '^2\.7\.0\+cu118') {
    Write-Host ("  Уже установлен: {0}" -f $probe.Text.Trim()) -ForegroundColor DarkGray
} else {
    & $venvPy -m pip uninstall torch torchvision torchaudio -y --quiet
    & $venvPy -m pip install torch==2.7.0 torchvision==0.22.0 torchaudio==2.7.0 --index-url https://download.pytorch.org/whl/cu118 --cache-dir "$pipCache"
    if ($LASTEXITCODE -ne 0) { throw 'Установка torch не удалась. Проверь сеть/прокси.' }
}
$torchInfo = Invoke-Capture $venvPy @('-c', 'import torch;print(torch.__version__, torch.version.cuda)')
if ($torchInfo.Code -eq 0) {
    Write-Host ("  torch: " + $torchInfo.Text.Trim()) -ForegroundColor Green
} else {
    throw ("torch установлен, но не импортируется:`n" + $torchInfo.Text)
}

# ---- зависимости ----
if (-not $SkipRequirements) {
    Step 'requirements.txt (включая comfy-aimdo, comfy-kitchen, frontend)'
    Push-Location $InstallDir
    try {
        & $venvPy -m pip install -r requirements.txt --cache-dir "$pipCache"
        if ($LASTEXITCODE -ne 0) { Write-Host '  ВНИМАНИЕ: requirements завершились с ошибкой - часть пакетов может отсутствовать' -ForegroundColor Yellow }
    } finally { Pop-Location }

    Step 'Фиксы версий'
    & $venvPy -m pip install --quiet "numpy==1.26.4" "onnxruntime" --cache-dir "$pipCache"
}

# ---- zluda ----
Step "Скачивание ZLUDA: $($zluda[$Profile].Name)"
$dl = Join-Path $repoRoot 'downloads'
if (-not (Test-Path $dl)) { New-Item -ItemType Directory -Path $dl -Force | Out-Null }
$zipPath = if ($ZludaArchive) { $ZludaArchive } else { Join-Path $dl $zluda[$Profile].File }
if (-not (Test-Path $zipPath)) {
    & "$env:SystemRoot\system32\curl.exe" -L --ssl-no-revoke -o $zipPath $zluda[$Profile].Url
    if ($LASTEXITCODE -ne 0) { throw 'Не удалось скачать ZLUDA' }
}
$zludaDir = Join-Path $InstallDir 'zluda'
if (Test-Path $zludaDir) {
    $bak = "$zludaDir.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
    Move-Item $zludaDir $bak
    Write-Host "  Старая папка ZLUDA сохранена: $bak" -ForegroundColor DarkGray
}
New-Item -ItemType Directory -Path $zludaDir -Force | Out-Null
Write-Host "  Распаковка в $zludaDir" -ForegroundColor White
Expand-Archive -LiteralPath $zipPath -DestinationPath $zludaDir -Force
# в архиве верхняя папка zluda\ - разворачиваем, если так
if ((Test-Path (Join-Path $zludaDir 'zluda')) -and -not (Test-Path (Join-Path $zludaDir 'cublas.dll'))) {
    Get-ChildItem (Join-Path $zludaDir 'zluda') -Force | Move-Item -Destination $zludaDir -Force
    Remove-Item (Join-Path $zludaDir 'zluda') -Recurse -Force -ErrorAction SilentlyContinue
}
Get-ChildItem $zludaDir -File | ForEach-Object { Write-Host ("    {0,-22} {1,10:N0} байт" -f $_.Name, $_.Length) -ForegroundColor DarkGray }

# ---- патч DLL ----
if (-not $NoPatch) {
    Step 'Подмена DLL в venv\torch\lib (с бэкапом)'
    if (-not (Test-Path $patchScript)) { throw "Не найден $patchScript" }
    if ($RestoreZluda) {
        Write-Host '  -RestoreZluda: возвращаю оригинальные DLL из бэкапов' -ForegroundColor Yellow
        & powershell -ExecutionPolicy Bypass -File $patchScript -Venv $venvDir -Restore
        if ($LASTEXITCODE -ne 0) { throw 'Не удалось восстановить оригинальные DLL (см. вывод выше)' }
    }
    & powershell -ExecutionPolicy Bypass -File $patchScript -ZludaDir $zludaDir -Venv $venvDir
}

# ---- comfy/zluda.py из customzluda ----
Step 'Активация comfy\zluda.py'
$czSrc = Join-Path $InstallDir 'comfy\customzluda\zluda.py'
$czDst = Join-Path $InstallDir 'comfy\zluda.py'
if ((Test-Path $czSrc) -and (Test-Path $czDst)) {
    $s1 = (Get-FileHash $czSrc -Algorithm SHA256).Hash
    $s2 = (Get-FileHash $czDst -Algorithm SHA256).Hash
    if ($s1 -ne $s2) {
        Copy-Item $czDst "$czDst.bak-$(Get-Date -Format yyyyMMdd-HHmmss)" -Force
        Copy-Item $czSrc $czDst -Force
        Write-Host '  comfy\zluda.py обновлён из customzluda (так делает install-n.bat)' -ForegroundColor Green
    } else {
        Write-Host '  Уже актуален.' -ForegroundColor DarkGray
    }
} else {
    Write-Host '  customzluda\zluda.py не найден - пропускаю' -ForegroundColor Yellow
}

# ---- лаунчер ----
Step 'Лаунчер'
if (Test-Path $launcher) {
    $dst = Join-Path $InstallDir 'comfyui-user.bat'
    if (Test-Path $dst) { Copy-Item $dst "$dst.bak-$(Get-Date -Format yyyyMMdd-HHmmss)" -Force }
    Copy-Item $launcher $dst -Force
    Write-Host "  Скопирован: $dst" -ForegroundColor Green
} else {
    Write-Host "  Лаунчер не найден в репозитории ($launcher) - пропускаю" -ForegroundColor Yellow
}

# ---- проверка ----
Header 'ПРОВЕРКА'
Push-Location $InstallDir
try {
    # ZLUDA-шим должен грузиться из venv\Scripts: кладём nvcuda.dll и туда
    $nvcudaSrc = Join-Path $zludaDir 'nvcuda.dll'
    $nvcudaDst = Join-Path $venvDir 'Scripts\nvcuda.dll'
    if (Test-Path $nvcudaSrc) {
        Copy-Item $nvcudaSrc $nvcudaDst -Force
        Write-Host "  nvcuda.dll -> $nvcudaDst" -ForegroundColor DarkGray
    }

    Write-Host '  torch.cuda:' -ForegroundColor White
    # Код передаём ФАЙЛОМ, а не через -c с here-string.
    # В PowerShell 5.1 многострочный аргумент для нативной программы
    # (python -c @'...'@) разбивается на части, и python получает обрывок
    # вроде "print(" -> SyntaxError: '(' was never closed.
    $probeScript = Join-Path $env:TEMP 'comfy-torch-probe.py'
    $probeCode = @(
        'import torch',
        'print("  torch      :", torch.__version__)',
        'print("  cuda       :", torch.version.cuda)',
        'print("  available  :", torch.cuda.is_available())',
        'if torch.cuda.is_available():',
        '    print("  device     :", torch.cuda.get_device_name(0))'
    ) -join "`n"
    Set-Content -Path $probeScript -Value $probeCode -Encoding UTF8
    $probe = Invoke-Capture $venvPy @($probeScript)
    if ($probe.Text) { $probe.Text -split "`n" | Where-Object { $_ -ne '' } | ForEach-Object { Write-Host "  $_" } }
    if ($probe.Code -ne 0) {
        Write-Host ("  [FAIL] проверка torch упала с кодом {0}" -f $probe.Code) -ForegroundColor Red
    }
    Remove-Item $probeScript -Force -ErrorAction SilentlyContinue
} catch { Write-Host "  Ошибка: $_" -ForegroundColor Red } finally { Pop-Location }

Write-Host ''
Write-Host 'Следующий шаг - ядра rocBLAS для gfx803:' -ForegroundColor Green
Write-Host '  powershell -ExecutionPolicy Bypass -File .\scripts\install\05-gfx803-rocblas.ps1 -HipVersion 6.4.2 -Archive .\downloads\rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z -ListOnly' -ForegroundColor Green
Write-Host ''
Write-Host 'Если torch.cuda.is_available() = False:' -ForegroundColor Yellow
Write-Host '  * HIP работает через ДРАЙВЕРНУЮ копию System32\amdhip64.dll - так и надо.' -ForegroundColor Gray
Write-Host '    НЕ добавляй bin HIP SDK в PATH: amdhip64_6.dll оттуда даёт hipInit=100.' -ForegroundColor Gray
Write-Host '  * что именно грузится - покажет этап 7: scripts\install\07-hip-probe.ps1' -ForegroundColor Gray
Write-Host '  * полная проверка: .\scripts\install\06-verify.ps1' -ForegroundColor Gray
Write-Host ''
Write-Host 'torch.cuda.is_available() = True ещё НЕ значит, что GEMM работает:' -ForegroundColor DarkGray
Write-Host 'без ядер gfx803 первый же matmul упадёт (TensileLibrary).' -ForegroundColor DarkGray
