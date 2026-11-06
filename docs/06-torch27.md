# torch 2.7 на ZLUDA: что поменялось и как лечить

## 1. Главное: cublasLt-путь стал доступен на Windows

**Факт из исходников.** В `aten/src/ATen/native/cuda/Blas.cpp` (функция `addmm_out_cuda_impl`):

- **torch 2.2.1:**
  ```cpp
  #if defined(CUDA_VERSION) && CUDA_VERSION >= 11040 && !defined(_MSC_VER)
      ...
      if (!disable_addmm_cuda_lt) { useLtInterface = ... }
  ```
  На Windows (`_MSC_VER`) ветка **не компилируется вообще** → используется обычный `cublasSgemm`. Именно поэтому на 2.2.1 всё «просто работало», даже с кривым cublasLt.

- **torch 2.4+ (и 2.7.x):** гвард `_MSC_VER` убран. Ветка компилируется и на Windows, и включается по условиям:
  ```cpp
  static bool disable_addmm_cuda_lt = getDisableAddmmCudaLt();
  ...
  if (!disable_addmm_cuda_lt) {
      useLtInterface = beta == 1.0 && self.dim() == 1 && result.dim() == 2 &&
                       self.sizes()[0] == mat2_sizes[1] && ... ;
  }
  ```
  То есть **bias-addmm** (а это буквально каждый Linear-слой с bias в SD/Flux) уходит в `cublasLt`.

**Что это значит на ZLUDA:**
- ZLUDA `cublasLt` = hipBLASLt. На gfx803 его ядер нет → `CUBLAS_STATUS_NOT_SUPPORTED` в `cublasLtMatmulAlgoGetHeuristic`;
- `DISABLE_ADDMM_CUDA_LT=1` гасит **только эту** ветку. Но cublasLt-путь остаётся ещё в:
  - `mm`-ветках с `gemm_and_bias` (ROCm-путь; в cu118-сборке обычно нет),
  - `bmm`/`baddbmm` для некоторых форм,
  - `_scaled_mm` (fp8, нам не нужно),
  - `torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction` — не cublasLt, но тоже чувствительно.

### Лечение (в порядке возрастания усилий)

**Вариант 1 (штатный).** В лаунчере:
```bat
set DISABLE_ADDMM_CUDA_LT=1
```
В ComfyUI проверка уже есть в `comfy/zluda.py` (`os.environ['DISABLE_ADDMM_CUDA_LT'] = '1'`), но **только** если импортируется `comfy.zluda` **до** первого использования torch. Убедиться, что `comfy\zluda.py` на месте и импортируется (в `comfy/model_management.py` есть `import comfy.zluda`).

**Вариант 2 (надёжный).** Не дать torch вообще загрузить настоящий `cublasLt.dll`: подложить в `venv\Lib\site-packages\torch\lib\` **заглушку** `cublasLt64_12.dll` (имя, которое ищет cu118-колесо), которая на `cublasLtCreate` возвращает `CUBLAS_STATUS_NOT_SUPPORTED`, а остальные функции — no-op. Тогда torch детектит «cublasLt нет» и уходит в `cublas*`.

Пример (собрать в MSVC, x64):
```cpp
// cublaslt_stub.cpp  -> cublasLt64_12.dll
extern "C" __declspec(dllexport) int cublasLtCreate(void**) { return 15; } // NOT_SUPPORTED
```
> Возвращаемое значение смотрите в `cuda_types`: `CUBLAS_STATUS_NOT_SUPPORTED = 15`. Проверить, что torch действительно проверяет код возврата `cublasLtCreate` при инициализации.

**Вариант 3 (ребилд).** Патч ZLUDA `zluda_blaslt`: ленивая загрузка `hipblaslt.dll` (см. `04-rebuild-zluda.md#a`). Это лучшее долгосрочное решение, потому что чинит и `addmm`, и будущие cublasLt-вызовы.

---

## 2. Что ещё изменилось в 2.4→2.7, что бьёт по Polaris/ZLUDA

| Изменение | Симптом | Что делать |
|---|---|---|
| `torch.compile`/inductor по умолчанию агрессивнее | падения при первом запуске, долгая компиляция | `--disable-comfy-compiler`, `TORCHDYNAMO_DISABLE=1`, `TORCHINDUCTOR_DISABLE=1` |
| Новый async weight offload в ComfyUI 0.4x | зависания, «Bad Image» | `--disable-async-offload` |
| Pinned memory по умолчанию | ошибки/зависания на Polaris | `--disable-pinned-memory` |
| `cudaMallocAsync` (если кто-то включил) | падения аллокатора | не включать; `cuda_malloc.py` для cu118 его и не включает |
| Dynamic VRAM (новый менеджер памяти) | «странные» OOM | `--disable-dynamic-vram` |
| SDPA: cudnn_sdp/flash_sdp доступны | выбор неподдерживаемого бэкенда | `comfy/zluda.py` уже гасит flash/mem_efficient; при падениях — `torch.backends.cuda.enable_cudnn_sdp(False)` |
| bf16-аккумуляция в некоторых путях | медленно/неверно на gfx803 | `--force-fp16`, следить за качеством |

---

## 3. Рекомендуемый лаунчер (см. `scripts/comfyui-user.bat`)

```bat
set DISABLE_ADDMM_CUDA_LT=1
set ZLUDA_COMGR_LOG_LEVEL=1
set ZLUDA_CACHE_DIR=D:\zluda-cache
set MIOPEN_FIND_MODE=2
set MIOPEN_LOG_LEVEL=3
set PYTHONUNBUFFERED=1
rem по желанию:
rem set ZLUDA_WAVE64_SLOW_MODE=1
rem set TORCHDYNAMO_DISABLE=1

set COMMANDLINE_ARGS=--auto-launch --use-quad-cross-attention --reserve-vram 0.9 --disable-async-offload --disable-pinned-memory --disable-comfy-compiler --disable-dynamic-vram

call venv\Scripts\activate.bat
.\zluda\zluda.exe -- python main.py %COMMANDLINE_ARGS%
```

Аргументы подобраны так, чтобы **минимально зависеть от новых подсистем ComfyUI** (которые рассчитаны на ROCm/CUDA-стеки, а не на ZLUDA на Polaris).

---

## 4. Если нужно остаться на 2.2.1, но с HIP 6.4

Это **тоже возможный** путь: ZLUDA 3.9.6 + HIP 6.4 + torch 2.2.1 (при условии, что rocBLAS с gfx803 решён). Плюс: никакого cublasLt на Windows. Минус: ComfyUI новее 0.3.x требует torch ≥ 2.4 в некоторых местах (см. `requirements.txt` и `comfy/`), плюс отсутствуют фиксы 2.4+.

**Практический совет:** если цель — «работающий ComfyUI», сначала поднимите **HIP 6.4 + 2.2.1** (проверка этапа 1), и только потом двигайтесь на 2.7. Так вы отделяете проблему rocBLAS от проблемы cublasLt.
