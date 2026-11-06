# Пошаговая установка: ComfyUI на RX 580 @ Windows 11 (HIP SDK 6.2.4 + torch 2.7.0+cu118)

> [!WARNING]
> **HIP SDK 6.4 — тупик** (подтверждено на железе 2026-11-06): сообщественный патч rocBLAS подменяет `rocblas.dll`
> целиком сборкой под 6.2.4/5.7.1, а у неё нет точек входа `rocblas_*_64`, требуемых `hipblas.dll` из 6.4 →
> `WinError 127` при `import torch`. **Ставьте HIP SDK 6.2.4** (или 5.7.1). Ниже везде 6.2.4; если вы всё же хотите
> эксперимента 6.4 — замените `-RocmVersion 6.2` на `6.4` и будьте готовы к ошибке из раздела про WinError 127.

Время: ~1–2 часа (большая часть — скачивание) + 10–30 минут первой компиляции при первом запуске.

## 0. Что нужно заранее

- Windows 11 (10 тоже подойдёт, скрипты не используют ничего 11-специфичного, кроме `tar.exe`, который есть и там).
- AMD Adrenalin — **свежий** драйвер для Polaris (PRO не нужен). Поставьте до начала.
- **HIP SDK 6.2.4** — ставится **вручную**: <https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html>
  (официальный инсталлятор, нужен аккаунт AMD / согласие с EULA — поэтому скрипт его не ставит сам).
  После установки проверьте, что есть `C:\Program Files\AMD\ROCm\6.2\bin\rocblas`.
  Историческая справка: с HIP 6.4 связка теперь **подтверждённо не работает** — патч rocBLAS gfx803 собран под 6.2.4/5.7.1
  и неэкспортирует `rocblas_*_64`, нужные `hipblas.dll` 6.4 (`WinError 127`).
- **Каталог `C:\Program Files\AMD\ROCm\6.2\bin` должен быть в `PATH`** (иначе `amdhip64.dll` и вся HIP-семья
  не загрузятся — классическая причина WinError 126/127 на `import torch`). Проверить:
  `[Environment]::GetEnvironmentVariable('Path','Machine') -split ';' | Where-Object { $_ -match 'ROCm' }`
  и то же с `'User'`. Если пусто — добавьте каталог через Система → О системе → Дополнительные параметры →
  Переменные среды (или одной строкой выше, с `'User'`), и **перезапустите консоль**.
- Git — <https://git-scm.com/download/win> (при установке отметьте "Git from the command line").
- Python **3.11** с python.org <https://www.python.org/downloads/windows/> — отмечайте **"Add Python to PATH"**.
  НЕ ставьте питон из Microsoft Store. 3.10–3.12 тоже подойдут.
- Visual C++ Redistributable x64 — <https://aka.ms/vs/17/release/vc_redist.x64.exe> (обычно уже есть).
- ~25 ГБ свободного места **на целевом диске** (по умолчанию — C:, можно целиком поставить на D:, см. раздел «Установка на диск D:» ниже) и **pagefile ≥ 16 ГБ** (Система → Дополнительные параметры → Быстродействие →
  Виртуальная память). На 8 ГБ VRAM без большого pagefile ComfyUI падает на VAE.
- Клонируйте этот репозиторий: `git clone https://github.com/SkyLord089/testing.git`

## 1. Предстартовая проверка

```powershell
cd testing\scripts
.\Test-Prerequisites.ps1
# для установки на D:: .\Test-Prerequisites.ps1 -InstallRoot D:\RX580-ComfyUI
```

Скрипт **ничего не меняет**, только печатает отчёт: ОС, видеокарта(ы), драйвер, Python/Git,
найден ли HIP SDK (и какой версии), размер pagefile, свободное место на диске.
Все пункты должны быть «OK» или «WARN» (WARN допустим, FAIL — остановитесь и исправьте).

> [!TIP]
> Если у вас на плате есть встроенная графика AMD (Ryzen iGPU), ZLUDA/HIT могут цепляться за неё.
> В этом случае жёстко выберите RX 580: `set HIP_VISIBLE_DEVICES=0`
> (или отключите iGPU в BIOS / через диспетчер устройств).

## 2. Патчим rocBLAS под gfx803 (нужен админ)

```powershell
.\Patch-RocBLAS-gfx803.ps1 -RocmVersion 6.2
```

Что делает скрипт:

1. Находит `C:\Program Files\AMD\ROCm\6.2\bin` (или ближайший установленный ROCm, если 6.2 не найден;
   при расхождении версии печатает громкое предупреждение).
2. Делает **полные резервные копии**: `rocblas.dll` → `rocblas.dll.zluda-bak`,
   `rocblas\library` → `rocblas\library.zluda-bak`.
3. Скачивает патч [advanced-lvl-up v1.0.0](https://github.com/advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU/releases/download/v1.0.0/rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z)
   (~7 МБ, перекомпилированный Tensile для gfx803) и раскладывает его по местам.
4. Кладёт маркер-файл `rocblas\gfx803.zluda-patched` (дата + источник патча).

> [!WARNING]
> Патч **официально собран под HIP SDK 5.7.1 / 6.2.4**. Применение на 6.4 — экспериментальный сценарий
> (сообщество подтверждает работоспособность; гарантий нет). Если что-то пойдёт не так —
> откат в одну команду (шаг 5).
>
> Если хотите минимальных приключений — поставьте именно **HIP SDK 6.2(.4)**
> (<https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html>) и запустите скрипт с `-RocmVersion 6.2`.
> Процедура идентична.

## 3. Ставим ComfyUI + torch + ZLUDA

```powershell
.\Install-ComfyUI-RX580.ps1 -RocmVersion 6.2
# установка на D:: .\Install-ComfyUI-RX580.ps1 -RocmVersion 6.2 -InstallRoot D:\RX580-ComfyUI
```

Параметры (все опциональны):

| Параметр | По умолчанию | Что делает |
|---|---|---|
| `-InstallRoot` | `%USERPROFILE%\RX580-ComfyUI` | куда всё ставится |
| `-TorchIndexUrl` | `https://download.pytorch.org/whl/cu118` | индекс колес torch |
| `-TorchVersion` | `2.7.0` | версия torch (`2.2.1` — обкатанный запасной) |
| `-SkipRocmCheck` | — | не падать, если скрипт не нашёл HIP SDK |
| `-SkipZludaPatch` | — | не патчить torch\lib (например, вы уже сделали это вручную) |
| `-KeepRocm6Nvcuda` | — | всегда оставить `nvcuda.dll` из rocm6-сборки (по умолчанию так и делается на HIP 6.4) |
| `-SwapRocm5Nvcuda` | — | заменить на `nvcuda.dll` из rocm5-сборки (поведение по умолчанию для HIP 5.7 / 6.2 по рецепту автора патча; на 6.4 — только по этому флагу) |

Что происходит внутри:

1. Клонируется <https://github.com/comfyanonymous/ComfyUI> в `…\RX580-ComfyUI\ComfyUI`.
2. Создаётся venv, ставятся зависимости ComfyUI.
3. Ставится **torch 2.7.0 + cu118** (~2.5 ГБ, дольше всего): `pip install torch==2.7.0 torchvision==0.22.0 torchaudio==2.7.0 --index-url https://download.pytorch.org/whl/cu118`.
4. Скачивается **ZLUDA v3.9.5** (rocm6 + rocm5 сборки) в `…\RX580-ComfyUI\zluda`;
   `nvcuda.dll`: для HIP 5.7 / 6.2 в rocm6-сборке заменяется на rocm5-версию (рецепт автора патча);
   для **HIP 6.4 по умолчанию остаётся rocm6** (консистентный набор rocm6), а рецепт `-SwapRocm5Nvcuda`
   включается вручную. Принудительно: `-KeepRocm6Nvcuda` / `-SwapRocm5Nvcuda`.
5. В `torch\lib` оригиналы библиотек (`cublas64_11.dll`, `cusparse64_11.dll`, `cublasLt64_11.dll`, `nvrtc*.dll`)
   бэкапятся как `*.orig` и заменяются ZLUDA-версиями.
6. В `…\RX580-ComfyUI\run-comfyui.bat` генерируется ярлык запуска с абсолютными путями.
7. Пишется манифест `…\tools\patch-manifest.txt` со списком изменённых файлов (для ручного отката).

> [!NOTE]
> Если на шаге 3 pip ругается и не находит torch 2.7.0+cu118 — проверьте, что Python 64-битный
> (`python -c "import struct;print(struct.calcsize('P')*8)"` → `64`). Если cu118-колеса почему-то не ставятся,
> обкатанный запасной вариант: `-TorchVersion 2.2.1` (именно его пинует ComfyUI-Zluda для RX 580).

## 4. Проверка

```powershell
.\Verify-Setup.ps1
# для установки на D:: .\Verify-Setup.ps1 -InstallRoot D:\RX580-ComfyUI
```

Ожидаемый вывод (пример):

```
torch: 2.7.0+cu118   cuda: 11.8
torch.cuda.is_available(): True
device: AMD Radeon RX 580 Series
rocBLAS patch marker: found (...)
```

## 5. Запуск ComfyUI

```cmd
%USERPROFILE%\RX580-ComfyUI\run-comfyui.bat
REM если ставили на D::  D:\RX580-ComfyUI\run-comfyui.bat
```

Либо из папки: `…\RX580-ComfyUI\zluda\zluda.exe -- …\ComfyUI\venv\Scripts\python.exe main.py --force-fp16`
(где `…` — ваш InstallRoot, например `D:\RX580-ComfyUI`).
Откроется `http://127.0.0.1:8188`.

Первый запуск — **10–30 минут компиляции ядер**, это норма (см. architecture.md).

Рекомендуемые флаги (уже встроены в `run-comfyui.bat`; в конец можно дописывать свои после `--`):

- `--force-fp16` — **обязателен** для Polaris (быстрый fp16)
- `--disable-cuda-malloc` — **фикс fake OOM на torch 2.3+**: ComfyUI по умолчанию включает `backend:cudaMallocAsync` для torch≥2.0+cu*, а ZLUDA его не поддерживает (см. [fake-oom.md](fake-oom.md))
- `--disable-smart-memory` — ComfyUI не гейтит аллокации по `torch.cuda.mem_get_info()` (под ZLUDA врёт)
- `--lowvram` — если 8 ГБ не хватает впритык
- `--cpu-vae` или `--fp32-vae` — если VAE падает/сыплет артефакты
- `--use-pytorch-cross-attention` — если не работает `quad` attention (он удалён в новых версиях ComfyUI)

## 6. Модели

Закиньте чекпоинты в `…\RX580-ComfyUI\ComfyUI\models\checkpoints`.
Для старта на RX 580 берите **SD 1.5** (`sd_xl` — только lowvram; FLUX — только GGUF Q3/Q4).
Хороший тест — SD 1.5, 512×512, 20–25 шагов, Euler a.

## Установка на диск D: (вместо C:)

Всё, кроме HIP SDK, штатно переносится на любой диск **одной опцией** `-InstallRoot`:

```powershell
.\Test-Prerequisites.ps1 -InstallRoot D:\RX580-ComfyUI
.\Install-ComfyUI-RX580.ps1 -InstallRoot D:\RX580-ComfyUI
.\Verify-Setup.ps1 -InstallRoot D:\RX580-ComfyUI
# запуск: D:\RX580-ComfyUI\run-comfyui.bat
```

Что окажется на D: (и очень кстати для тяжёлых SDXL/FLUX-чекпоинтов, они по 6–12 ГБ):

- `D:\RX580-ComfyUI\ComfyUI` — сам ComfyUI, venv (включая torch ~2.5 ГБ)
- `D:\RX580-ComfyUI\zluda` — ZLUDA и её DLL
- `D:\RX580-ComfyUI\ComfyUI\models` — модели, `input`, `output`, `temp`
- `D:\RX580-ComfyUI\tools` — скачанные зипы ZLUDA и `patch-manifest.txt`

Что **останется на C:** (это нормально и не обходится):

- **HIP SDK** — ставится AMD, обычно в `C:\Program Files\AMD\ROCm\6.2` (рекомендуемая версия; 6.4 нежизнеспособна — см. заголовок), патч rocBLAS применяется там же (шаг 2); на скорость/расположение ComfyUI это не влияет
- сам **Python** и **Git** — ставятся с python.org / git-scm.com куда вы скажете (для минимизации C: можно поставить их тоже на D:, но чаще они занимают < 500 МБ)
- **pagefile** — Система → Виртуальная память: можно перенести/разделить на D:, на RX 580 с 8 ГБ VRAM это даже полезнее (держите суммарно ≥ 16 ГБ)
- `%TEMP%` — временные мелкие файлы (~7 МБ для патча), переносить смысла нет

Нюансы:

- Диск D: должен быть NTFS. Проверить свободно ≥ 25 ГБ: `.\Test-Prerequisites.ps1 -InstallRoot D:\RX580-ComfyUI`.
- `run-comfyui.bat` содержит абсолютные пути. Если потом перевезёте папку в другое место —
  просто поправьте две строки (`ZLUDA_DIR` и `COMFY_DIR`) в `run-comfyui.bat`, переустанавливать не нужно.
- `git clone testing.git` можно держать где угодно (например, `D:\tools\testing`) — на итоговую установку
  влияют только флаги `-InstallRoot`, и скрипты не завязаны на то, откуда они запущены.

## 7. Полный откат

```powershell
# Удалить всё ComfyUI/ZLUDA (подставьте свой InstallRoot; пример для D:)
rmdir /s /q "D:\RX580-ComfyUI"

# Вернуть rocBLAS (админ)
cd testing\scripts
.\Restore-RocBLAS.ps1 -RocmVersion 6.4

# Патч torch\lib откатывается вручную: *.orig → исходные имена
# (список затронутых файлов — в <InstallRoot>\tools\patch-manifest.txt)
```

HIP SDK с системы не удаляем — он ставился штатным деинсталлятором AMD и патч rocBLAS откатывается шагом выше.
