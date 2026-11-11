# Почему ComfyUI + ZLUDA падает с «CUDA error: out of memory» на RX 480–580

Все ссылки ниже проверены по исходникам, скачанным по конкретным тегам и коммитам.
Каждый пункт — это то, что реально есть в файле, а не предположение.

**Не проверено на железе.** Ни одна строка не выполнялась на RX 580. Цепочка
выведена чтением кода; из внешних наблюдений есть один лог с RX 580 и несколько
сообщений в трекерах.

---

## Цепочка

### 1. PyTorch 2.3.0 перевёл `.item()` на pinned-память

Коммит `579999a7313cbd05d46bd1dbd7b26a271cb43818`, PR
[#119202 «Back scalar value to pinned memory for .item()»](https://github.com/pytorch/pytorch/pull/119202)
(2024-02-08). Впервые попал в релиз 2.3.0. Мотивация в PR — выигрыш около 3 %.

`aten/src/ATen/native/cuda/CUDAScalar.cu`:

**v2.2.1** — значение копируется в обычную переменную на стеке:

```cpp
Scalar _local_scalar_dense_cuda(const Tensor& self) {
  Scalar r;
  AT_DISPATCH_ALL_TYPES_AND_COMPLEX_AND4(..., [&] {
      scalar_t value;
      cudaStream_t stream = at::cuda::getCurrentCUDAStream();
      at::cuda::memcpy_and_sync(&value, self.const_data_ptr<scalar_t>(),
                                sizeof(scalar_t), cudaMemcpyDeviceToHost, stream);
      r = Scalar(value);
    });
  return r;
}
```

**v2.7.1** — выделяется pinned-тензор:

```cpp
// Create pinned memory for the scalar value to avoid implicit
// locking/sync in cuda library due to pageable memory
auto value = at::detail::empty_cpu(
  {1}, /* size */ c10::CppTypeToScalarType<scalar_t>(), /* dtype */
  std::nullopt, std::nullopt,
  true, /* pin_memory */
  std::nullopt);
```

Это значит, что `.item()`, `bool(tensor)`, `float(tensor)`, `int(tensor)` и
`if tensor:` теперь выделяют pinned-память.

### 2. Pinned-память выделяется через `cudaHostAlloc`

`aten/src/ATen/cuda/CachingHostAllocator.cpp:92-98` (v2.7.1):

```cpp
if (c10::cuda::CUDACachingAllocator::CUDAAllocatorConfig::
        pinned_use_cuda_host_register()) {
  allocWithCudaHostRegister(ptr, size);
} else {
  // Use cudaHostAlloc for allocating pinned memory (global lock in driver)
  C10_CUDA_CHECK(cudaHostAlloc(ptr, size, cudaHostAllocDefault));
}
```

Освобождение — `cudaFreeHost` (`:118`). Кэшированные блоки освобождаются только
из `empty_cache()`.

Другие внутренние выделения pinned-памяти:

* v2.7.1 `aten/src/ATen/native/cuda/Nonzero.cu` — `pinned_num_nonzeros_h` через
  `at::detail::empty_cpu(..., pin_memory=true)`;
* v2.7.1 `aten/src/ATen/native/cuda/TensorShape.cu:255` — `pack_vecs`.

В v2.2.1 pinned-память в этих файлах не выделялась вообще.

### 3. ZLUDA передаёт флаги в HIP без изменений

`zluda/src/cuda.rs:675-681`, ZLUDA v3.9.5 (коммит `5e71745`); в v3.9.6
(коммит `854c58e`) тот же код на строках 681-687:

```rust
pub(crate) unsafe fn cuMemHostAlloc(
    pp: *mut *mut ::std::os::raw::c_void,
    bytesize: usize,
    flags: ::std::os::raw::c_uint,
) -> hipError_t {
    hipHostMalloc(pp, bytesize, flags)
}
```

PyTorch передаёт `cudaHostAllocDefault` = 0.

### 4. HIP при `flags == 0` требует SVM с атомарными операциями

`hipamd/src/hip_memory.cpp:571` (ROCm/clr, тег `rocm-5.7.1`):

```cpp
unsigned int ihipFlags = CL_MEM_SVM_FINE_GRAIN_BUFFER;
if (flags == 0 ||
    flags & (hipHostMallocCoherent | hipHostMallocMapped | hipHostMallocNumaUser) ||
    (!(flags & hipHostMallocNonCoherent) && HIP_HOST_COHERENT)) {
  ihipFlags |= CL_MEM_SVM_ATOMICS;
}
```

### 5. Если устройство не поддерживает атомики, память не выделяется вообще

`rocclr/platform/context.cpp:326-363` (`Context::svmAlloc`):

```cpp
if (curDev != nullptr) {
  if (!(flags & CL_MEM_SVM_ATOMICS) ||
      (curDev->info().svmCapabilities_ & CL_DEVICE_SVM_ATOMICS)) {
    svmPtrAlloced = curDev->svmAlloc(*this, size, alignment, flags, svmPtrAlloced);
    ...
  }
}

for (const auto& dev : svmAllocDevice_) {
  if (dev == curDev) continue;
  // check if the device support svm platform atomics,
  // skipped allocation for platform atomics if not supported by this device
  if ((flags & CL_MEM_SVM_ATOMICS) &&
      !(dev->info().svmCapabilities_ & CL_DEVICE_SVM_ATOMICS)) {
    continue;
  }
  ...
}
return svmPtrAlloced;   // остаётся nullptr
```

Обе ветки пропускаются — возвращается `nullptr`.

### 6. Polaris не получает `CL_DEVICE_SVM_ATOMICS`

`rocclr/device/pal/paldevice.cpp:565-575` (Windows-бэкенд, `rocm-5.7.1`):

```cpp
if (settings().oclVersion_ >= OpenCL20) {
  info_.svmCapabilities_ = (CL_DEVICE_SVM_COARSE_GRAIN_BUFFER | CL_DEVICE_SVM_FINE_GRAIN_BUFFER);
  if (settings().svmAtomics_) {
    info_.svmCapabilities_ |= CL_DEVICE_SVM_ATOMICS;
  }
  ...
  if (amd::IS_HIP && ipLevel_ >= Pal::GfxIpLevel::GfxIp9) {
    info_.svmCapabilities_ |= CL_DEVICE_SVM_ATOMICS;
  }
```

`settings().svmAtomics_` зависит от поддержки IOMMUv2, которой на десктопном
Windows для Polaris нет. RX 480/570/580 — это Polaris10/11/12, то есть **GfxIp8**.
Vega (gfx900) и новее — GfxIp9+, поэтому у них всё работает.

Тот же гейт присутствует и в `rocm-5.7.0`, и в `rocm-6.2.4` — обновление ROCm
его не снимает.

### 7. Ошибка возвращается как «out of memory»

`hipamd/src/hip_memory.cpp:274-318` (`ihipMalloc`):

```cpp
*ptr = amd::SvmBuffer::malloc(*amdContext, flags, sizeBytes, dev_info.memBaseAddrAlign_,
            useHostDevice ? curDevContext->svmDevices()[0] : nullptr);

if (*ptr == nullptr) {
  if (!useHostDevice) {
    ...
  } else {
    LogPrintfError("Allocation failed : Pinned Memory, size :%zu \n", sizeBytes);
  }
  return hipErrorOutOfMemory;
}
```

`hipErrorOutOfMemory = 2` (`include/hip/hip_runtime_api.h:222`, ROCm/HIP
`rocm-5.7.1`), что численно совпадает с `CUDA_ERROR_OUT_OF_MEMORY = 2`.
Поэтому PyTorch печатает:

```
CUDA error: out of memory
```

— хотя видеопамяти свободно.

---

## Почему это выглядит именно так

* Ломается **ровно** начиная с 2.3.0 — версии, в которую попал PR #119202.
* Ошибка возникает на первых же шагах семплера: в `comfy/samplers.py` есть
  `torch.count_nonzero(latent_image) > 0`, а это и `.item()`-подобное извлечение
  скаляра, и `count_nonzero` с его pinned-буфером.
* На Vega (gfx900) с тем же ZLUDA и torch 2.3.1+cu118 проблем нет — там
  `CL_DEVICE_SVM_ATOMICS` включён.
* Сообщения о «out of memory» на RX 480/570/580 в трекере ComfyUI-Zluda
  (#10, #32, #441) идут именно с torch ≥ 2.3.
* Лог с RX 580 (`Failed to create hostcall buffer` / `Kernel expects a hostcall
  buffer, but none found`, ZLUDA #38) приходит из того же `svmAlloc`
  (`palvirtual.cpp:3781`, `palkernel.cpp:344`) — там он не фатален, но причина та же.

---

## Как подтвердить у себя

На **рабочем** torch 2.2.1, под ZLUDA:

```bat
run_diagnose.bat venv --check pin
```

Ожидание, если диагноз верен: `CUDA error: out of memory`.

Дополнительно:

```bat
set AMD_LOG_LEVEL=1
```

В логе должна появиться строка `Allocation failed : Pinned Memory, size :N`.

И контрольное сравнение:

```bat
run_diagnose.bat venv --check item      :: на 2.2.1 проходит
run_diagnose.bat venv-torch27 --check item   :: на 2.7 падает без заплаток
```

---

## Правильное место для починки — ZLUDA

Тorch-side заплатки в этом наборе — обход. По-хорошему чинить надо в ZLUDA:
`cuMemHostAlloc` не должен требовать от устройства атомиков, которых у него нет.

Псевдодифф к `zluda/src/cuda.rs` (**не компилировался** — в этой среде нет
тулчейна Rust):

```rust
pub(crate) unsafe fn cuMemHostAlloc(
    pp: *mut *mut ::std::os::raw::c_void,
    bytesize: usize,
    flags: ::std::os::raw::c_uint,
) -> hipError_t {
    let result = hipHostMalloc(pp, bytesize, flags);
    if result == hipError_t::hipErrorOutOfMemory && flags == 0 {
        // Устройство без CL_DEVICE_SVM_ATOMICS (gfx803 и старше) не может
        // выполнить hipHostMalloc с флагами по умолчанию: HIP добавляет
        // CL_MEM_SVM_ATOMICS и Context::svmAlloc возвращает nullptr.
        // Пробуем некогерентное выделение — оно атомиков не требует.
        return hipHostMalloc(pp, bytesize, hipHostMallocNonCoherent);
    }
    result
}
```

Что при этом известно, а что нет:

* `hipHostMalloc` при `hipHostMallocNonCoherent` снимает `CL_MEM_SVM_ATOMICS`
  (`hip_memory.cpp:601-603`) — это проверено по исходникам;
* останется `CL_MEM_SVM_FINE_GRAIN_BUFFER`, то есть выделение пойдёт по
  `svmAlloc` без требования атомиков;
* **не проверено**, что такое выделение вообще удаётся на Polaris под Windows
  и что данные будут когерентны: без атомиков PAL не выставляет
  `gl2CacheDisabled_` (`palresource.cpp`), а поведение fine-grain SVM без
  когерентности на gfx803 никто не тестировал.

Поэтому этот путь — кандидат для апстрима, а не готовое решение.

---

## Что было отвергнуто

| вариант | почему не подходит |
|---|---|
| Байтовый патч `torch_cuda.dll` на диске | wheel PyTorch для Windows недоступен для анализа; ломает подписи и обновления |
| Колбэк Python через ctypes в качестве цели хука | требует GIL, при `KeyboardInterrupt` внутри возвращает 0 и нулевой указатель, небезопасен при завершении интерпретатора |
| Перенаправление импорта на `cudaMallocHost` | ZLUDA не реализует `cuMemAllocHost_v2` |
| `PYTORCH_CUDA_ALLOC_CONF=pinned_use_cuda_host_register:True` как основное решение | может упасть в `registerPages()` на проверке совпадения host/device указателей; оставлено как опция для проверки |
| Патч только `Tensor.item` | не закрывает `Nonzero.cu` и `TensorShape.cu` — они выделяют pinned-память изнутри C++ |
| `DISABLE_ADDMM_CUDA_LT` как причина OOM | disproved: исключение MSVC для cublasLt снято только в v2.4.0, а добавленные в 2.3.0 `cublasLtCreate`/`cublasLtDestroy` находятся под `#ifdef USE_ROCM` |

---

## Источники

* pytorch/pytorch `v2.2.1`, `v2.3.0`, `v2.7.1` — файлы
  `aten/src/ATen/native/cuda/CUDAScalar.cu`, `Nonzero.cu`, `TensorShape.cu`,
  `aten/src/ATen/cuda/CachingHostAllocator.cpp`, `aten/src/ATen/core/CachingHostAllocator.h`
* pytorch/pytorch коммит `579999a7313cbd05d46bd1dbd7b26a271cb43818` (PR #119202)
* lshqqytiger/ZLUDA `5e71745` (v3.9.5) и `854c58e` (v3.9.6) — `zluda/src/cuda.rs`;
  в v3.9.6 также `zluda/src/impl/mod.rs:494-505` (`ZLUDA_CACHE_DIR`)
* ROCm/clr `rocm-5.7.1` — `hipamd/src/hip_memory.cpp`,
  `rocclr/platform/context.cpp`, `rocclr/device/pal/paldevice.cpp`
* ROCm/HIP `rocm-5.7.1` — `include/hip/hip_runtime_api.h`
* patientx/ComfyUI-Zluda — issues #10, #32, #441; `install-n.bat`, `patchzluda.bat`,
  `fixforrx580.bat`, `comfy/customzluda/zluda-default.py`
* lshqqytiger/ZLUDA — issues #38, #115, #138

---

## Подтверждение на живом железе

Прогон `run_diagnose.bat` на RX 580 (Python 3.12.10, torch 2.7.1+cu118,
ZLUDA 3.9.6 rocm5) дал ровно то, что предсказывала цепочка выше:

```
[pin] pin_memory() - прямой вызов cudaHostAlloc
  ! RuntimeError: CUDA error: out of memory
```

Карта при этом видна и работает (`Radeon RX 580 Series [ZLUDA]`, 8 ГиБ),
то есть это не «нет драйвера» и не настоящая нехватка памяти: `hipHostMalloc`
возвращает `nullptr` из-за `CL_MEM_SVM_ATOMICS`, что и превращается в
`hipErrorOutOfMemory == CUDA_ERROR_OUT_OF_MEMORY`.

Отдельный результат, который **опровергает** один из запланированных способов
лечения: опрос таблицы импорта 38 загруженных модулей не нашёл ни одного слота
`cudaHostAlloc` / `cudaFreeHost` из `cudart*`. Значит `torch_cuda.dll` не
импортирует эти функции через IAT, и уровень 3 (подмена слота) неприменим.
Проверка `imports` уточняет механизм: имена ищутся ещё и в сырых байтах файла,
и их наличие без записи в IAT означает разрешение через `GetProcAddress`.

## Второй результат прогона: matmul падал не из-за pinned-памяти

`cublasSgemm` (fp32) и `cublasGemmEx` (fp16) возвращали
`CUBLAS_STATUS_NOT_SUPPORTED`. Проверка `libs` показала причину: в `torch\lib`
лежали оригинальные библиотеки NVIDIA — `cublas64_11.dll` 84.56 МиБ,
`cusparse64_11.dll` 264.59 МиБ, `cufft64_10.dll` 266.91 МиБ. Подмена не
выполнилась, потому что на момент установки ZLUDA ещё не скачался, и torch
вызывал настоящий cuBLAS от NVIDIA поверх драйверного слоя ZLUDA.

Заглушки ZLUDA (`cublas.dll`, `cusparse.dll`, `nvrtc.dll`, `cufft.dll`,
`cufftw.dll`) работают поверх rocBLAS/rocFFT, и именно они нужны. Скрипт форка
`zluda396-test-hip571.bat` для ZLUDA 3.9.6 + HIP 5.7.1 копирует ровно этот
набор, что подтверждает состав архива `ZLUDA-windows-rocm5-amd64.zip`.
