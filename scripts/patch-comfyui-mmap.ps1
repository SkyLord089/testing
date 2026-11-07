<#
.SYNOPSIS
    Патч ComfyUI для ZLUDA на gfx803: чтение safetensors без mmap.

.DESCRIPTION
    Чинит "Windows fatal exception: access violation" при загрузке моделей.

    ПРИЧИНА
    Штатный --disable-mmap в ComfyUI бесполезен: он делает
        tensor = f.get_tensor(k)          <- здесь происходит падение
        if DISABLE_MMAP:
            tensor = tensor.to(copy=True) <- а копия делается УЖЕ ПОСЛЕ
    safetensors.safe_open() отдаёт тензор, который ссылается на mmap-область
    файла. ZLUDA читает её как свою память устройства -> access violation
    в torch/storage.py, __getitem__. Флаг ничего не предотвращает.

    ФИКС
    При --disable-mmap читать файл целиком через
        safetensors.torch.load_file(ckpt, device=...)
    который читает, а не отображает в память.

    Взято из public-патча WYSTAF/polaris-comfyui-zluda (patches/
    comfyui-legacy-gpu.patch, часть 3/4) и issue Comfy-Org/ComfyUI#15424.

    Что ещё делает: добавляет --disable-cudnn. Бэкенд ZLUDA представляется
    как CUDA и содержит cuDNN-DLL, но рабочей cuDNN для gfx803 нет -
    свёртки падают с CUDNN_STATUS_INTERNAL_ERROR посреди генерации.
    ComfyUI-шная проверка на AMD это не ловит, потому что устройство
    рапортуется как CUDA.

    Ничего не ломает: правки помечены блоками и откатываются через -Restore.
    Повторный запуск безопасен - патч накладывается один раз.

.PARAMETER InstallDir
    Папка ComfyUI-Zluda. По умолчанию рядом с китом.

.PARAMETER Restore
    Откатить: вернуть comfy\utils.py и comfy\cli_args.py из .bak-*.

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

if (-not $InstallDir) {
    $kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
    $InstallDir = Join-Path $kitRoot 'ComfyUI-Zluda'
}

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
$cliFile = Join-Path $InstallDir 'comfy\cli_args.py'
$mmFile = Join-Path $InstallDir 'comfy\model_management.py'

if (-not (Test-Path $utilsFile)) {
    Fail2 "Нет файла $utilsFile"
    Info2 'ComfyUI не установлен или указан неверный -InstallDir. Сначала этап 4.'
    exit 1
}

# ---------- откат ----------
if ($Restore) {
    Header 'ОТКАТ'
    $restored = 0
    foreach ($f in @($utilsFile, $cliFile, $mmFile)) {
        if (-not (Test-Path $f)) { continue }
        $dir = Split-Path $f -Parent
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

# ---------- 1. comfy/utils.py: DISABLE_MMAP -> load_file ----------
Header '1. comfy/utils.py'

$utils = Get-Content $utilsFile -Raw -Encoding UTF8

if ($utils -match 'ZLUDA_PATCH_MMAP') {
    Ok 'патч mmap уже применён - пропускаю'
} else {
    # Ищем оригинальный блок: safe_open + DISABLE_MMAP после get_tensor
    $origPattern = '(?s)(\s+)with safetensors\.safe_open\(ckpt, framework="pt", device=device\.type\) as f:\r?\n(\s+)sd = \{\}\r?\n\s+for k in f\.keys\(\):\r?\n\s+tensor = f\.get_tensor\(k\)\r?\n\s+if DISABLE_MMAP:.*?\r?\n\s+sd\[k\] = tensor'

    if ($utils -notmatch $origPattern) {
        # возможно, уже другая раскладка - ищем просто строку с DISABLE_MMAP
        if ($utils -match 'if DISABLE_MMAP') {
            Fail2 'структура load_torch_file не совпала с ожидаемой'
            Info2 'Версия ComfyUI отличается. Правь вручную по инструкции:'
            Info2 '  https://github.com/Comfy-Org/ComfyUI/issues/15424'
        } else {
            Fail2 'не найден блок safe_open в load_torch_file'
            Info2 'Возможно, это другая версия ComfyUI. Патч не применён.'
        }
        exit 1
    }

    Copy-Item $utilsFile "$utilsFile.bak-$stamp" -Force
    Ok ("бэкап: utils.py.bak-{0}" -f $stamp)

    $replacement = @'
                # ZLUDA_PATCH_MMAP: safe_open() отображает файл в память, и
                # первый же get_tensor() падает в torch/storage.py, __getitem__
                # с access violation (ZLUDA читает mmap как свою память).
                # Штатный --disable-mmap копировал ПОСЛЕ get_tensor, то есть
                # уже после падения. load_file() читает файл, а не отображает.
                elif DISABLE_MMAP:
                    from safetensors.torch import load_file
                    sd = load_file(ckpt, device=device.type)
                    if return_metadata:
                        try:
                            with safetensors.safe_open(ckpt, framework="pt", device="cpu") as f:
                                metadata = f.metadata()
                        except Exception:
                            metadata = None
                    else:
                        metadata = None
'@

    # Заменяем ЦЕЛИКОМ блок "else: with safetensors.safe_open(...)" на цепочку
    # elif DISABLE_MMAP / else. По точному тексту, как в эталонном патче
    # WYSTAF/polaris-comfyui-zluda. Вставка по кускам опасна: можно попасть
    # во внутренний else блока aimdo и получить невалидный Python.
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
        '                # ZLUDA_PATCH_MMAP: safe_open() отображает файл в память, и',
        '                # первый же get_tensor() падает в torch/storage.py, __getitem__',
        '                # с access violation: ZLUDA читает mmap как свою память.',
        '                # Штатный --disable-mmap копировал ПОСЛЕ get_tensor, то есть',
        '                # уже после падения. load_file() читает, а не отображает.',
        '                from safetensors.torch import load_file',
        '                sd = load_file(ckpt, device=device.type)',
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

    $patched = $utils.Replace($oldBlock, $newBlock)

    [IO.File]::WriteAllText($utilsFile, $patched, (New-Object System.Text.UTF8Encoding($false)))
    Ok 'вставлена ветка elif DISABLE_MMAP -> load_file'
}

# ---------- 2. comfy/cli_args.py: --disable-cudnn ----------
Header '2. comfy/cli_args.py'

if (-not (Test-Path $cliFile)) {
    Warn2 "нет $cliFile - пропускаю --disable-cudnn"
} else {
    $cli = Get-Content $cliFile -Raw -Encoding UTF8
    if ($cli -match 'disable-cudnn') {
        Ok '--disable-cudnn уже есть'
    } else {
        $anchor = 'parser.add_argument("--cpu-vae", action="store_true", help="Run the VAE on the CPU.")'
        if ($cli -match [regex]::Escape($anchor)) {
            Copy-Item $cliFile "$cliFile.bak-$stamp" -Force
            $add = $anchor + "`r`nparser.add_argument(`"--disable-cudnn`", action=`"store_true`", help=`"Disable cuDNN entirely. Needed on backends that present as CUDA but have no working cuDNN, such as ZLUDA on gfx803.`")"
            $cli = $cli.Replace($anchor, $add)
            [IO.File]::WriteAllText($cliFile, $cli, (New-Object System.Text.UTF8Encoding($false)))
            Ok 'добавлен флаг --disable-cudnn'
        } else {
            Warn2 'не нашёл якорь --cpu-vae - --disable-cudnn не добавлен'
            Info2 'cuDNN отключается и так: comfy\zluda.py ставит torch.backends.cudnn.enabled = False'
        }
    }
}

# ---------- 3. comfy/model_management.py: обработка флага ----------
Header '3. comfy/model_management.py'

if (-not (Test-Path $mmFile)) {
    Warn2 "нет $mmFile - пропускаю"
} else {
    $mm = Get-Content $mmFile -Raw -Encoding UTF8
    if ($mm -match 'ZLUDA_PATCH_CUDNN') {
        Ok 'обработка --disable-cudnn уже есть'
    } elseif ($mm -match 'disable_cudnn') {
        Ok 'флаг уже упоминается - пропускаю'
    } else {
        $anchor = 'AMD_ENABLE_MIOPEN_ENV = ''COMFYUI_ENABLE_MIOPEN'''
        if ($mm -match [regex]::Escape($anchor)) {
            Copy-Item $mmFile "$mmFile.bak-$stamp" -Force
            $add = $anchor + @'

# ZLUDA_PATCH_CUDNN: бэкенд ZLUDA представляется как CUDA и содержит
# cuDNN-DLL, но рабочей cuDNN для gfx803 нет - свёртки падают с
# CUDNN_STATUS_INTERNAL_ERROR посреди генерации.
if getattr(args, "disable_cudnn", False):
    torch.backends.cudnn.enabled = False
    logging.info("Set: torch.backends.cudnn.enabled = False from --disable-cudnn.")
'@
            $add = $add -replace "`r`n", "`r`n"
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
Write-Host '  Что изменилось:' -ForegroundColor White
Info2 'load_torch_file при --disable-mmap читает файл через load_file()'
Info2 'вместо safe_open(): mmap больше не используется, падения нет'
Write-Host ''
Write-Host '  Дальше:' -ForegroundColor Green
Info2 '1) убедись, что лаунчер передаёт --disable-mmap  (он это уже делает)'
Info2 '2) удали кэш ZLUDA, если менял DLL: Remove-Item -Recurse .\ComfyUI-Zluda\zluda-cache'
Info2 '3) запусти ComfyUI тем же comfyui-user.bat'
Write-Host ''
Write-Host '  Откат:' -ForegroundColor Yellow
Info2 'powershell -ExecutionPolicy Bypass -File .\scripts\patch-comfyui-mmap.ps1 -Restore'
exit 0
