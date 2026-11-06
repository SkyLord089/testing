# Матрица совместимости (по первоисточникам)

Легенда: ✅ подтверждено, ⚠️ «обычно работает, но есть нюансы», ❌ не работает, ❓ данных нет.

## HIP SDK × ZLUDA × torch

| HIP SDK | ZLUDA | torch cu118 | gfx803 (RX 580) | Комментарий |
|---|---|---|---|---|
| 5.7.1 | 3.9.5 `ZLUDA-windows-rocm5-amd64` | **2.2.1** | ✅ | Исторический «золотой» набор. rocBLAS для gfx803 — community-сборка (Tensile 5.7) |
| 5.7.1 | 3.9.5 rocm5 | 2.3.0 | ⚠️ | Работает у многих, но именно здесь начинаются cublasLt-сюрпризы |
| 5.7.1 | 3.9.6 `rocm5` | 2.2.1 | ⚠️ | ZLUDA новее, но у 3.9.6 в rocm5-сборке набор DLL тот же, что у rocm6 |
| 6.2.4 | 3.9.5 rocm6 (+ gfx803 Tensile от community) | 2.2.1 | ✅ | Второй рабочий набор; популярен в ComfyUI-Zluda |
| 6.2.4 | 3.9.5 nightly rocm6 | 2.7.0 | ⚠️ | Именно этот путь прописан в `install-n.bat`; на gfx803 даёт `CUBLAS_STATUS_*` |
| **6.4.2** | **3.9.6 `ZLUDA-windows-amd64`** | **2.7.x** | ❓ **цель** | ZLUDA умеет 6.4; **rocBLAS/Tensile для gfx803 в 6.4 отсутствует** — главный блокер |
| 7.1.1 | 3.9.6 | 2.7.1 | ❌/❓ | + на 7.1 есть отдельный баг перечисления даже на поддерживаемых картах |
| 6.4.4 (официальный AMD PyTorch) | — | — | ❌ | AMD-шные wheels только для RDNA2+ (gfx101x/gfx103x/gfx11xx/gfx12xx) |

## Что именно «обещают» источники

- **ZLUDA 3.9.6 changelog** («Introduce HIP SDK 6.4 and 7.1 supports», 2026‑02‑11) — https://github.com/lshqqytiger/ZLUDA/releases
- **ZLUDA README**: для PyTorch на Windows — заменить DLL в официальном cu118-колесе; `DISABLE_ADDMM_CUDA_LT=1` для torch > 2.4 (не nightly).
- **ZLUDA issue #137** (запрос матрицы совместимости), ответ мейнтейнера:
  - `HIP SDK 7.1.1 + ZLUDA 3.9.6 + torch 2.7.1+cu118` — совместимы;
  - `HIP SDK 6.4.2 + ZLUDA 3.9.5 + torch 2.7.1+cu118` — совместимы.
  Важно: это про **поддерживаемые** карты (gfx1030+). Для gfx803 обещаний нет.
- **HIP SDK для Windows, component support**: «HIPBlasLT is now available for Windows users in HIP SDK version 6.4.2 and later, supported on gfx1101» — то есть **hipBLASLt на 6.4.2 существует, но не для Polaris**.
- **AMD про gfx803**: официальная поддержка прекращена (ROCm ≥ 3.10/4.x для gfx803 в разных компонентах), в 6.4 gfx803 отсутствует в Tensile-ядрах (подтверждается тредом ROCm/TheRock #9127 про отсутствие Tensile для gfx906 — та же механика).
- **torch 2.2.1 vs 2.4+ (Windows, cu118)**: в `aten/src/ATen/native/cuda/Blas.cpp` в 2.2.1 стоит гвард `#if CUDA_VERSION >= 11040 && !defined(_MSC_VER)` вокруг cublasLt-ветки `addmm`; в 2.4+ гвард `_MSC_VER` убран → cublasLt-путь включён и на Windows. Переменная `DISABLE_ADDMM_CUDA_LT` остаётся единственным выключателем.

## Ссылки на gfx803-сборки rocBLAS/Tensile (по убыванию удобства)

| Источник | Что даёт | Под какой HIP | Замечания |
|---|---|---|---|
| `advanced-lvl-up/Rx470-Vega10-Rx580-gfx803-gfx900-fix-AMD-GPU` | `rocblas.dll` + `library/` для gfx803/gfx900, фиксы утечек/падений | **5.7.1 и 6.2.4** | Проверенный community-набор; 6.4 **нет** |
| `likelovewant/ROCmLibs-for-gfx1103-AMD780M-APU` (release `v0.6.2.4`) | `rocBLAS-HIP6.2.4.7z` (gfx803 внутри) | 6.2.4 (+6.1.2) | Много карт в одном архиве |
| `likelovewant` release `v0.6.4.2` | наборы под 6.4.2, **но** список: gfx1010/1012/1031/1032/1034/1035/1036/1103/1152/1153 + official | 6.4.2 | **gfx803 в списке нет** — поэтому нужен вариант B/C |
| `brknsoul/ROCmLibs` | `ROCmLibs.7z`, `Newer ROCmLibs.rar` (в т.ч. gfx803) | 5.7.1 | Для 6.4 не подходит напрямую |
| `spy1134/rocm-gfx803`, `robertrosenbusch/gfx803_rocm`, `lakitu12/rocm-gfx803` | сборка ROCm/Tensile/torch для gfx803 с нуля (Linux-центрично, есть Docker) | 6.4 | Источник **логик Tensile** и патчей для варианта C |

> Проверять актуальность этих репозиториев нужно каждый раз: community-наборы под 6.4 для Polaris появляются и ломаются регулярно. Перед скачиванием смотреть дату ассетов и список архитектур внутри архива.
