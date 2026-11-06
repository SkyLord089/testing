# Troubleshooting / типичные ошибки

> Везде ниже `%USERPROFILE%\RX580-ComfyUI` — это InstallRoot по умолчанию.
> Если ставили на D: (`-InstallRoot D:\RX580-ComfyUI`), подставляйте `D:\RX580-ComfyUI`.


## Установка и запуск

**`python` открывает Microsoft Store**
Вы установили Python из Store. Удалите его, поставьте с python.org, отметив "Add Python to PATH", перезапустите консоль.

**`git` не найден / `pip` не найден**
Не отмечены пути при установке Git/Python. Переустановите с галочками "Git from the command line"/"Add Python to PATH"
либо откройте новую консоль после установки.

**`torch.cuda.is_available()` возвращает `False`**
По пунктам:
1. Запускаете проверку **через zluda.exe** (`Verify-Setup.ps1` так и делает)? Просто `python -c "import torch; print(torch.cuda.is_available())"`
   без ZLUDA на AMD всегда покажет `False`.
2. Патч rocBLAS применён? Ищите файл `C:\Program Files\AMD\ROCm\6.4\bin\rocblas\gfx803.zluda-patched`. Нет — запустите `Patch-RocBLAS-gfx803.ps1` (админ).
3. В `PATH` попал **другой** ROCm (5.7 при установленных 6.4 и 6.2)? Оставьте в PATH только ваш каталог `ROCm\<версия>\bin`
   (System Variables / User Variables — проверьте оба). Наличие двух версий — классическая причина тихих крашей.
4. Антивирус не забанил `zluda.exe` / `nvcuda.dll`? (ZLUDA — DLL-инжектор, эвристика его не любит.) Добавьте папку `RX580-ComfyUI` в исключения.
5. Драйвер свежий? AMD Adrenalin → Проверка обновлений. ZLUDA/HIP требуют AMD Adrenalin ≥ 23.x по факту.

**`OSError: [WinError 127] The specified procedure could not be found. Error loading "...\torch\lib\cublas64_11.dll"`**
Фактическая картина у нас: `nvcuda.dll`/`nvrtc.dll` грузятся, **`cublas.dll`/`cusparse.dll` и пропатченные
`cublas64_11.dll`/`cusparse64_11.dll` — нет**, а оригиналы (`*.orig`) — грузятся. Локализует проблему в цепочке
зависимостей **BLAS/SPARSE** (hipblas / rocblas / rocsparse из HIP SDK, вместе с rocBLAS-патчем), а не в ZLUDA как таковой.
По порядку:
1. Убедитесь, что `C:\Program Files\AMD\ROCm\6.4\bin` есть в PATH (машинном или пользовательском) и консоль перезапущена:
   `[Environment]::GetEnvironmentVariable('Path','Machine') -split ';' | ? { $_ -match 'ROCm' }`
   и то же с `'User'`. `Verify-Setup.ps1 -MemoryReport` сам печатает, найден ли каталог
   и лежит ли он в PATH, пробует загрузить `amdhip64/hipblas/rocblas/hipsparse/rocsparse` (по полному пути и по короткому имени),
   показывает версии файлов и делает **офлайн-разбор PE-таблиц импорта/экспорта**: печатает `DEP MISSING <dll>`
   (модуль не найден → WinError 126) либо `MISSING EXPORTS in <dll>: <список>` (точка входа не найдена → WinError 127)
   с полным путём виновной DLL.
2. Если `hipblas.dll`/`rocblas.dll`/`hipsparse.dll`/`rocsparse.dll` есть в каталоге, но **не загружаются** либо PE-разбор показывает
   `MISSING EXPORTS` — это неполная/несовместимая установка HIP SDK (не дожили BLAS/SPARSE-компоненты): переустановите HIP SDK целиком (все чекбоксы, включая rocBLAS/rocSPARSE), затем `Patch-RocBLAS-gfx803.ps1 -RocmVersion 6.4` (админ).
3. Если `rocblas.dll` загрузился, но ядра не под gfx803 — проверьте маркер `...\6.4\bin\rocblas\gfx803.zluda-patched` и перезагрузитесь.
4. Если `load OK` везде, но torch всё равно падает с 127 — смешение версий: переустановите **HIP SDK 6.2.4** (обкатанный путь Advanced-lvl-up;
   на 6.2/5.7 установщик сам оставит рецепт с rocm5 nvcuda). Крайний вариант — HIP 5.7.1 + `-TorchVersion 2.2.1`.
   На HIP 6.4 рецепта, подтверждённого сообществом, пока нет — это экспериментальный трек.
   WinError 126 = модуль/зависимость не найдена (PATH / нет hipblas-rocblas-rocsparse), WinError 127 = точка входа не найдена (несовместимая пара DLL).

**`zluda` папка пустая / `[ERROR] ZLUDA DLL missing: ...\zluda\cublas.dll` / «zluda.exe not found»**
Причина (v1 скрипта): ZLUDA-архивы не обязаны иметь классическую плоскую раскладку, а установщик
копировал только папку «где лежит zluda.exe», молча оставляя `zluda\` пустым. Исправлено: payload
собирается из обоих архивов по всему дереву (rocm6 в приоритете, rocm5 как fallback) с обязательной
проверкой `zluda.exe / nvcuda.dll / cublas.dll / cusparse.dll / nvrtc.dll`.
Если такое повторится — скрипт сам напечатает полный рекурсивный листинг обоих архивов;
пришлите этот вывод. Распакованные деревья остаются в `<InstallRoot>\tools\zluda-rocm6` и `zluda-rocm5`
(можно смотреть `tar -tf` по зипам там же). Переустановка быстрая: torch 2.9 ГБ берётся из pip-кэша.

**`hipErrorNoBinaryForGpu` / `no binary image is available for the device`**
rocBLAS не может найти ядра под gfx803. Скрипт `Patch-RocBLAS-gfx803.ps1` либо не запускался (нет прав/ошибка скачивания),
либо библиотека заменена не в том ROCm. Запустите с явным `-RocmVersion 6.4`, проверьте маркер-файл, перезагрузите ПК.

**`ZLUDA: nvcuda.dll not found` / «не удаётся загрузить nvcuda.dll»**
Вы запускаете `python main.py` напрямую, а не через `zluda.exe`, либо в `zluda\` нет `nvcuda.dll`.
Запускайте только через `run-comfyui.bat` (он и вызывает `zluda.exe -- python …`).
Если `nvcuda.dll` отсутствует — повторите `Install-ComfyUI-RX580.ps1` (или извлеките `nvcuda.dll` из `ZLUDA-windows-rocm5-amd64.zip` в папку `zluda\`).

**Первый запуск «завис» на 15 минут без вывода**
Это штатная компиляция ядер ZLUDA/rocBLAS (10–30 мин, GPU почти не загружен, Power-лампочка мигает).
Не закрывайте окно. Для диагностики выставите `set ZLUDA_COMGR_LOG_LEVEL=1` перед запуском.
Последующие запуски и повторные генерации с теми же настройками — быстрый кэш.

## Генерация

**Чёрный / мусорный (серо-шум) результат, особенно на VAE**
Классика gfx803: декод VAE уезжает в своп. Варианты по убыванию простоты:
1. Флаг `--cpu-vae` (декод на CPU — медленно, но надёжно).
2. `--fp32-vae` вместо fp16 VAE.
3. TinyVAE / taesd (node `Tiny Auto Encoder`) в workflow.
4. Проверьте pagefile ≥ 16 ГБ и закрытые «тяжелые» приложения (Chrome на 6 вкладок с видео держит гигабайты ОЗУ).
5. GGUF-кванты UNet вместо полных safetensors.

**torch 2.3+ падает с fake OOM («Free (according to CUDA): 0 bytes»)**
Самая частая причина на новых torch под ZLUDA — ComfyUI сам включает `backend:cudaMallocAsync` для torch≥2.0+cu*, а ZLUDA его не тянет.
Фикс уже встроен в `run-comfyui.bat`: `--disable-cuda-malloc --disable-smart-memory` + `PYTORCH_CUDA_ALLOC_CONF=expandable_segments:False`.
Если запускаете ComfyUI вручную — добавьте эти флаги. Полный разбор, диагностика (`Verify-Setup.ps1 -MemoryReport`)
и лестница обходов — в [fake-oom.md](fake-oom.md).

**CUDA out of memory / `HIP out of memory`** (честный, после фикса выше)
`--lowvram`, уменьшите batch/разрешение, fp16 обязателен, GGUF-кванты, `--disable-smart-memory`
(если версия ComfyUI его поддерживает), перезапуск после каждой OOM (память на gfx803 возвращается не всегда чисто).

**10–20 минут на картинку даже после «прогрева»**
- Убедитесь, что нет `--cpu` и нет `--directml` в аргументах.
- Убедитесь, что работает именно zluda-путь: в логе при старте torch должен печатать версию с суффиксом `+cu118`.
- Температуры/троттлинг: под compute-нагрузкой Polaris скисает — WattMan → даунвольт/лимит power; мониторьте t°.
- Не используйте fp32-модели: только `--force-fp16`.

**«ImportError: DLL load failed» при импорте torch**
Патч `torch\lib` сделан не до конца (например, сбой копирования `cublas64_11.dll`).
Снова выполните `Install-ComfyUI-RX580.ps1 -SkipComfyUI`-подобный прогон (часть скрипта с патчем) или по манифесту
`…\RX580-ComfyUI\tools\patch-manifest.txt` восстановите из `*.orig` и пропатчите вручную.
Частая причина: антивирус подчистил «подозрительные» DLL — добавьте папку в исключения и повторите.

**В ComfyUI Manager / расширениях половина узлов красная**
Это нормально для старых GPU: узлы, завязанные на конкретную CUDA-версию (kaolin, различная nvdiffrast-экзотика), под ZLUDA не заведутся.
Используйте базовый зоопарк: обычные KSampler, VAELoader, CheckpointLoaderSimple, GGUFLoader, TinyVAE.

**Тихий краш окна без ошибок после «Compilation is in progress»**
Почти всегда: (а) маленький pagefile, (б) две версии ROCm в PATH, (в) не тот `nvcuda.dll` в папке zluda
(должен быть из rocm5-сборки для rocm6-ZLUDA), (г) драйвер старее, чем требует HIP SDK 6.4 — обновите Adrenalin.

## Откат

- rocBLAS: `.\Restore-RocBLAS.ps1 -RocmVersion 6.4` (админ) — вернёт оригинальные `rocblas.dll` и `rocblas\library` из `.zluda-bak`.
- torch\lib: файлы `*.orig` в `venv\Lib\site-packages\torch\lib` → переименовать обратно в убранные расширения (список — в `patch-manifest.txt`).
- ComfyUI целиком: удалить `%USERPROFILE%\RX580-ComfyUI`.
- HIP SDK: штатный деинсталлятор Windows («Установка и удаление программ» → AMD HIP SDK). При этом ярлыков/ключей реестра от ComfyUI-пути не остаётся.

## Если всё совсем плохо

Обкатанные альтернативы для этой же карты (см. также `architecture.md`):

1. **HIP SDK 6.2.4** вместо 6.4 (`-RocmVersion 6.2`) — патч официально собирался под неё.
2. **HIP SDK 5.7.1 + torch 2.2.1+cu118** — ровно то, что пинует [ComfyUI-Zluda](https://github.com/patientx/ComfyUI-Zluda) (`install-for-older-amd.bat`). Максимально скучный, проверенный путь.
3. **DirectML**: `pip install torch-directml` + запуск ComfyUI с `--directml`. Без HIP/ZLUDA вовсе, медленнее, но железно.
4. **Linux вместо Windows**: ROCm 6.4.4 для Polaris пересобран сообществом ([spy1134/rocm-gfx803](https://github.com/spy1134/rocm-gfx803)) — самый здравый вариант, если ОС не принципиальна.
