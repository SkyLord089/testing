# Установка с нуля: RX 580 + Windows 11 + ComfyUI-ZLUDA

Документ для **чистой машины**: ничего не установлено. Идём по шагам, после каждого есть проверка. Все скрипты лежат в `scripts\install\`.

---

## 0. Выбрать профиль

| | **Профиль `safe`** | **Профиль `target`** |
|---|---|---|
| HIP SDK | 6.2.4 | **6.4.2** |
| ZLUDA | 3.9.5 `ZLUDA-windows-rocm6-amd64` | **3.9.6 `ZLUDA-windows-amd64`** |
| torch | 2.7.0+cu118 | 2.7.0+cu118 |
| Ядра rocBLAS gfx803 | **есть готовые** (community, 6.2.4) | нет; надо добывать (см. `05-gfx803-rocblas.md`) |
| Шанс «заработает сегодня» | высокий | средний |
| Смысл | получить рабочую систему и точку отсчёта | добить цель: 6.4 + 2.7 |

**Рекомендация:** ставь `target` (ты этого и хочешь), но если на этапе 5 не найдёшь ядра gfx803 под 6.4 — не геройствуй, переставь HIP на 6.2.4 (`-Profile safe`) и работай. Переключение HIP занимает 15 минут и обратимо.

Почему так: `rocBLAS` для Polaris есть только в наборах 5.7.1/6.2.4. В релизах под 6.4.2 и 7.1.1 (включая свежий `v0.7.1.1` от likelovewant) архитектуры `gfx803` **нет** — это проверено по спискам ассетов.

---

## 1. Что нужно скачать (ссылки проверены)

| Файл | Ссылка | Размер |
|---|---|---|
| **HIP SDK 6.4.2** | `https://download.amd.com/developer/eula/rocm-hub/AMD-Software-PRO-Edition-25.Q3-Win10-Win11-For-HIP.exe` | ~1.7 ГБ |
| HIP SDK 6.2.4 (fallback) | `https://download.amd.com/developer/eula/rocm-hub/AMD-Software-PRO-Edition-24.Q4-Win10-Win11-For-HIP.exe` | ~1.6 ГБ |
| **Драйвер Polaris (RX 580)** | страница AMD → Radeon RX 580 → Adrenalin (Polaris/Vega ветка). Актуальный на середину 2026: **26.5.2**; проверенно рабочие с HIP: **25.8.1**, **24.9.1** | ~620 МБ |
| **ZLUDA 3.9.6** | `https://github.com/lshqqytiger/ZLUDA/releases/download/rel.854c58e1565c3597e17046d7cc2b1eab96fcdf55/ZLUDA-windows-amd64.zip` | 6 МБ |
| ZLUDA 3.9.5 (для `safe`, HIP 6.2) | `https://github.com/lshqqytiger/ZLUDA/releases/download/rel.5e717459179dc272b7d7d23391f0fad66c7459cf/ZLUDA-windows-rocm6-amd64.zip` | 5 МБ |
| **Ядра rocBLAS gfx803** | `https://github.com/advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU/releases/download/v1.0.0/rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z` | 6 МБ |
| Python 3.11.9 x64 | `https://www.python.org/ftp/python/3.11.9/python-3.11.9-amd64.exe` | 25 МБ |
| Git for Windows | `https://git-scm.com/download/win` | 60 МБ |
| 7-Zip (или использовать встроенный `tar.exe`) | `https://www.7-zip.org/` | 1.5 МБ |

> ⚠️ Не бери HIP SDK **7.1.1** для Polaris. Даже на официально поддерживаемых картах (RX 6800 XT) есть свежие репорты, что 7.1.1 не перечисляет устройство, а 6.4 — перечисляет. Для наших целей 7.1.1 — лишний риск.

---

## 2. Порядок установки (важен!)

```
драйвер → Python/Git/7-Zip → HIP SDK → ComfyUI-Zluda (venv + torch) → ZLUDA → ядра gfx803 → проверки
```

Драйвер **до** HIP SDK: установщик HIP приносит свой (старый PRO-драйвер), и если поставить его последним, он может откатить свежий.

---

## 3. Этапы скриптами

Всё запускается из PowerShell. Скрипты идемпотентны: повторный запуск не ломает уже сделанное.

Есть оркестратор — он прогоняет этапы по порядку и останавливается там, где нужно руками:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\00-all.ps1 -Profile target
```

Отдельные этапы — ниже (так понятнее, если что-то пошло не так).

### Этап 0 - предпосылки (Python, 7-Zip)

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\00a-prereq-install.ps1
```

Ставит тихо, **без прав администратора** (в профиль пользователя):

| Что | Версия | Куда |
|---|---|---|
| Python | 3.11.9 x64 | `%LOCALAPPDATA%\Programs\Python\Python311` |
| 7-Zip | 26.05 | `C:\Program Files\7-Zip` (или в профиль, если без админа) |

Установщики качаются на диск кита (D:), на C: не оседают.

> **После этого обязательно закройте окно PowerShell и откройте новое.**
> Иначе `PATH` в текущем процессе останется старым и `python` не найдётся.

**Про заглушку Microsoft Store.** Проверяйте `Get-Command python` (в PowerShell
`where` — это алиас `Where-Object`, а не `where.exe`, и он молча ничего не
напечатает). Если `Get-Command python` показывает
`...\WindowsApps\python.exe` — это не Python, а ярлык на Store. Настоящий
python.org встаёт в PATH выше по приоритету и обычно перекрывает её. Если
Store всё равно открывается:

*Параметры → Приложения → Дополнительные компоненты → Управление псевдонимами
выполнения приложений* → выключить `python.exe` и `python3.exe`.

**7-Zip не обязателен.** Если его не хочется ставить, скрипт `05` сам
распакует `.7z` через python-модуль `py7zr` (поставит его автоматически).
7-Zip нужен только для удобства и ускорения распаковки.

---

### Этап 1 - проверка окружения

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\01-prereq-check.ps1
```

Проверяет: Windows 11 и сборку, права администратора, свободное место (нужно ≥25 ГБ), наличие Python/Git/7-Zip, видеокарту и VRAM, размер файла подкачки, «залежи» старых ROCm в PATH.
**Выход:** таблица `OK/WARN/FAIL` и список того, что доустановить.

### Этап 2 - драйвер

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\02-driver.ps1 -OpenPage
```

Скрипт открывает страницу драйверов RX 580, ждёт установки и проверяет версию + наличие `amdhip64_6.dll`.
Ставить: **Minimal Install** или **Driver Only** (Adrenalin-центр не нужен). После установки — **перезагрузка**.

> Если после установки HIP `hipInfo` не видит карту — вернись сюда и поставь **25.8.1** (Polaris), затем **24.9.1**. Это единственный «магический» параметр во всей схеме.

### Этап 3 - HIP SDK

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\03-install-hip.ps1 -HipVersion 6.4.2
```

Скрипт скачивает установщик, **открывает GUI** и просит снять галочку с драйвера (`AMD Software: PRO Edition` → **None**), потому что драйвер мы поставили на этапе 2.
Можно и молча: `-Silent` (но тогда установщик поставит свой PRO-драйвер — потом переустанови драйвер из этапа 2).

Проверка после:
```powershell
& 'C:\Program Files\AMD\ROCm\6.4\bin\hipInfo.exe' | Select-String 'gfx|Name|Marketing'
```
Ожидаем увидеть `gfx803` и `Radeon RX 580`.

> **Если `hipErrorNoDevice`:** см. `docs\01-diagnosis.md#1`. Порядок: убрать чужие ROCm из PATH → проверить `HIP_VISIBLE_DEVICES` (пустая строка = ноль устройств) → другой драйвер.

### Этап 4 - ComfyUI-Zluda + torch 2.7 + ZLUDA

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\04-comfyui-zluda.ps1 -InstallDir C:\AI\ComfyUI-Zluda -Profile target
```

Что делает:
1. `git clone` репозитория `patientx/ComfyUI-Zluda` (если папки нет);
2. создаёт `venv`, обновляет pip;
3. ставит `torch==2.7.0 torchvision==0.22.0 torchaudio==2.7.0` с индекса **cu118**;
4. ставит `requirements.txt`, `numpy==1.26.4`, `onnxruntime`;
5. **не** ставит Triton/sageattention/flash-attn — на gfx803 они бесполезны, а без MSVC Build Tools просто не соберутся;
6. скачивает ZLUDA 3.9.6 → `zluda\`;
7. вызывает `patch-torch-zluda.ps1`:
   * снимает копию **оригинального** `nvrtc64_112_0.dll` → `nvrtc_cuda.dll` (так же, как `install-n.bat`);
   * с бэкапом подменяет `cublas64_11`, `cusparse64_11`, `cufft64_10`, `cufftw64_10`, `nvrtc64_112_0`;
   * **не** трогает `cublasLt*` — это принципиально для gfx803;
8. копирует `comfy\customzluda\zluda.py` → `comfy\zluda.py` (штатный патч ComfyUI под ZLUDA: снимает ROCm из PATH, ставит `DISABLE_ADDMM_CUDA_LT`, гасит cudnn/flash_sdp, чинит topk/stft/onnx);
9. кладёт свой лаунчер `comfyui-user.bat`.

### Этап 5 - ядра rocBLAS для gfx803

> **Скачать архив заранее** (6 МБ):
> ```powershell
> curl.exe -L -o .\downloads\rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z `
>   https://github.com/advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU/releases/download/v1.0.0/rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z
> ```


```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\05-gfx803-rocblas.ps1 -HipVersion 6.4.2 -Archive .\downloads\rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z -ListOnly
```
Сначала `-ListOnly` — просто посмотреть, что в архиве:
```
gfx803/Kernels.so-000-gfx803.hsaco
gfx803/TensileLibrary_*.dat
rocblas.dll
...
```
Потом установка (с бэкапом; `-Clean` затирает старые `TensileLibrary*`/`Kernels*`, чтобы не смешивать наборы):
```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\05-gfx803-rocblas.ps1 -HipVersion 6.4.2 -Archive .\downloads\rocm.gfx800-gfx900-for.hip.sdk.5.7.1-and-6.2.4.7z -Clean
```

Скрипт делает `rocblas.bak-<timestamp>` рядом с `library`, копирует только файлы с `gfx803` в имени, и **не** трогает `rocblas.dll`, если не указан `-WithRocblasDll` (родной DLL от 6.4 важнее, чем DLL из архива 6.2).

Варианты, если для 6.4 ядра «не зашли» (ошибка `Cannot read TensileLibrary` / мусор вместо картинки):
- **B** — взять ядра из того же архива, но это ядра 6.2.4 → на 6.4 могут не подойти (Tensile ABI). Проверяется только опытом.
- **C** — собрать Tensile под 6.4 (см. `docs\05-gfx803-rocblas.md`).
- **Fallback** — автоматически: `powershell -ExecutionPolicy Bypass -File .\scripts\install\90-fallback-hip62.ps1`.
  Он удалит 6.4, поставит 6.2.4, переведёт ZLUDA на 3.9.5-rocm6, поставит ядра 6.2.4 и **оставит torch 2.7.0+cu118**.
  Вручную то же самое: `03 -HipVersion 6.2.4` → `04 -Profile safe` → `05 -HipVersion 6.2.4`.

### Этап 6 - проверка и первый запуск

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\06-verify.ps1 -InstallDir C:\AI\ComfyUI-Zluda
```

Скрипт проверяет всю цепочку и печатает `PASS/WARN/FAIL`:
HIP-каталог и `hipInfo` (gfx803), наличие ядер `*gfx803*`, версию torch, какие DLL в `torch\lib`
подменены (маленькие = ZLUDA), что `cublasLt` **не** подменён, `nvrtc_cuda.dll`, актуальность
`comfy\zluda.py`, `torch.cuda.is_available()`, имя устройства с `[ZLUDA]`, GEMM fp32/fp16 и
`addmm` с bias со сверкой с CPU.

Потом — `verify-torch27.py` (детальный тест операций) и запуск:

```powershell
cd C:\AI\ComfyUI-Zluda
.\venv\Scripts\python.exe C:\path\to\repo\scripts\verify-torch27.py
.\comfyui-user.bat
```

Ожидаемый «здоровый» вывод `verify-torch27.py`: все секции `PASS`, `ZLUDA detected`, `mm fp16` даёт ненулевой TFLOP/s (для RX 580 ориентир — единицы TFLOP/s).

---

## 4. Чек-лист «всё стоит правильно»

- [ ] `hipInfo.exe` → `gfx803`, `Radeon RX 580`
- [ ] в `C:\Program Files\AMD\ROCm\6.4\bin\rocblas\library` есть файлы `*gfx803*`
- [ ] `venv\Lib\site-packages\torch\lib\cublas64_11.dll` весит **мегабайты**, а не ~100 МБ (значит подменён на ZLUDA)
- [ ] `venv\Lib\site-packages\torch\lib\cublasLt*.dll` — **оригинальный** (не ZLUDA) или удалён
- [ ] `venv\Lib\site-packages\torch\lib\nvrtc_cuda.dll` — **оригинальный** nvrtc (≥ 10 МБ), а не ZLUDA-шим
- [ ] `comfy\zluda.py` совпадает с `comfy\customzluda\zluda.py`
- [ ] `DISABLE_ADDMM_CUDA_LT=1` в лаунчере
- [ ] `python -c "import torch;print(torch.cuda.get_device_name(0))"` → `... [ZLUDA]`
- [ ] `verify-torch27.py` без `FAIL`

---

## 5. Типовые провалы на чистой машине и что делать

| Симптом | Причина | Действие |
|---|---|---|
| `python не найден` | Python не в PATH или из Microsoft Store | переустановить с python.org, галочка «Add python.exe to PATH» |
| `hipInfo` нет устройства | драйвер/переменные | `01-diagnosis.md#1` |
| `torch.cuda.is_available() = False` | ZLUDA-подмена не сделана или PATH | `04` перезапустить, проверить `torch\lib` |
| `WinError 126` на `cublasLt` | в `torch\lib` попал ZLUDA-`cublasLt` | удалить его из `torch\lib`, перезапустить `04` |
| `CUBLAS_STATUS_NOT_SUPPORTED` | cublasLt-путь torch | `DISABLE_ADDMM_CUDA_LT=1` + `06-torch27.md#1` |
| `Cannot read TensileLibrary` | нет ядер gfx803 | этап 5 / fallback на 6.2.4 |
| Картинка серая/мусор | чужие Tensile-ядра | этап 5 другой вариант, сверка с CPU |
| Зависание на VAE | 8 ГБ VRAM | `--novram`, tiled VAE |

---

## 6. Если совсем не хочется PowerShell

Есть «ручной» путь — ровно то же самое, но руками, за ~40 минут:

1. Python 3.11.9 (Add to PATH) + Git + 7-Zip.
2. Драйвер RX 580 (Polaris) — Minimal/Driver Only, перезагрузка.
3. HIP SDK 6.4.2 — GUI, **снять** компонент драйвера, пути по умолчанию, перезагрузка.
4. `git clone https://github.com/patientx/ComfyUI-Zluda C:\AI\ComfyUI-Zluda`
5. В `C:\AI\ComfyUI-Zluda`: `python -m venv venv`, `venv\Scripts\activate`, затем
   `pip install torch==2.7.0 torchvision==0.22.0 torchaudio==2.7.0 --index-url https://download.pytorch.org/whl/cu118`,
   `pip install -r requirements.txt`, `pip install numpy==1.26.4 onnxruntime`.
6. Скачать `ZLUDA-windows-amd64.zip` (3.9.6) → распаковать в `C:\AI\ComfyUI-Zluda\zluda`.
7. Скопировать (с бэкапом!) из `zluda\` в `venv\Lib\site-packages\torch\lib\`:
   `cublas.dll→cublas64_11.dll`, `cusparse.dll→cusparse64_11.dll`, `cufft.dll→cufft64_10.dll`, `cufftw.dll→cufftw64_10.dll`, `nvrtc.dll→nvrtc64_112_0.dll`.
   **`cublasLt.dll` не копировать.**
8. Ядра gfx803: распаковать `.7z`, файлы `*gfx803*` → `C:\Program Files\AMD\ROCm\6.4\bin\rocblas\library` (сначала бэкап папки).
9. Запуск: `scripts\comfyui-user.bat` (положить в корень ComfyUI-Zluda).

---

## 7. Кодировки: почему скрипты иногда падают с `Unexpected token`

Если вы отредактировали `.ps1` в чужом редакторе и получили такое:

```
At .\scripts\install\01-prereq-check.ps1:45 char:24
+ $note = 'РћР¶РёРґР°РµС‚СЃСЏ Windows 11; ...
+                        ~~~~
Unexpected token 'СЃСЃ' in expression or statement.
```

**Это не ошибка в скрипте.** Кракозябры `РћР¶РёРґР°РµС‚СЃСЏ` — это UTF-8, прочитанный как CP1251.
Windows PowerShell 5.1 определяет кодировку `.ps1` так:

| BOM в файле | Как читает PS 5.1 | Результат |
|---|---|---|
| UTF-8 BOM (`EF BB BF`) | UTF-8 | корректно |
| UTF-16 LE/BE BOM | UTF-16 | корректно |
| **BOM нет** | **ANSI (CP1251)** | **кракозябры и parse-ошибки** |

Кавычки внутри русских строк после такого «декодирования» перестают быть
кавычками — парсер видит обрыв строки и сыпет `Unexpected token` пачками.

### Правила для файлов репозитория

| Тип | Кодировка | BOM | EOL |
|---|---|---|---|
| `*.ps1` | UTF-8 | **есть** | CRLF |
| `*.bat` | **CP866** | **нет** | CRLF |
| `*.py` | UTF-8 | нет | LF |
| `*.md` | UTF-8 | нет | LF |

`.bat` — отдельная история: `cmd.exe` читает файл в кодировке консоли (CP866)
и **не понимает BOM** — он попадёт в первую строку и сломает `@echo off`.
Поэтому в `.bat` русский текст допустим только в CP866, а `git` для них
помечен `-text`, чтобы ничего не перекодировать.

### Проверка и починка

```powershell
# проверить все скрипты
powershell -ExecutionPolicy Bypass -File .\scripts\check-encoding.ps1

# починить: добавить BOM и привести EOL к CRLF
powershell -ExecutionPolicy Bypass -File .\scripts\check-encoding.ps1 -Fix
```

### Если правите в VS Code

В правом нижнем углу должно быть **UTF-8 with BOM** для `.ps1`.
Если стоит просто `UTF-8` — нажмите на неё → *Save with Encoding* → `UTF-8 with BOM`.
Для `.bat` — наоборот, `Cyrillic (Windows 866)` без BOM.

> Обходной путь без правки файлов: запускать через PowerShell 7 (`pwsh`),
> он читает UTF-8 без BOM корректно. Но на чистой Windows 11 `pwsh` нет,
> поэтому основной путь — BOM.

---

## 8. Раскладка по дискам: что можно унести с C:

Если на `C:` мало места (типичная картина: 20 ГБ из 120), а ComfyUI хочется
поставить на `D:`, вот что реально переносится, а что нет.

### Что куда

| Компонент | Куда | Размер | Можно ли унести с C: |
|---|---|---|---|
| ComfyUI + `venv` + **torch 2.7+cu118** | диск кита (`D:\...\ComfyUI-Zluda`) | ~5 ГБ | **да** |
| ZLUDA (6 DLL) + кэш ядер | рядом с ComfyUI | ~1–10 ГБ | **да** |
| Модели (checkpoints, VAE, LoRA) | любой диск | десятки ГБ | **да** (`extra_model_paths.yaml`) |
| Архивы: установщик HIP, ядра rocBLAS | `D:\...\downloads` | ~1.7 ГБ | **да** |
| pip-кэш (колёса torch) | `D:\...\_cache\pip` | ~3 ГБ | **да** (скрипт `04` уже так делает) |
| Python 3.11, Git, 7-Zip | `C:` или `D:` | ~700 МБ | да, но смысла мало |
| **HIP SDK** | `C:\Program Files\AMD\ROCm` | ~4 ГБ | **нет** |
| Драйвер AMD | `C:\Windows\System32` | ~700 МБ | нет |

### Про HIP SDK: почему только C:

- Установщик AMD **не даёт** выбрать диск для самого SDK. Поле
  «Install location» в GUI меняет путь только для компонента
  *AMD Software: PRO Edition* (драйвера), а HIP SDK всегда идёт
  в `C:\Program Files\AMD\ROCm\<версия>`.
- При установке он распаковывает пакеты в `C:\AMD` (~3 ГБ) и **удаляет их
  после завершения**. Так что «постоянно занято» — это ~4 ГБ, а не 7.
- Теоретически можно поставить и потом перенести папку + переписать
  переменные `HIP_PATH` и `PATH`. **Не советую**: в реестре и в
  `PATH` останутся старые пути, а `hipInfo`/`amdhip64.dll` начнут
  искаться не там. Если очень нужно — только через `mklink /J`.

### Итоговые требования к дискам

| Диск | Нужно свободно | На что |
|---|---|---|
| `C:` | **~9 ГБ** | HIP SDK 4 + распаковка установщика 3 + драйвер/мелочь 2 |
| диск ComfyUI | **~35 ГБ** | torch+venv 5, кэш ZLUDA 2–10, pip-кэш 3, место под модели |

Если на `C:` меньше 9 ГБ — сначала освободи. Что съедает место чаще всего:

```powershell
# топ-20 самых больших файлов на C:
Get-ChildItem C:\ -Recurse -File -ErrorAction SilentlyContinue |
  Sort-Object Length -Descending | Select-Object -First 20 FullName, @{n='GB';e={[math]::Round($_.Length/1GB,2)}}
```

Типичные кандидаты: `C:\Windows\Installer`, `C:\Windows\WinSxS` (чистится
`Dism /Online /Cleanup-Image /StartComponentCleanup`), спящий режим
(`powercfg /h off` освобождает 8–16 ГБ), точки восстановления,
`C:\Users\<вы>\AppData\Local\pip`.

> **Про файл подкачки.** У вас 16 ГБ RAM и (возможно) 4 ГБ VRAM — подкачку
> с C: уносить не надо, наоборот: 16 ГБ подкачки нужно оставить.
> Если переносите её на D:, система может не суметь записать дамп, но
> для ComfyUI это не критично.

### Как запускать с учётом раскладки

Все скрипты по умолчанию ставят ComfyUI **рядом с китом**, то есть на
тот же диск, где лежит репозиторий. То есть положили кит в
`D:\Apps\rx-580-comfyui` — ComfyUI встанет в
`D:\Apps\rx-580-comfyui\ComfyUI-Zluda`, архивы в `...\downloads`,
кэш в `...\_cache`. На `C:` уйдёт только HIP SDK и драйвер.

Если нужно указать явно:

```powershell
.\scripts\install\00-all.ps1 -Profile target -InstallDir "D:\AI\ComfyUI-Zluda"
```
