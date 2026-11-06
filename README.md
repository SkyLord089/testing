# RX 580 + Windows 11: ComfyUI-ZLUDA на HIP SDK 6.4+ и PyTorch 2.7+cu118

Рабочая площадка для упорной отладки: **ZLUDA 3.9.x + ROCm/HIP SDK 6.4.2 (и выше) + torch 2.7.x+cu118 + ComfyUI** на старой AMD (gfx803 / Polaris / RX 470/480/570/580/590).

**Известный рабочий базис (то, что не ломалось):**

| Слой | Работало | Цель |
|---|---|---|
| HIP SDK | 5.7.1, 6.2.4 | 6.4.2 (и, если получится, 7.1.1) |
| PyTorch | 2.2.1+cu118 (реже 2.3.0) | 2.7.0/2.7.1+cu118 |
| ZLUDA | 3.9.5 (`rocm5` / `rocm6`) | 3.9.6 (`ZLUDA-windows-amd64`, умеет HIP 6.4/7.1) |

Ниже — карта поломок, диагностика, пошаговый план и готовые скрипты. Ничего не «на удачу»: каждый шаг заканчивается проверкой, которая либо проходит, либо даёт конкретный следующий шаг.

---

## С чего начать

### Машина пустая? → установка с нуля

Всё ставится одной последовательностью, с проверкой после каждого шага:

```powershell
# 0) один раз: убедиться, что скрипты в правильной кодировке
powershell -ExecutionPolicy Bypass -File .\scripts\check-encoding.ps1

# 0a) поставить предпосылки (Python 3.11.9 + 7-Zip), тихо, без админа
powershell -ExecutionPolicy Bypass -File .\scripts\install\00a-prereq-install.ps1

# !!! закрыть это окно, открыть НОВОЕ (обновится PATH) !!!

# дальше — по шагам или оркестратором
powershell -ExecutionPolicy Bypass -File .\scripts\install\00-all.ps1 -Profile target
```

Место на дисках: **HIP SDK всегда встаёт на `C:`** (~4 ГБ, выбора нет), всё
остальное — ComfyUI, torch, ZLUDA, архивы, кэши — скрипты кладут рядом с китом,
то есть на его диск. Требования: `C:` ~9 ГБ, диск ComfyUI ~35 ГБ.
Подробно — [`docs/08-install-fresh.md` → раздел 8](docs/08-install-fresh.md#8-раскладка-по-дискам-что-можно-унести-с-c).

> Если вместо таблицы проверок вы увидели `Unexpected token` и строки вида
> `РћР¶РёРґР°РµС‚СЃСЏ` — это кодировка, а не ошибка в скрипте. Причина и лечение:
> [`docs/08-install-fresh.md` → раздел 7](docs/08-install-fresh.md#7-кодировки-почему-скрипты-иногда-падают-с-unexpected-token).

Или по шагам (так виднее, что происходит):

| Этап | Команда | Что делает | Ручного |
|---|---|---|---|
| 0 | `00a-prereq-install.ps1` | ставит Python 3.11.9 и 7-Zip тихо, без админа | — |
| 1 | `01-prereq-check.ps1` | проверяет ОС, карту, RAM/VRAM, диск, git/python/7-Zip, остатки старых ROCm | — |
| 2 | `02-driver.ps1 -OpenPage` | открывает страницу драйвера RX 580, потом проверяет `amdhip64_6.dll` | ставим драйвер, перезагрузка |
| 3 | `03-install-hip.ps1 -HipVersion 6.4.2` | качает установщик AMD, запускает GUI | **снять галочку PRO-драйвера**, перезагрузка |
| 4 | `04-comfyui-zluda.ps1 -Profile target` | clone, venv, torch 2.7.0+cu118, ZLUDA 3.9.6, подмена DLL | — |
| 5 | `05-gfx803-rocblas.ps1 -Archive <путь.7z>` | бэкап + установка ядер gfx803 для Polaris | скачать архив |
| 6 | `06-verify.ps1` | сквозная проверка всей цепочки (HIP → rocBLAS → DLL → torch → GEMM) | — |

Подробное описание с критериями «прошло/не прошло» — [`docs/08-install-fresh.md`](docs/08-install-fresh.md).

**Если на этапе 5 ядра под 6.4 не завелись** — не тратим дни: есть готовый план Б.

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install\90-fallback-hip62.ps1
```

Он откатывает HIP на 6.2.4 (там ядра gfx803 родные), ставит ZLUDA 3.9.5-rocm6 и **оставляет torch 2.7.0+cu118** — цель по torch сохраняется.

### Система уже стоит? → диагностика

1. **Снять состояние системы.** PowerShell **от администратора**:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\preflight.ps1
   ```

   Скрипт создаст `preflight-report.txt` рядом с собой: версии HIP SDK, наличие `amdhip64_6.dll`, есть ли в `rocblas\library` ядра gfx803, что лежит в `venv\torch\lib`, переменные окружения, вывод `hipInfo.exe`.
   **Пришлите этот файл** — по нему сразу видно, на каком слое мы стоим.

2. **Прогнать тест операций torch 2.7** (в активированном venv, из папки ComfyUI-Zluda):

   ```powershell
   .\venv\Scripts\python.exe .\scripts\verify-torch27.py
   ```

   Он бьёт по всем местам, где torch 2.6/2.7 разошёлся с 2.2.1 (cublasLt/addmm-с-bias, bmm, fp16/bf16, conv, SDPA) и печатает `PASS/FAIL` по каждой операции.

3. По результатам — открыть `docs/07-tracker.md`, взять первое `FAIL` и идти по его разделу в `docs/03-fix-plan.md`.

---

## Документы

| Файл | Зачем |
|---|---|
| [`docs/00-stack-map.md`](docs/00-stack-map.md) | Карта стека: кто кого вызывает, где именно рвётся на 6.4/2.7 |
| [`docs/01-diagnosis.md`](docs/01-diagnosis.md) | Диагностика: симптом → причина → проверка (таблица типовых ошибок) |
| [`docs/02-matrix.md`](docs/02-matrix.md) | Матрица совместимости версий + ссылки на первоисточники |
| [`docs/03-fix-plan.md`](docs/03-fix-plan.md) | Пошаговый план: этапы 0–4, критерии выхода с каждого |
| [`docs/04-rebuild-zluda.md`](docs/04-rebuild-zluda.md) | Ребилд ZLUDA под HIP 6.4/7.1 + патчи-кандидаты (cublasLt, lazy hipBLASLt, wave64) |
| [`docs/05-gfx803-rocblas.md`](docs/05-gfx803-rocblas.md) | Главный блокер: rocBLAS/Tensile без gfx803 в 6.4. Варианты A/B/C + сборка Tensile |
| [`docs/06-torch27.md`](docs/06-torch27.md) | Что поменялось в torch 2.4→2.7 и как это лечится на ZLUDA |
| [`docs/07-tracker.md`](docs/07-tracker.md) | Трекер препятствий: статусы, кто/что/почему |
| [`docs/08-install-fresh.md`](docs/08-install-fresh.md) | **Установка с нуля**: профили, ссылки, порядок, проверки, ручной путь |
| `scripts/preflight.ps1` | Сбор диагностики в файл |
| `scripts/patch-torch-zluda.ps1` | Аккуратная подмена DLL в `venv\torch\lib` c бэкапом и проверками |
| `scripts/verify-torch27.py` | Тест операций torch 2.7 на ZLUDA (PASS/FAIL по каждой) |
| `scripts/comfyui-user.bat` | Лаунчер ComfyUI с настройками под gfx803 + torch 2.7 |
| `scripts/install/00-all.ps1` | Оркестратор: этапы 1–6 с остановками на ручных шагах |
| `scripts/install/00a-prereq-install.ps1` | Ставит Python 3.11.9 x64 и 7-Zip; идемпотентно |
| `scripts/install/01-prereq-check.ps1` | Проверка чистой машины перед установкой |
| `scripts/install/02-driver.ps1` | Драйвер RX 580: что стоит, что поставить, проверка после |
| `scripts/install/03-install-hip.ps1` | Скачивание и установка HIP SDK 6.4.2 / 6.2.4 |
| `scripts/install/04-comfyui-zluda.ps1` | ComfyUI-Zluda + venv + torch 2.7.0+cu118 + ZLUDA + патч DLL |
| `scripts/install/05-gfx803-rocblas.ps1` | Ядра rocBLAS gfx803: `-ListOnly`, бэкап, `-Clean`, проверка |
| `scripts/install/06-verify.ps1` | Проверка установки: hipInfo, ядра gfx803, DLL, torch.cuda, GEMM/addmm |
| `scripts/check-encoding.ps1` | Сторож кодировок: ловит `.ps1` без BOM и `.bat` с BOM, умеет `-Fix` |
| `scripts/install/90-fallback-hip62.ps1` | План Б: откат на HIP 6.2.4 с сохранением torch 2.7 |

---

## Три главных вывода (кратко)

1. **ZLUDA 3.9.6 — не проблема, а наоборот.** В changelog прямо: *«Introduce HIP SDK 6.4 and 7.1 supports»*. Апстрим `xtask` уже ищет `HIP_PATH_71 → 64 → 62 → 61`. Значит собирать под 6.4 ничего не надо — надо правильно приготовить **rocBLAS с ядрами gfx803** и **режим GEMM в torch**.
2. **Главный блокер — rocBLAS/Tensile для gfx803 на HIP 6.4.** AMD выпилила Polaris из Tensile; в сборках 6.4 для gfx803 ядер нет (в отличие от 5.7.1/6.2.4, где есть community-сборки). Пока это не решено, `hipInfo` может видеть карту, а первый же `addmm`/`conv` упадёт или вернёт мусор.
3. **torch 2.7 сам по себе ломает путь GEMM на ZLUDA.** В 2.7 `DISABLE_ADDMM_CUDA_LT=1` отключает только *addmm*-ветку cublasLt, но cublasLt-путь остаётся ещё в нескольких местах, а ZLUDA 3.9.6 **впервые начала класть `cublasLt.dll` в обычную (не nightly) сборку** — и он линкуется к `hipblaslt.dll` из HIP SDK, у которого для gfx803 ядер нет вообще. Отсюда `CUBLAS_STATUS_NOT_SUPPORTED` и «Bad Image» на ровном месте. Подробности и лечение — `docs/04` и `docs/06`.

---

## Принципы работы (чтобы не потеть зря)

- **Один слой за раз.** Сначала HIP видит карту (`hipInfo.exe`) → потом ZLUDA видит устройство (`torch.cuda.is_available()`) → потом считает GEMM (`verify-torch27.py`) → потом ComfyUI. Чинить «ComfyUI не рисует» на сломанном `hipInfo` бессмысленно.
- **Ничего не перетирать без бэкапа.** Все скрипты кладут `.bak-<дата>` рядом.
- **Кэш ZLUDA — отдельно.** `ZLUDA_CACHE_DIR` (новинка 3.9.6) на быстрый диск; чистить между экспериментами, иначе ловишь «старые» баги после смены библиотек.
- **Гипотеза → проверка → запись в `docs/07-tracker.md`.** Иначе через 3 дня непонятно, что уже пробовали.
