# Ребилд ZLUDA под HIP 6.4/7.1 и патчи-кандидаты

## 0. Что уже не надо делать

- **Не нужно «портировать» ZLUDA на 6.4.** В 3.9.6 это уже сделано: changelog релиза 2026‑02‑11 и `xtask/src/main.rs`, где путь HIP SDK подбирается автоматически:

  ```rust
  let hip_path = if rocm5 { env::var("HIP_PATH_57").or_else(|_| env::var("HIP_PATH_55")) }
  else {
      env::var("HIP_PATH_71")
          .or_else(|_| env::var("HIP_PATH_64"))
          .or_else(|_| env::var("HIP_PATH_62"))
          .or_else(|_| env::var("HIP_PATH_61"))
  };
  ```

- **Не нужно собирать `cudnn.dll`/`nccl.dll`** для базового сценария: в 3.9.6 они доступны только в `--nightly`, а обычная сборка их не содержит. На gfx803 MIOpen-путь всё равно почти наверняка не годится.

## 1. Сборка (Windows, HIP SDK 6.4.2)

Требования из README: Rust ≥ 1.89, CMake, Python 3, C++ компилятор (VS Build Tools), HIP SDK, свежий Adrenalin.

```powershell
git clone --recurse-submodules https://github.com/lshqqytiger/ZLUDA.git
cd ZLUDA

# Явно указать нужный SDK (важно, если установлено несколько)
$env:HIP_PATH = 'C:\Program Files\AMD\ROCm\6.4\'

# release-сборка под ROCm6 (HIP 6.4)
cargo xtask --release
```

Результат — в `target\release`: `cublas.dll`, `cublasLt.dll`, `cufft.dll`, `cufftw.dll`, `cusparse.dll`, `nvrtc.dll`, `nvml.dll`, `nvcuda.dll`, `zluda.exe`, `zluda_dump.dll`, `zluda_redirect.dll`.

> **Не собирайте `--rocm5`** и не смешивайте артефакты с ROCm5-сборкой: другой набор хидеров (`hip_runtime_api_v5.rs`) и другие символы.

### Быстрый локальный конфиг (Windows PowerShell)

```powershell
$env:HIP_PATH_64 = 'C:\Program Files\AMD\ROCm\6.4\'
$env:PATH = "$env:HIP_PATH_64\bin;$env:PATH"
cargo xtask --release
```

### Что проверить после сборки

```powershell
# версия коммита (в 3.9.6 добавлена zluda_get_version)
# и что nvcuda.dll собран под 64 бита
foreach ($f in 'cublas.dll','cublasLt.dll','nvcuda.dll','nvrtc.dll') {
  $p = "target\release\$f"
  $b = [IO.File]::ReadAllBytes((Resolve-Path $p))[0..1]
  "{0} -> PE machine 0x{1:X4} (0x8664 = x64)" -f $f, ([BitConverter]::ToUInt16($b,0) -as [int])
}
```

Ожидаемо: `0x8664`.

---

## 2. Патчи-кандидаты (когда стандартная сборка не тянет)

Ниже — точки в исходниках ZLUDA, где чаще всего «зарыта» несовместимость с Polaris и с torch 2.7. Каждый патч — самостоятельный эксперимент с проверкой.

### A. `cublasLt` → hipBLASLt: сделать загрузку ленивой/мягкой

**Проблема.** В 3.9.6 `zluda_blaslt` впервые собирается и для non-nightly Windows и линкуется прямо на `hipblaslt`. Если `hipblaslt.dll` нет (или в HIP SDK нет ядер для gfx803) — DLL либо не грузится (`WinError 126`), либо все `cublasLt*` возвращают `CUBLAS_STATUS_NOT_SUPPORTED`, что валит `addmm` в torch 2.5+.

**Идея.** При загрузке библиотеки (в `hipblaslt-sys/build.rs` / `zluda_blaslt/src/lib.rs`) заменить жёсткий линк на `LoadLibraryW("hipblaslt.dll")` + `GetProcAddress`, и при неудаче — тихо возвращать `CUBLAS_STATUS_NOT_SUPPORTED` для всех `cublasLt*`, **не роняя DLL**. Тогда torch падает в обычный `cublas*` путь (rocBLAS), где у нас есть шанс с gfx803-ядрами.

**Скелет (для обсуждения, не финальный код):**

```rust
// zluda_blaslt/src/lib.rs (эскиз)
use std::{ffi::CString, sync::OnceLock};

static HIPBLASLT: OnceLock<Option<libloading::Library>> = OnceLock::new();

fn get() -> Option<&'static libloading::Library> {
    HIPBLASLT.get_or_init(|| unsafe {
        let path = CString::new("hipblaslt.dll").unwrap();
        libloading::Library::new(path).ok()
    }).as_ref()
}

pub(crate) fn unsupported() -> cublasStatus_t {
    cublasStatus_t::CUBLAS_STATUS_NOT_SUPPORTED
}
```

и в каждом экспорте — `let Some(lib) = get() else { return unsupported() };` + вызов через `GetProcAddress`. Это большая правка на ~90 функций, но она механическая (её можно сгенерировать из `decl.rs`).

> Более дешёвая альтернатива: **не копировать `cublasLt.dll` в `venv\torch\lib`** и заменить его на «пустой» шим, который возвращает `CUBLAS_STATUS_NOT_SUPPORTED` из `cublasLtCreate` (см. `06-torch27.md#1`, вариант 2). Тогда torch уходит на `cublas*`.

### B. Wave64: принудительный быстрый режим

**Проблема.** gfx803 отдаёт `warpSize=64`, ZLUDA выбирает `DoubleWave32OnWave64` (медленно и местами нестабильно), `ZLUDA_WAVE64_SLOW_MODE=1` — `Wave32OnWave64` (медленнее, но стабильнее).

**Что попробовать:** запускать ComfyUI с `ZLUDA_WAVE64_SLOW_MODE=1` и сравнить: скорость падает, но если пропадают падения — это сигнал, что проблема в wave-режиме, а не в rocBLAS.

Код: `zluda/src/impl/device.rs::get_wave64_mode()`.

### C. ISA/gcnArchName: принудительная подмена

**Проблема.** `comgr_isa()` собирает строку `amdgcn-amd-amdhsa--<gcnArchName>`. Если HIP 6.4 отдаёт для Polaris `gfx803` без фич (или с суффиксом), comgr может принять её за неподдерживаемую.

**Что попробовать:** добавить переменную `ZLUDA_OVERRIDE_ISA` (2 строки в `hip_common/src/lib.rs::comgr_isa`) и проверить `gfx803`, `gfx803:sramecc-:xnack-`, `gfx900`.

### D. Compute capability 8.8

**Проблема.** ZLUDA репортует `COMPUTE_CAPABILITY_MAJOR=8`, `MINOR=8` (чтобы обмануть Blender). Часть кода torch/ComfyUI проверяет `get_device_capability()` и делает выводы (например, про fp8/bf16).

**Что попробовать:** в `comfy\zluda.py` уже есть `DeviceProperties.PROPERTIES_OVERRIDE`, но только для `regs_per_multiprocessor` и `gcnArchName`; при проблемах добавить туда же `major`/`minor` (например, 8/0).

### E. Память: утечки и «странные» аллокации

**Проблема.** На Polaris (по многочисленным отчётам) HIP иногда возвращает «успешную» аллокацию больше доступной VRAM, из-за чего потом падения в VAE.

**Что делать без патчей:**
- `--reserve-vram 0.9` (или больше),
- `--novram`, `--lowvram` при 4–8 ГБ,
- tiled VAE,
- `--disable-pinned-memory`, `--disable-async-offload`,
- `PYTORCH_CUDA_ALLOC_CONF=expandable_segments:False` — если начнутся странности с аллокатором.

---

## 3. Как собрать «свою» ветку ZLUDA под эксперимент

```powershell
git clone --recurse-submodules https://github.com/lshqqytiger/ZLUDA.git zluda-exp
cd zluda-exp
git switch -c exp/gfx803-hip64
# правим comgr/hip_common/zluda_blaslt...
$env:HIP_PATH='C:\Program Files\AMD\ROCm\6.4\'
cargo xtask --release
# артефакты в target\release — копируем в отдельную папку zluda-exp\bin,
# затем patch-torch-zluda.ps1 -ZludaDir .\zluda-exp\target\release
```

Держим **две** папки: `zluda-stable` (3.9.6 как есть) и `zluda-exp` (эксперименты). Переключение — только через скрипт с бэкапом, чтобы всегда можно было откатиться.

## 4. Полезные env-переменные ZLUDA 3.9.6

| Переменная | Значение | Смысл |
|---|---|---|
| `ZLUDA_COMGR_LOG_LEVEL` | `1` | лог компиляции ядер (видно «Compilation is in progress») |
| `ZLUDA_CACHE_DIR` | путь | куда класть кэш скомпилированных ядер |
| `ZLUDA_WAVE64_SLOW_MODE` | `1` | Wave32OnWave64 вместо DoubleWave32OnWave64 |
| `CUDA_MODULE_LOADING` | `EAGER` | грузить модули сразу (полезно для диагностики) |
| `AMD_LOG_LEVEL` | `3` | подробный лог HIP (видно, что видит драйвер) |
