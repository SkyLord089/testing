# Пошаговая установка: ComfyUI на RX 580 @ Windows 11 (HIP SDK 6.4 + torch 2.7.0+cu118)

Время: ~1–2 часа (большая часть — скачивание) + 10–30 минут первой компиляции при первом запуске.

## 0. Что нужно заранее

- Windows 11 (10 тоже подойдёт, скрипты не используют ничего 11-специфичного, кроме `tar.exe`, который есть и там).
- AMD Adrenalin — **свежий** драйвер для Polaris (PRO не нужен). Поставьте до начала.
- **HIP SDK 6.4** — ставится **вручную**: <https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html>
  (официальный инсталлятор, нужен аккаунт AMD / согласие с EULA — поэтому скрипт его не ставит сам).
  После установки проверьте, что есть `C:\Program Files\AMD\ROCm\6.4\bin\rocblas`.
- Git — <https://git-scm.com/download/win> (при установке отметьте "Git from the command line").
- Python **3.11** с python.org <https://www.python.org/downloads/windows/> — отмечайте **"Add Python to PATH"**.
  НЕ ставьте питон из Microsoft Store. 3.10–3.12 тоже подойдут.
- Visual C++ Redistributable x64 — <https://aka.ms/vs/17/release/vc_redist.x64.exe> (обычно уже есть).
- ~25 ГБ свободного места и **pagefile ≥ 16 ГБ** (Система → Дополнительные параметры → Быстродействие →
  Виртуальная память). На 8 ГБ VRAM без большого pagefile ComfyUI падает на VAE.
- Клонируйте этот репозиторий: `git clone https://github.com/SkyLord089/testing.git`

## 1. Предстартовая проверка

```powershell
cd testing\scripts
.\Test-Prerequisites.ps1
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
.\Patch-RocBLAS-gfx803.ps1 -RocmVersion 6.4
```

Что делает скрипт:

1. Находит `C:\Program Files\AMD\ROCm\6.4\bin` (или ближайший установленный ROCm, если 6.4 не найден;
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
.\Install-ComfyUI-RX580.ps1
```

Параметры (все опциональны):

| Параметр | По умолчанию | Что делает |
|---|---|---|
| `-InstallRoot` | `%USERPROFILE%\RX580-ComfyUI` | куда всё ставится |
| `-TorchIndexUrl` | `https://download.pytorch.org/whl/cu118` | индекс колес torch |
| `-TorchVersion` | `2.7.0` | версия torch (`2.2.1` — обкатанный запасной) |
| `-SkipRocmCheck` | — | не падать, если скрипт не нашёл HIP SDK |
| `-SkipZludaPatch` | — | не патчить torch\lib (например, вы уже сделали это вручную) |

Что происходит внутри:

1. Клонируется <https://github.com/comfyanonymous/ComfyUI> в `…\RX580-ComfyUI\ComfyUI`.
2. Создаётся venv, ставятся зависимости ComfyUI.
3. Ставится **torch 2.7.0 + cu118** (~2.5 ГБ, дольше всего): `pip install torch==2.7.0 torchvision==0.22.0 torchaudio==2.7.0 --index-url https://download.pytorch.org/whl/cu118`.
4. Скачивается **ZLUDA v3.9.5** (rocm6 + rocm5 сборки) в `…\RX580-ComfyUI\zluda`;
   `nvcuda.dll` в rocm6-сборке заменяется на `nvcuda.dll` из rocm5-сборки (так советует автор патча для SDK 6.x).
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
```

Либо из папки: `…\RX580-ComfyUI\zluda\zluda.exe -- …\ComfyUI\venv\Scripts\python.exe main.py --force-fp16`.
Откроется `http://127.0.0.1:8188`.

Первый запуск — **10–30 минут компиляции ядер**, это норма (см. architecture.md).

Рекомендуемые флаги (передаются в `run-comfyui.bat` после `--`):

- `--force-fp16` — **обязателен** для Polaris (быстрый fp16)
- `--lowvram` — если 8 ГБ не хватает впритык
- `--cpu-vae` или `--fp32-vae` — если VAE падает/сыплет артефакты
- `--use-pytorch-cross-attention` — если не работает `quad` attention (он удалён в новых версиях ComfyUI)
- `--disable-smart-memory` — исторический флаг, помогает на старых версиях ComfyUI

## 6. Модели

Закиньте чекпоинты в `…\RX580-ComfyUI\ComfyUI\models\checkpoints`.
Для старта на RX 580 берите **SD 1.5** (`sd_xl` — только lowvram; FLUX — только GGUF Q3/Q4).
Хороший тест — SD 1.5, 512×512, 20–25 шагов, Euler a.

## 7. Полный откат

```powershell
# Удалить всё ComfyUI/ZLUDA
rmdir /s /q "%USERPROFILE%\RX580-ComfyUI"

# Вернуть rocBLAS (админ)
cd testing\scripts
.\Restore-RocBLAS.ps1 -RocmVersion 6.4

# Патч torch\lib откатывается вручную: *.orig → исходные имена
# (список затронутых файлов — в …\RX580-ComfyUI\tools\patch-manifest.txt)
```

HIP SDK с системы не удаляем — он ставился штатным деинсталлятором AMD и патч rocBLAS откатывается шагом выше.
