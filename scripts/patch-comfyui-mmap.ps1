<#
.SYNOPSIS
    Патч ComfyUI для ZLUDA на gfx803: чтение safetensors без mmap.

.DESCRIPTION
    Чинит "Windows fatal exception: access violation" при загрузке моделей:
    torch/storage.py, __getitem__ <- comfy/utils.py, get_tensor.

    ПРИЧИНА
    safetensors.safe_open() отображает файл в память (mmap) и отдает тензор,
    который ссылается на это отображение. ZLUDA на gfx803 читает такую
    область как свою память устройства - access violation на первом же
    get_tensor().

    Штатный --disable-mmap в ComfyUI НЕ помогает: он делает копию
        if DISABLE_MMAP:
            tensor = tensor.to(device=device, copy=True)
    то есть УЖЕ ПОСЛЕ того, как get_tensor() упал.

    ВАЖНО: safetensors.torch.load_file() тоже использует mmap по умолчанию.
    Параметр backend="pread" появился только в safetensors 0.8.0. Поэтому:
        safetensors >= 0.8.0  -  load_file(..., backend="pread")
        safetensors <  0.8.0  -  safetensors.torch.load(bytes), без mmap
    Оба варианта заложены в патч, выбор происходит во время работы.

    ЧТО ЕЩЕ ДЕЛАЕТ
    1. Проверяет и при необходимости обновляет лаунчер comfyui-user.bat.
       В нем обязан быть --disable-mmap. После git pull копия в ComfyUI-Zluda
       сама не обновляется - это уже дважды приводило к тому, что патч
       применялся, но не срабатывал (ветка elif не выполнялась).
    2. Добавляет --disable-cudnn. ZLUDA представляется как CUDA и тянет
       cuDNN-DLL, но рабочей cuDNN для gfx803 нет - свертки падают
       с CUDNN_STATUS_INTERNAL_ERROR посреди генерации.

    Правки помечены ZLUDA_PATCH_MMAP_V2, файлы бэкапятся с меткой времени,
    откат через -Restore. Повторный запуск безопасен.

.PARAMETER InstallDir
    Папка ComfyUI-Zluda. По умолчанию рядом с китом.

.PARAMETER Restore
    Откатить: вернуть comfy\utils.py, comfy\cli_args.py,
    comfy\model_management.py и comfyui-user.bat из .bak-*.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\patch-comfyui-mmap.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\patch-comfyui-mmap.ps1 -Restore
#>
[CmdletBinding()]
param(
    [string]$InstallDir = '',
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'

$kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $InstallDir) { $InstallDir = Join-Path $kitRoot 'ComfyUI-Zluda' }

function Header([string]$t) {
    Write-Host ''
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
    Write-Host "  $t" -ForegroundColor Cyan
    Write-Host ('-' * 70) -ForegroundColor DarkCyan
}
function Ok([string]$t) { Write-Host "[OK]   $t" -ForegroundColor Green }
function Warn2([string]$t) { Write-Host "[WARN] $t" -ForegroundColor Yellow }
function Fail2([string]$t) { Write-Host "[FAIL] $t" -ForegroundColor Red }
function Info2([string]$t) { Write-Host "       $t" -ForegroundColor DarkGray }

Header 'ПАТЧ ComfyUI: чтение safetensors без mmap (для ZLUDA)'

$utilsFile = Join-Path $InstallDir 'comfy\utils.py'
$cliFile   = Join-Path $InstallDir 'comfy\cli_args.py'
$mmFile    = Join-Path $InstallDir 'comfy\model_management.py'
$batFile   = Join-Path $InstallDir 'comfyui-user.bat'
$srcBat    = Join-Path $kitRoot 'scripts\comfyui-user.bat'

if (-not (Test-Path $utilsFile)) {
    Fail2 "Нет файла $utilsFile"
    Info2 'ComfyUI не установлен или указан неверный -InstallDir. Сначала этап 4.'
    exit 1
}

# ---------- откат ----------
if ($Restore) {
    Header 'ОТКАТ'
    $restored = 0
    foreach ($f in @($utilsFile, $cliFile, $mmFile, $batFile)) {
        if (-not (Test-Path $f)) { continue }
        $dir  = Split-Path $f -Parent
        $leaf = Split-Path $f -Leaf
        $bak = Get-ChildItem $dir -Filter "$leaf.bak-*" -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($bak) {
            Copy-Item $bak.FullName $f -Force
            Ok ("восстановлен {0} из {1}" -f $leaf, $bak.Name)
            $restored++
        }
    }
    if ($restored -eq 0) { Warn2 'бэкапов не найдено - откатывать нечего' }
    exit 0
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

# ---------- версия safetensors ----------
$stVer = ''
$dist = Get-ChildItem (Join-Path $InstallDir 'venv\Lib\site-packages') `
        -Filter 'safetensors-*.dist-info' -Directory -ErrorAction SilentlyContinue |
        Select-Object -First 1
if ($dist) { $stVer = $dist.Name -replace '^safetensors-', '' -replace '\.dist-info$', '' }

$usePread = $false
if ($stVer) {
    try {
        if ([version]$stVer -ge [version]'0.8.0') { $usePread = $true }
    } catch { }
}

# ---------- 1. comfy/utils.py ----------
Header '1. comfy/utils.py'

$utils = Get-Content $utilsFile -Raw -Encoding UTF8
$needApply = $false

if ($utils -match 'ZLUDA_PATCH_MMAP_V2') {
    Ok 'патч уже применён (V2) - пропускаю'
} else {
    if ($utils -match 'ZLUDA_PATCH_MMAP') {
        Info2 'найдена версия V1 (load_file без backend - в safetensors <0.8 это тот же mmap)'
        Info2 'возвращаю чистый файл из бэкапа и патчу заново'
        $dir = Split-Path $utilsFile -Parent
        $bak = Get-ChildItem $dir -Filter 'utils.py.bak-*' -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime -Descending |
               Where-Object { -not ((Get-Content $_.FullName -Raw -Encoding UTF8) -match 'ZLUDA_PATCH_MMAP') } |
               Select-Object -First 1
        if (-not $bak) {
            Fail2 'чистый бэкап utils.py не найден - не могу откатить V1 автоматически'
            Info2 'Запусти: .\scripts\patch-comfyui-mmap.ps1 -Restore, затем этот скрипт заново'
            exit 1
        }
        Copy-Item $bak.FullName $utilsFile -Force
        Ok ("чистый файл восстановлен из {0}" -f $bak.Name)
        $utils = Get-Content $utilsFile -Raw -Encoding UTF8
    }
    $needApply = $true
}

if ($needApply) {
    $nl = if ($utils.Contains("`r`n")) { "`r`n" } else { "`n" }

    $oldBlock = @(
        '            else:',
        '                with safetensors.safe_open(ckpt, framework="pt", device=device.type) as f:',
        '                    sd = {}',
        '                    for k in f.keys():',
        '                        tensor = f.get_tensor(k)',
        '                        if DISABLE_MMAP:  # TODO: Not sure if this is the best way to bypass the mmap issues',
        '                            tensor = tensor.to(device=device, copy=True)',
        '                        sd[k] = tensor',
        '                    if return_metadata:',
        '                        metadata = f.metadata()'
    ) -join $nl

    if (-not $utils.Contains($oldBlock)) {
        Fail2 'не найден исходный блок safe_open - версия ComfyUI отличается'
        Info2 'Файл НЕ изменён. Ручной фикс: https://github.com/Comfy-Org/ComfyUI/issues/15424'
        exit 1
    }

    $newBlock = @(
        '            elif DISABLE_MMAP:',
        '                # ZLUDA_PATCH_MMAP_V2: safe_open() отображает файл в память, и',
        '                # первый же get_tensor() падает в torch/storage.py, __getitem__.',
        '                # load_file() по умолчанию ТОЖЕ mmap, поэтому нужен backend:',
        '                #   safetensors >= 0.8.0 - backend="pread" (читает, не отображает)',
        '                #   safetensors <  0.8.0 - load(bytes): файл читается целиком',
        '                from safetensors.torch import load_file',
        '                try:',
        '                    sd = load_file(ckpt, device=device.type, backend="pread")',
        '                except TypeError:',
        '                    from safetensors.torch import load as _zluda_load_bytes',
        '                    with open(ckpt, "rb") as _zluda_fh:',
        '                        sd = _zluda_load_bytes(_zluda_fh.read())',
        '                if return_metadata:',
        '                    try:',
        '                        with safetensors.safe_open(ckpt, framework="pt", device="cpu") as f:',
        '                            metadata = f.metadata()',
        '                    except Exception:',
        '                        metadata = None',
        '                else:',
        '                    metadata = None',
        '            else:',
        '                with safetensors.safe_open(ckpt, framework="pt", device=device.type) as f:',
        '                    sd = {}',
        '                    for k in f.keys():',
        '                        tensor = f.get_tensor(k)',
        '                        sd[k] = tensor',
        '                    if return_metadata:',
        '                        metadata = f.metadata()'
    ) -join $nl

    Copy-Item $utilsFile "$utilsFile.bak-$stamp" -Force
    Ok ("бэкап: utils.py.bak-{0}" -f $stamp)

    $patched = $utils.Replace($oldBlock, $newBlock)
    [IO.File]::WriteAllText($utilsFile, $patched, (New-Object System.Text.UTF8Encoding($false)))

    if ($usePread) {
        Ok 'вставлена ветка elif DISABLE_MMAP -> load_file(backend="pread")'
    } else {
        Ok 'вставлена ветка elif DISABLE_MMAP -> load(bytes), без mmap'
    }
}

# ---------- 2. лаунчер ----------
Header '2. comfyui-user.bat (лаунчер)'

$launcherOk = $false
if (-not (Test-Path $batFile)) {
    Fail2 "нет $batFile - ComfyUI запускать нечем"
} else {
    $enc866 = [Text.Encoding]::GetEncoding(866)
    $batText = $enc866.GetString([IO.File]::ReadAllBytes($batFile))

    if ($batText -match '--disable-mmap') {
        Ok 'лаунчер передает --disable-mmap'
        $launcherOk = $true
    } elseif (-not (Test-Path $srcBat)) {
        Fail2 "не найден $srcBat - не могу обновить лаунчер"
        Info2 'Сделай git pull в папке кита и запусти скрипт заново'
    } else {
        Warn2 'лаунчер УСТАРЕЛ: в аргументах нет --disable-mmap'
        Copy-Item $batFile "$batFile.bak-$stamp" -Force
        Copy-Item $srcBat $batFile -Force
        Ok ("обновлен из репозитория (бэкап: comfyui-user.bat.bak-{0})" -f $stamp)
        $batText = $enc866.GetString([IO.File]::ReadAllBytes($batFile))
        if ($batText -match '--disable-mmap') {
            $launcherOk = $true
        } else {
            Fail2 'в репозиторном comfyui-user.bat тоже нет --disable-mmap'
            Info2 'Сделай git pull в папке кита и запусти скрипт заново'
        }
    }

    if ($batText -match 'set "COMMANDLINE_ARGS=([^"]*)"') {
        Info2 ("теперь аргументы: " + $Matches[1])
    }
}

# ---------- 3. comfy/cli_args.py ----------
Header '3. comfy/cli_args.py'

if (-not (Test-Path $cliFile)) {
    Warn2 "нет $cliFile - пропускаю --disable-cudnn"
} else {
    $cli = Get-Content $cliFile -Raw -Encoding UTF8
    if ($cli -match 'disable-cudnn') {
        Ok '--disable-cudnn уже есть'
    } else {
        $anchor = 'parser.add_argument("--cpu-vae", action="store_true", help="Run the VAE on the CPU.")'
        if ($cli.Contains($anchor)) {
            Copy-Item $cliFile "$cliFile.bak-$stamp" -Force
            $add = $anchor + "`r`n" + 'parser.add_argument("--disable-cudnn", action="store_true", help="Disable cuDNN entirely. Needed on backends that present as CUDA but have no working cuDNN, such as ZLUDA on gfx803.")'
            $cli = $cli.Replace($anchor, $add)
            [IO.File]::WriteAllText($cliFile, $cli, (New-Object System.Text.UTF8Encoding($false)))
            Ok 'добавлен флаг --disable-cudnn'
        } else {
            Warn2 'не нашёл якорь --cpu-vae - --disable-cudnn не добавлен'
            Info2 'cuDNN и так отключается: comfy\zluda.py ставит torch.backends.cudnn.enabled = False'
        }
    }
}

# ---------- 4. comfy/model_management.py ----------
Header '4. comfy/model_management.py'

if (-not (Test-Path $mmFile)) {
    Warn2 "нет $mmFile - пропускаю"
} else {
    $mm = Get-Content $mmFile -Raw -Encoding UTF8
    if ($mm -match 'ZLUDA_PATCH_CUDNN') {
        Ok 'обработка --disable-cudnn уже есть'
    } elseif ($mm -match 'disable_cudnn') {
        Ok 'флаг уже упоминается - пропускаю'
    } else {
        $anchor = "AMD_ENABLE_MIOPEN_ENV = 'COMFYUI_ENABLE_MIOPEN'"
        if ($mm.Contains($anchor)) {
            Copy-Item $mmFile "$mmFile.bak-$stamp" -Force
            $nl2 = if ($mm.Contains("`r`n")) { "`r`n" } else { "`n" }
            $body = @(
                '# ZLUDA_PATCH_CUDNN: бэкенд ZLUDA представляется как CUDA и содержит',
                '# cuDNN-DLL, но рабочей cuDNN для gfx803 нет - свертки падают с',
                '# CUDNN_STATUS_INTERNAL_ERROR посреди генерации.',
                'if getattr(args, "disable_cudnn", False):',
                '    torch.backends.cudnn.enabled = False',
                '    logging.info("Set: torch.backends.cudnn.enabled = False from --disable-cudnn.")'
            ) -join $nl2
            $add = $anchor + $nl2 + $nl2 + $body
            $mm = $mm.Replace($anchor, $add)
            [IO.File]::WriteAllText($mmFile, $mm, (New-Object System.Text.UTF8Encoding($false)))
            Ok 'добавлена обработка --disable-cudnn'
        } else {
            Warn2 'не нашёл якорь AMD_ENABLE_MIOPEN_ENV - пропускаю'
        }
    }
}

# ---------- итог ----------
Header 'ГОТОВО'

Write-Host '  Версия safetensors: ' -NoNewline -ForegroundColor White
if (-not $stVer) {
    Write-Host 'не найдена в venv\Lib\site-packages' -ForegroundColor Yellow
} elseif ($usePread) {
    Write-Host ("{0} - будет backend='pread'" -f $stVer) -ForegroundColor Green
} else {
    Write-Host ("{0} - backend='pread' нет, будет полное чтение load(bytes)" -f $stVer) -ForegroundColor Yellow
    Warn2 'safetensors старее 0.8.0: файл читается в RAM ЦЕЛИКОМ'
    Info2 'Пик памяти равен размеру файла модели. Для VAE (~0.3 GB) нормально,'
    Info2 'но текстовый энкодер на 9 GB при 16 GB RAM может не влезть.'
    Info2 ''
    Info2 'Лучше обновить safetensors: тогда работает backend=pread,'
    Info2 'он читает по одному тензору и держит в RAM только его:'
    Info2 ('  {0}\venv\Scripts\python.exe -m pip install -U "safetensors>=0.8.0"' -f $InstallDir)
    Info2 'Патч переписывать не нужно: ветка выбирает backend сама.'
}

Write-Host ''
Write-Host '  Что изменилось:' -ForegroundColor White
Info2 'load_torch_file при --disable-mmap больше не использует mmap вообще'
Info2 'safetensors отдает тензоры из обычной памяти, падения нет'
Write-Host ''
Write-Host '  Дальше:' -ForegroundColor Green
if ($launcherOk) {
    Info2 '1) запусти .\ComfyUI-Zluda\comfyui-user.bat'
} else {
    Info2 '1) СТОП: лаунчер не передает --disable-mmap, патч не сработает'
    Info2 '   сделай git pull в D:\Apps\rx-580-comfyui и запусти скрипт заново'
}
Info2 '2) в логе должна быть строка args: ... --disable-mmap'
Info2 '3) если появилось "CUDA device detected: None" - перезагрузи ПК'
Info2 '   (после access violation контекст HIP остается зависшим)'
Write-Host ''
Write-Host '  Откат:' -ForegroundColor Yellow
Info2 'powershell -ExecutionPolicy Bypass -File .\scripts\patch-comfyui-mmap.ps1 -Restore'
exit 0
