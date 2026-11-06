# Карта стека: где именно рвётся на HIP 6.4 + torch 2.7

## Слои снизу вверх

```
┌──────────────────────────────────────────────────────────────────────────┐
│ ComfyUI (comfy/model_management.py → import comfy.zluda)                 │
│  • ZLUDA-патчи: cudnn off, flash/mem_efficient SDPA off, math on, topk   │
│  • cuda_malloc.py: для +cu118 backend:cudaMallocAsync НЕ включён (cu<130)│
└──────────────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────────────┐
│ torch 2.7.x+cu118 (питоновский + нативные .dll в torch/lib)               │
│  aten::addmm/mm/bmm → ATen/native/cuda/Blas.cpp → at::cuda::blas::gemm   │
│  aten::conv2d        → cuDNN (libcudnn_*) либо fallback                  │
│  SDPA                → flash / mem_efficient / math / cudnn              │
└──────────────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────────────┐
│ ZLUDA 3.9.6 (cublas.dll, cublasLt.dll, cudnn.dll, cufft, cusparse,       │
│             nvrtc, nvcuda.dll)                                            │
│  • cublas.dll    → rocBLAS          (epilogue: gemm_and_bias / GELU)     │
│  • cublasLt.dll  → hipBLASLt        ← НОВОЕ в 3.9.6 (и в non-nightly!)   │
│  • cudnn.dll     → MIOpen           (MIOpen для gfx803 — большой вопрос)  │
│  • PTX → AMDGCN через comgr/LLVM, режим волны выб. по warpSize карты     │
└──────────────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────────────┐
│ HIP SDK 6.4.2: amdhip64_6.dll, amd_comgr.dll, rocblas.dll(+library/),    │
│                hipblaslt.dll(+library/), miopen, rocfft, ...             │
│  • rocblas/library/TensileLibrary* — НЕТ gfx803 в официальной сборке 6.4 │
│  • hipblaslt — впервые на Windows в 6.4.2, официально только gfx1101     │
└──────────────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────────────┐
│ AMD Adrenalin driver (в System32: amdhip64_6.dll, amd_comgr.dll)         │
│  • Polaris на legacy-ветке; версия драйвера определяет, что видит HIP    │
└──────────────────────────────────────────────────────────────────────────┘
```

## Почему «на 6.2 работало, а на 6.4 нет» — по слоям

### 1. Драйвер/HIP enumeration
`amdhip64` на Windows перечисляет адаптеры через DXCore и **квалифицирует** их (ISA + версия драйвера). Если драйвер не тот, что ждёт данный HIP — `hipGetDeviceCount = 0` → `hipErrorNoDevice (100)` → у ZLUDA пустой список устройств → `RuntimeError: No CUDA GPUs are available` ещё до всякого torch.
Известны случаи, когда 6.4 карту видит, а 7.1.1 — нет (и наоборот), причём без изменения железа. Для Polaris это ещё чувствительнее: официально gfx803 в 6.4 не поддерживается.
**Проверка:** `C:\Program Files\AMD\ROCm\6.4\bin\hipInfo.exe` (см. `scripts/preflight.ps1`).

### 2. rocBLAS / Tensile
В 6.4 библиотека грузит `TensileLibrary_lazy_gfx803.dat` (или `.co`/`.hsaco`) из `...\bin\rocblas\library`. Официальный инсталлятор их для gfx803 не кладёт → первый же GEMM:
`rocBLAS error: Cannot read .../TensileLibrary.dat ... for GPU arch : gfx803` либо тихий мусор в результате (Tensile-ядра собраны неверно).
**Это главный блокер перехода на 6.4.**

### 3. ZLUDA
Сам по себе 3.9.6 умеет HIP 6.4/7.1 (см. `xtask`: `HIP_PATH_71 → 64 → 62 → 61`) и содержит правки под ROCm6. Но:
- он определяет `compilation_mode` по `warpSize` устройства (gfx803 → 64 → `DoubleWave32OnWave64`, медленный режим; `ZLUDA_WAVE64_SLOW_MODE=1` → `Wave32OnWave64`);
- ISA берётся из `gcnArchName` (для 6.4 API — `hipGetDevicePropertiesR0600`);
- `cublasLt.dll` теперь в обычной сборке, а для `--nightly` ещё и `cudnn.dll`/`nccl.dll`. Это меняет то, что нужно копировать в `venv\torch\lib`.

### 4. torch 2.7
CMake-флаг `CUDA_VERSION >= 11040 && !defined(_MSC_VER)` в 2.2.1 (Windows-cu118 → cublasLt-ветка addmm выключена на этапе компиляции) в 2.4+ **убран**. Теперь cublasLt-путь включён и на Windows, и `DISABLE_ADDMM_CUDA_LT=1` — единственный способ его выключить. Плюс:
- `USE_ROCM` в наших сборках не определён (это cu118-бинарник), значит `isSupportedHipLtROCmArch()` ZLUDA не спасает;
- cublasLt-путь есть не только в `addmm`, а ещё в `mm`-ветках с `gemm_and_bias`, в `bmm` (для некоторых форм), в `_scaled_mm` — поэтому «одна переменная окружения» не всегда закрывает проблему.

### 5. ComfyUI
`comfy/zluda.py` делает базовые патчи, но рассчитан на набор DLL «как раньше» (2.2.1). При 2.7:
- `cuda_malloc.py` не включает `cudaMallocAsync` (это хорошо — ZLUDA его не любит), но **не выключает** и явно, если кто-то выставил `PYTORCH_CUDA_ALLOC_CONF` руками;
- асинхронный offload весов (новый, по умолчанию) и pinned memory на Polaris/ZLUDA дают зависания и «Bad Image» — нужны `--disable-async-offload --disable-pinned-memory`.

## Кто что «обещает» по версиям (проверено по исходникам/релизам)

- **ZLUDA 3.9.6** (2026-02-11), changelog: *«Introduce HIP SDK 6.4 and 7.1 supports»*; в сборке **нет** `cudnn.dll` и `nccl.dll` (они только в `--nightly`).
- **ZLUDA 3.9.5**: nightly (для ROCm6) содержит `cudnn.dll`, `nccl.dll`, `nvml.dll`, `nvcuda.dll`, `cublas*.dll`, `cufft*.dll`, `cusparse.dll`, `nvrtc.dll`.
- **torch 2.2.1 cu118 на Windows**: cublasLt-ветка addmm отключена на этапе сборки (`!defined(_MSC_VER)`); в 2.4+ гвард убран.
- **HIP SDK 6.4.2**: HIPBlasLT на Windows впервые, официально только gfx1101; для gfx803 ни `rocblas/library`, ни `hipblaslt/library` ядер не содержит.
