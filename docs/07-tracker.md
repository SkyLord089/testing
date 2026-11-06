# Трекер препятствий

Обновляем по ходу. Статусы: `TODO` / `WIP` / `BLOCKED` / `DONE` / `WONTFIX`.

| # | Препятствие | Слой | Статус | Проверка | Заметки |
|---|---|---|---|---|---|
| 1 | HIP 6.4 не видит gfx803 (`hipErrorNoDevice`) | драйвер/HIP | TODO | `hipInfo.exe` 6.4 | см. `01-diagnosis.md#1` |
| 2 | В `rocblas\library` (6.4) нет ядер gfx803 | rocBLAS/Tensile | TODO | `Get-ChildItem -match gfx803` | главный блокер, см. `05` |
| 3 | Требуется community-набор rocBLAS gfx803 под 6.4 | rocBLAS/Tensile | TODO | наличие архива | варианты A/B/C |
| 4 | `cublasLt.dll` (ZLUDA 3.9.6) линкуется на hipblaslt → `WinError 126/127` | DLL | TODO | Process Monitor | вариант «не копировать» / заглушка |
| 5 | `CUBLAS_STATUS_NOT_SUPPORTED` в `cublasLtMatmulAlgoGetHeuristic` | torch 2.6+ | TODO | `verify-torch27.py gemm` | `DISABLE_ADDMM_CUDA_LT=1` + варианты из `06#1` |
| 6 | `CUBLAS_STATUS_NOT_SUPPORTED` в `cublasSgemm` | rocBLAS | TODO | `verify-torch27.py gemm` | зависит от #2/#3 |
| 7 | Мусор/серые картинки при «успешных» GEMM | Tensile-ядра | TODO | сверка с CPU | опасно: неверные ядра |
| 8 | Зависание на VAE decode (8 ГБ VRAM) | память | TODO | `--novram` / tiled VAE | `04#e` |
| 9 | Бесконечная компиляция ядер | ZLUDA кэш | TODO | лог `ZLUDA_COMGR_LOG_LEVEL=1` | `01-diagnosis.md#10` |
| 10 | ComfyUI 0.4x падает на старте | ComfyUI | TODO | лаунчер с `--disable-*` | `06#3` |
| 11 | Triton/flash-attention недоступны на gfx803 | triton | WONTFIX | — | не наш путь, используем quad/math SDPA |
| 12 | cuDNN/MIOpen на gfx803 | MIOpen | TODO | `miopenStatusNotImplemented` | скорее всего оставить cudnn выключенным |

---

## Журнал экспериментов

Каждая запись: дата, что меняли, до/после, вывод. Так через неделю не придётся вспоминать.

### Шаблон

```
### YYYY-MM-DD  —  <короткое название>
Изменили:
  - ...
Проверка:
  - команда: ...
  - результат: ...
Вывод: ...
Откат: ...
```

### Пример (заполнить своим)

```
### 2026-11-06 — baseline
Изменили: ничего
Проверка:
  - preflight.ps1 → preflight-report.txt
  - результат: HIP 6.4 видит gfx803 = <да/нет>; rocblas library gfx803 = <есть/нет>; torch = <2.7.x>
Вывод: стартуем с этапа <N>
```
