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
    [string]$InstallDir = 'C:\AI\ComfyUI-Zluda',
    [ValidateSet('target', 'safe')]
    [string]$Profile = 'target',
    [string]$PythonExe = '',
    [string]$ZludaArchive = '',
    [switch]$SkipRequirements,
    [switch]$NoPatch,
    [switch]$RestoreZluda
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$patchScript = Join-Path $repoRoot 'scripts\patch-torch-zluda.ps1'
$launcher = Join-Path $repoRoot 'scripts\comfyui-user.bat'

function Header([string]$t) {
    Write-Host ''
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('─' * 70) -ForegroundColor DarkCyan
}
function Step([string]$t) { Write-Host "`n>>> $t" -ForegroundColor White }

$zluda = @{
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
& $venvPy -m pip install --upgrade pip --quiet
Write-Host ("  pip: " + (& $venvPy -m pip --version)) -ForegroundColor Green

# ---- torch ----
Step 'PyTorch 2.7.0 + cu118 (~2.7 ГБ, это долго)'
$torchVersion = & $venvPy -c "import torch;print(torch.__version__)" 2>$null
if ($torchVersion -match '^2\.7\.0\+cu118') {
    Write-Host "  Уже установлен: $torchVersion" -ForegroundColor DarkGray
} else {
    & $venvPy -m pip uninstall torch torchvision torchaudio -y --quiet
    & $venvPy -m pip install torch==2.7.0 torchvision==0.22.0 torchaudio==2.7.0 --index-url https://download.pytorch.org/whl/cu118
    if ($LASTEXITCODE -ne 0) { throw 'Установка torch не удалась. Проверь сеть/прокси.' }
}
Write-Host ("  torch: " + (& $venvPy -c "import torch;print(torch.__version__, torch.version.cuda)")) -ForegroundColor Green

# ---- зависимости ----
if (-not $SkipRequirements) {
    Step 'requirements.txt (включая comfy-aimdo, comfy-kitchen, frontend)'
    Push-Location $InstallDir
    try {
        & $venvPy -m pip install -r requirements.txt
        if ($LASTEXITCODE -ne 0) { Write-Host '  ВНИМАНИЕ: requirements завершились с ошибкой - часть пакетов может отсутствовать' -ForegroundColor Yellow }
    } finally { Pop-Location }

    Step 'Фиксы версий'
    & $venvPy -m pip install --quiet "numpy==1.26.4" "onnxruntime"
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
    & $venvPy -c @'
import torch
print("  torch      :", torch.__version__)
print("  cuda       :", torch.version.cuda)
print("  available  :", torch.cuda.is_available())
if torch.cuda.is_available():
    print("  device     :", torch.cuda.get_device_name(0))
'@
} catch { Write-Host "  Ошибка: $_" -ForegroundColor Red } finally { Pop-Location }

Write-Host ''
Write-Host 'Следующий шаг - ядра rocBLAS для gfx803:' -ForegroundColor Green
Write-Host '  powershell -ExecutionPolicy Bypass -File .\scripts\install\05-gfx803-rocblas.ps1 -HipVersion 6.4.2 -Archive .\downloads\rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z -ListOnly' -ForegroundColor Green
Write-Host ''
Write-Host 'Если torch.cuda.is_available() = False:' -ForegroundColor Yellow
Write-Host '  * проверь, что HIP SDK есть в PATH (bin и bin\rocblas\library):' -ForegroundColor Gray
Write-Host '    C:\Program Files\AMD\ROCm\6.4\bin' -ForegroundColor Gray
Write-Host '  * запусти проверку: .\scripts\install\06-verify.ps1' -ForegroundColor Gray
Write-Host ''
Write-Host 'torch.cuda.is_available() = True ещё НЕ значит, что GEMM работает:' -ForegroundColor DarkGray
Write-Host 'без ядер gfx803 первый же matmul упадёт (TensileLibrary).' -ForegroundColor DarkGray
