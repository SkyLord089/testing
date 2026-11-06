# Главный блокер: rocBLAS / Tensile для gfx803 под HIP 6.4

## Суть

rocBLAS — это то, что ZLUDA вызывает вместо cuBLAS. Внутри rocBLAS лежат **Tensile-ядра** (GEMM-кернелы), скомпилированные **под конкретную архитектуру**. На Windows они лежат в:

```
C:\Program Files\AMD\ROCm\6.4\bin\rocblas\library\
```

Имена файлов содержат архитектуру: `TensileLibrary_lazy_gfx803.dat`, `Kernels.so-000-gfx803.hsaco` и т.п.

**В официальном HIP SDK 6.4 файлов с `gfx803` в имени нет.** AMD прекратила поддержку Polaris в Tensile; gfx803 отсутствует в списке таргетов сборки. Поэтому:

- rocBLAS **инициализируется** (библиотека грузится), но на первом GEMM пишет
  `rocBLAS error: Cannot read ...\TensileLibrary.dat ... for GPU arch : gfx803`
  и/или `Could not initialize Tensile host`;
- если подсунуть **чужие** ядра (например, от gfx900/gfx1010) — карта их «выполнит», но результат будет мусором (нет гарантий, что ISA-фичи совпадают), что в SD выглядит как серая/шумная картинка;
- в 5.7.1 и 6.2.4 ядра gfx803 существуют (в том числе community-пересборки), поэтому «там работало».

> Это же относится и к **hipBLASLt** (cublasLt-путь): на Windows hipBLASLt появился только в 6.4.2 и официально поддержан **только для gfx1101**. Для gfx803 ядер там нет вообще — см. `06-torch27.md`.

---

## Вариант A. Готовый community-набор под 6.4 (сначала это)

**Что искать:** архив вида `rocm gfx803 for hip sdk 6.4.2.7z`, внутри — `rocblas.dll` и/или `library/` c `TensileLibrary*_gfx803.dat`.

**Где искать (по состоянию на момент написания):**

- `likelovewant/ROCmLibs-for-gfx1103-AMD780M-APU` → релизы `v0.6.4.2` и новее. **Внимательно смотреть список архитектур в описании релиза**: на 6.4.2 официально перечислены gfx1010/1012/1031/1032/1034/1035/1036/1103/1152/1153; gfx803 там **не было**. Если появится — это самый быстрый путь.
- `advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU` — набор для **5.7.1 и 6.2.4**; 6.4 пока нет. Полезен как источник **логик Tensile** для варианта C.
- `brknsoul/ROCmLibs` — наборы для 5.7.x.

**Установка (общая схема):**

```powershell
# 1. БЭКАП
$roc = 'C:\Program Files\AMD\ROCm\6.4\bin'
Copy-Item "$roc\rocblas" "$roc\rocblas.bak-$(Get-Date -Format yyyyMMdd-HHmm)" -Recurse

# 2. Распаковать архив (7-Zip) и разложить:
#    rocblas.dll        -> C:\Program Files\AMD\ROCm\6.4\bin\
#    library\*          -> C:\Program Files\AMD\ROCm\6.4\bin\rocblas\library\
#    (не в old_library!)

# 3. Проверить, что появились файлы с gfx803
Get-ChildItem "$roc\rocblas\library" | Where-Object Name -match 'gfx803' | Select-Object Name,Length

# 4. Перезагрузка (обязательно: rocBLAS держит DLL, драйвер кэширует)
Restart-Computer
```

**Критерий успеха:** `Get-ChildItem ... -match gfx803` непустой **и** GEMM-тест проходит со сверкой с CPU (см. ниже).

---

## Вариант B. Позаимствовать ядра gfx803 из 6.2.4 (быстрый эксперимент)

Идея: оставить всё от 6.4, но подложить в `rocblas\library` ядра gfx803 из 6.2.4. Иногда работает, потому что формат Tensile-библиотеки между 6.2 и 6.4 не менялся; чаще — нет, потому что меняется версия Tensile и набор символов.

```powershell
# на машине, где стоит 6.2.4 (или из архива likelovewant v0.6.2.4 / advanced-lvl-up)
$src = 'C:\Program Files\AMD\ROCm\6.2\bin\rocblas\library'
$dst = 'C:\Program Files\AMD\ROCm\6.4\bin\rocblas\library'
Get-ChildItem $src -Filter '*gfx803*' | Copy-Item -Destination $dst -Verbose
```

**Проверка:** должен пройти GEMM-тест. Если в логе `Cannot read TensileLibrary` остался — вариант не сработал (Tensile не понимает формат), идём в C.

**Важно:** `rocblas.dll` при этом **оставить от 6.4**. Если положить `rocblas.dll` от 6.2 рядом с HIP 6.4 — почти гарантирован `WinError 127` (не найдены точки входа amdhip64_6).

---

## Вариант C. Собрать rocBLAS/Tensile для gfx803 из исходников 6.4

Это «правильный» путь, но самый долгий. Два подхода:

### C1. Windows-сборка Tensile вручную

1. Взять исходники `rocBLAS`/`Tensile` релиза 6.4.x.
2. Принести **логики Tensile** для Polaris (`polaris_Cijk_Alik_Bljk_*.yaml` / `gfx803` logic) из community-репозиториев (`polaris803YAML.zip` в `advanced-lvl-up`).
3. Собрать только нужные архитектуры:
   ```cmake
   -DTensile_ARCHITECTURE=gfx803
   -DTensile_LOGIC=<path to polaris logic>
   -DTensile_LIBRARY_FORMAT=msgpack
   ```
4. Собрать `rocblas.dll` с `-DAMDGPU_TARGETS=gfx803` и положить `library/` рядом.

### C2. Взять Linux-сборку из готовых Docker-проектов и вытащить артефакты

Есть публичные Docker-сборки ROCm 6.4 для Polaris, где Tensile/rocBLAS уже собраны:

- `robertrosenbusch/gfx803_rocm` (`Dockerfile_rocm64_base`, `Dockerfile_rocm64_pytorch`)
- `spy1134/rocm-gfx803` (ROCm 6.4.4 + фиксы некорректных результатов на Polaris)
- `lakitu12/rocm-gfx803` (порт ROCR/CLR 7.14, заметки по gfx803)

**Важно:** Linux-`rocblas.dll` (`.so`) в Windows не годится. Годятся:
1. **Tensile-ядра** (`.dat`/`.hsaco`) — формат кроссплатформенный, их можно положить в Windows-`library\`;
2. **Логики Tensile** (YAML) — исходник для Windows-сборки;
3. **Список патчей**, которые авторы применили к Tensile для Polaris (в этих репозиториях обычно есть `docs/` и `KERNEL_BUGS.md`).

```bash
# пример: вытащить только нужное из docker-образа
docker create --name gfx803 robertrosenbusch/gfx803_rocm:6.4
docker cp gfx803:/opt/rocm/lib/rocblas/library ./gfx803-library
docker cp gfx803:/opt/rocm/lib/librocblas.so ./librocblas.so
# далее: .dat/.hsaco → в Windows library\, YAML — в C1
```

---

## Проверка результата (для любого варианта)

1. **Быстрый тест GEMM** (`scripts/verify-torch27.py`, секция `gemm`).
2. **Сверка с CPU** — обязательна, потому что с «чужими» ядрами ошибка может не падать, а тихо врать:

```python
import torch
torch.manual_seed(0)
a = torch.randn(512, 512)
b = torch.randn(512, 512)
ref = (a.double() @ b.double())
out = (a.cuda() @ b.cuda()).cpu()
rel = (out - ref.float()).abs().max() / ref.abs().max()
print("rel_err:", rel.item())   # fp32: <1e-4, fp16: <1e-2
```

3. В логе ComfyUI не должно быть `rocBLAS error` / `Tensile` / `no kernel found`.

---

## Чего НЕ делать

- ❌ Не ставить `HSA_OVERRIDE_GFX_VERSION=8.0.3` «на всякий случай» — на Windows это почти всегда ломает перечисление устройств, а не помогает.
- ❌ Не смешивать `rocblas.dll` от одной версии HIP с `library/` от другой (и тем более бинарники 5.7 с HIP 6.4).
- ❌ Не копировать `library` целиком из 5.7 в 6.4 поверх — старые `.dat` могут «выигрывать» у новых и давать мусор.
- ❌ Не оставлять несколько HIP SDK в PATH: первый найденный `amdhip64_6.dll` определит всё, включая rocBLAS.
