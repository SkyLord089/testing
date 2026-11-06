# Диагностика: симптом → причина → что проверить

Держите этот файл открытым во время экспериментов. Для каждого симптома указан **слой** (см. `00-stack-map.md`), **чем подтвердить** и **куда идти** лечить.

---

## Таблица

| # | Симптом | Слой | Чем подтвердить | Лечение |
|---|---|---|---|---|
| 1 | `hipInfo.exe` → `no ROCm-capable device is detected` | драйвер/HIP | `hipInfo.exe` из папки нужного HIP SDK | `01-diagnosis.md#1`, этап 0 в `03-fix-plan.md` |
| 2 | `torch.cuda.is_available()` → `False`, «CUDA device detected: None» | ZLUDA↔HIP | лог `AMD_LOG_LEVEL=3` (see below) | `#2` |
| 3 | `RuntimeError: No CUDA GPUs are available` уже на старте ComfyUI | ZLUDA↔HIP | то же | `#2` |
| 4 | `OSError: [WinError 1114]`/`WinError 127` при импорте torch | DLL-связка | `dumpbin /dependents` или Process Monitor | `#4` |
| 5 | `OSError: [WinError 126]` на `cublas64_11.dll`/`cublasLt.dll` | DLL-связка | есть ли `hipblaslt.dll` рядом/в PATH | `#4` |
| 6 | `CUDA error: CUBLAS_STATUS_NOT_SUPPORTED` в `cublasLtMatmulAlgoGetHeuristic` | torch 2.6+/ZLUDA | стек: `addmm` → `cublasLt*` | `06-torch27.md#1` |
| 7 | `CUBLAS_STATUS_NOT_SUPPORTED` в `cublasSgemm` | rocBLAS gfx803 | ошибка `TensileLibrary...gfx803` в логе | `05-gfx803-rocblas.md` |
| 8 | `rocBLAS error: Cannot read ... for GPU arch : gfx803` | rocBLAS/Tensile | листинг `rocblas\library` | `05-gfx803-rocblas.md#a` |
| 9 | Картинка «серая»/мусор/NA при работающих GEMM | Tensile-ядра | сравнение с CPU (скрипт `verify-torch27.py`, секция `accuracy`) | `05-gfx803-rocblas.md#b` |
| 10 | «Compilation is in progress. Please wait...» бесконечно | ZLUDA кэш/компиляция | `ZLUDA_COMGR_LOG_LEVEL=1`, размер `ZLUDA_CACHE_DIR` | `#10` |
| 11 | Зависание/чёрный экран/перезагрузка драйвера при VAE decode | VRAM/Tensile | `--novram`, tiled VAE, мониторинг VRAM | `04-rebuild-zluda.md#e` |
| 12 | ComfyUI падает на первой же генерации после апдейта | ComfyUI 0.4x + torch 2.7 | `--disable-async-offload --disable-pinned-memory` | `06-torch27.md#3` |
| 13 | `miopenStatusNotImplemented` / `GET was unable to find an engine` | MIOpen/gfx803 | включён ли `torch.backends.cudnn` | `03-fix-plan.md` этап 3 |
| 14 | `ValueError: Unknown CUDA arch (8.8+PTX)` | compute capability | ZLUDA рапортует CC 8.8 | `04-rebuild-zluda.md#d` |

---

## 1. HIP не видит карту (`hipErrorNoDevice`)

**Проверить по порядку:**

1. `C:\Program Files\AMD\ROCm\6.4\bin\hipInfo.exe` — видит ли `gcnArchName: gfx803`.
2. `C:\Program Files\AMD\ROCm\6.2\bin\hipInfo.exe` — для контроля: если 6.2 видит, а 6.4 нет, дело в HIP 6.4 + драйвер.
3. `where amdhip64_6.dll` — какая копия грузится. **Порядок поиска DLL в Windows**: каталог exe → System32 → PATH. Если в каталоге запуска (`C:\ComfyUI-Zluda\` или рядом с python.exe) лежит старая `amdhip64_6.dll` — она перекроет System32.
4. Версия драйвера: `Get-WmiObject Win32_VideoController | select Name,DriverVersion,DriverDate`.
5. Убедиться, что нет `HIP_VISIBLE_DEVICES` со мусором (`all`, пустая строка, несуществующий индекс). **`HIP_VISIBLE_DEVICES=""` = ни одного устройства.** Это очень частая причина «No CUDA GPUs are available».
6. Убедиться, что не установлено несколько HIP SDK и `HIP_PATH` указывает на нужный, а PATH не содержит `...\ROCm\5.7\bin` **раньше** `6.4`.

**Команды:**

```powershell
# какие DLL реально видны
where.exe amdhip64_6.dll
where.exe amd_comgr.dll
where.exe rocblas.dll

# переменные, которые ломают перечисление
Get-ChildItem Env: | Where-Object Name -match 'HIP|ROCM|HSA|CUDA|ZLUDA|AMD' | Format-Table -AutoSize

# подробный лог HIP
$env:AMD_LOG_LEVEL=3
& 'C:\Program Files\AMD\ROCm\6.4\bin\hipInfo.exe' 2>&1 | Select-Object -First 40
```

В логе ищите строки `hipGetDeviceCount: Returned hipErrorNoDevice` — это подтверждает слой драйвера, а не ZLUDA.

---

## 2. ZLUDA не видит устройство (при работающем `hipInfo`)

**Признак:** `hipInfo` видит gfx803, а `python -c "import torch; print(torch.cuda.is_available())"` → `False`.

1. Проверить, что в `venv\Lib\site-packages\torch\lib` **подменены** `cublas64_11.dll`, `cusparse64_11.dll`, `cufft64_10.dll`, `cufftw64_10.dll`, `nvrtc64_112_0.dll` — и что `nvcuda.dll` рядом (или запуск через `zluda\zluda.exe --`).
2. `ZLUDA_COMGR_LOG_LEVEL=1` + `AMD_LOG_LEVEL=3` — если в логе нет вообще упоминаний HIP, значит `amdhip64_6.dll` не загрузилась (не тот PATH / не та разрядность / битая).
3. Разрядность: python 64-bit ↔ все DLL 64-bit. Проверить: `python -c "import platform;print(platform.architecture())"`.
4. Попробовать запуск строго через `zluda\zluda.exe -- venv\Scripts\python.exe -c "import torch;print(torch.cuda.get_device_name())"`.
5. Проверить, что `torch` — именно `+cu118`, а не `+cu126/+cu128` (последние тянут другой набор DLL и другой путь в ZLUDA):
   `python -c "import torch;print(torch.__version__, torch.version.cuda)"`.

---

## 4. DLL-связки (`WinError 126/127/1114`, «Bad Image»)

«Bad Image» = DLL найдена, но не может быть загружена (несовместимая зависимость или повреждена). Для ZLUDA почти всегда это:

- `cublasLt.dll` (ZLUDA) → `hipblaslt.dll` (HIP SDK) **отсутствует или другой версии**;
- `cublas.dll` (ZLUDA) → `rocblas.dll` **не той версии** (например, оставили от 5.7 при HIP 6.4);
- `cudnn.dll` (ZLUDA nightly) → `miopen.dll` не найдена/не та;
- подмешались x86-DLL или DLL от другой версии HIP (5.7) в PATH.

**Быстрая проверка зависимостей без Visual Studio:**

```powershell
# dumpbin из Build Tools, если установлены:
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find **\dumpbin.exe

# либо через PowerShell + System.Reflection.Metadata (или просто Process Monitor)
```

Самое надёжное — **Process Monitor** (Sysinternals), фильтр `Process Name is python.exe` + `Result is NAME NOT FOUND`. Сразу видно, какую DLL ищут и откуда.

**Правило:** в `venv\torch\lib` должны лежать только DLL **одной** версии ZLUDA и **одной** версии HIP SDK. Смесь 5.7+6.4 = гарантированные странности.

---

## 10. Бесконечное «Compilation is in progress»

ZLUDA компилирует PTX в AMDGCN при первом запуске каждого ядра. На gfx803 это долго (десятки минут), и выглядит как зависание.

- `ZLUDA_COMGR_LOG_LEVEL=1` — видно, что компиляция идёт;
- `ZLUDA_CACHE_DIR` на NVMe (по умолчанию кэш рядом/в профиле);
- после **любой** смены rocBLAS/hipBLASLt/драйвера кэш лучше чистить (иначе смесь старых и новых ядер);
- если компиляция падает по памяти — уменьшить `-j` нельзя, но можно уменьшить разрешение/число потоков загрузки модели, чтобы процесс не свопился.

---

## Что присылать в чат

1. `preflight-report.txt` (из `scripts/preflight.ps1`).
2. Вывод `verify-torch27.py` целиком.
3. Последние 60 строк лога ComfyUI **от строки `:: ------------------------ ZLUDA -----------------------`**.
4. Точный стектрейс ошибки (первые 15 и последние 15 строк).
