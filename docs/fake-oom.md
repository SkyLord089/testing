# Fake OOM на PyTorch 2.3+ под ZLUDA: причина и фикс

**Симптом:** генерация падает с `torch.OutOfMemoryError`, при этом:

- `Free (according to CUDA): 0 bytes` при пустой карте,
- или `Allocation on device 0 would exceed allowed memory ... Device limit : 8.00 GiB`,
- "PyTorch limit (set by user-supplied memory fraction): 17179869184.00 GiB",
- либо `torch.cuda.mem_get_info()` внутри ComfyUI показывает 0 свободных байт, хотя видеопк пуста,
- на torch 2.2.1 при этом всё работало, а на 2.3+ — нет.

Это **«фальшивый» OOM**: памяти на самом деле хватает, проблема в том, *кто и как считает* память.

## Что на самом деле поменяли (по исходникам ComfyUI)

ComfyUI при старте сам выбирает CUDA-аллокатор (`cuda_malloc.py`):

```python
if int(version[0]) >= 2 and "+cu" in version:   # torch>=2.0 И CUDA-сборка (наша cu118)
    if cuda_version >= 130: args.cuda_malloc = True
    else:                   args.cuda_malloc = cuda_malloc_supported()  # True, AMD не в блэклисте
```

и дальше, если `args.cuda_malloc`:

```python
env_var = os.environ.get('PYTORCH_CUDA_ALLOC_CONF', None)
env_var = (env_var + "," if env_var else "") + "backend:cudaMallocAsync"
os.environ['PYTORCH_CUDA_ALLOC_CONF'] = env_var
```

То есть **ComfyUI без вопросов переключает torch ≥ 2.0 + `+cu*` на stream-ordered аллокатор
`cudaMallocAsync`** (через `cuMemAllocAsync` / mempool). Блэклист в `cuda_malloc_supported()`
перечисляет только старые NVIDIA-карты — AMD туда не попадает, поэтому на RX 580
`cudaMallocAsync` включается всегда.

А ZLUDA этот режим поддерживает криво/частично: `cuMemAllocAsync`/mempool под HIP 6.4 на
Polaris либо возвращают ошибку, либо `mem_get_info` начинает возвращать мусор/нули.
Новые версии torch (2.3+) перестали молча откатываться в обычный кэширующий аллокатор —
поэтому проявляется «не хватало» памяти, которой полно. Сам ComfyUI печатает подсказку:
*"this card most likely does not support cuda-malloc, if you get CUDA error please run
ComfyUI with: --disable-cuda-malloc"* (`main.py: cuda_malloc_warning`).

«2.2.1 работал» — потому что с ним ComfyUI тоже включал cudaMallocAsync, но ошибки этого
аллокатора в связке со старым torch не доводили дело до exception, либо fallback проходил.
С 2.3+ — нет.

## Фикс — уже встроен в наш `run-comfyui.bat`

1. **`--disable-cuda-malloc`** в аргументах ComfyUI — отменяет принудительный
   `backend:cudaMallocAsync`. torch возвращается на обычный caching allocator.
2. **`--disable-smart-memory`** — ComfyUI перестаёт гейтить аллокации по
   `torch.cuda.mem_get_info()` (под ZLUDA — ненадёжно) и просто доверяет реальной попытке аллокации.
3. **`PYTORCH_CUDA_ALLOC_CONF=expandable_segments:False`** — страховка: expandable segments
   построены на VMM API (`cuMemAddressReserve`/`cuMemMap`), которые ZLUDA не реализует.
   По умолчанию `expandable_segments` выключен, но явная фиксация снимает риск,
   что какой-то образ/инструкция включит его.
4. **`CUDA_MODULE_LOADING=LAZY`** — отложенная загрузка модулей: меньше пиковый расход памяти при импорте torch.

Если всё это есть — torch 2.7.0+cu118 на RX 580 должен грузить и генерировать без fake OOM.

## Если всё равно падает — по порядку

1. `PYTORCH_NO_CUDA_MEMORY_CACHING=1` (раскомментируйте строку в `run-comfyui.bat`) —
   полный обход кэширующего аллокатора (каждая аллокация идёт напрямую в драйвер).
   Медленно, зато fake OOM исчезает как класс явлений. Диагностический режим, которым можно и работать.
2. `PYTORCH_CUDA_ALLOC_CONF=expandable_segments:False,max_split_size_mb:512` —
   если падает по фрагментации (в ошибке «reserved by PyTorch but unallocated» сильно больше «allocated»).
3. `--disable-dynamic-vram` — динамическая VRAM ComfyUI рассчитана на NVIDIA и AMD ROCm ≥ 7.14;
   на torch 2.7 с ней и так fallback на legacy ModelPatcher, но флаг снимает вопросы.
4. `--disable-pinned-memory` — ComfyUI по умолчанию пиннит до 40% RAM (`cudaHostRegister`);
   под ZLUDA это сомнительная поддержка. Отключает — трансферы H2D будут чуть медленнее.
5. `--lowvram` / `--novram` — если 8 ГБ реально впритык (уже не fake OOM, а честный).
6. **Полный откат на torch 2.2.1+cu118** (то, что пинует ComfyUI-Zluda — самый скучный и проверенный вариант):
   ```powershell
   cd $env:USERPROFILE\RX580-ComfyUI\testing\scripts
   .\Install-ComfyUI-RX580.ps1 -TorchVersion 2.2.1 -VisionVersion 0.17.1 -AudioVersion 2.2.1
   ```

## Диагностика

```powershell
.\Verify-Setup.ps1 -MemoryReport
```

Печатает: версию torch, `mem_get_info` (free/total), `allocated`/`reserved`/`max_allocated`,
`allocator_backend` и зафиксированные `PYTORCH_*` переменные. Признаки fake OOM:
`free=0.00 GiB`, либо `allocator_backend=cudaMallocAsync` внутри ComfyUI-сессии
(значит, `--disable-cuda-malloc` потерялся из аргументов).

## Честность

Механизм восстановлен по исходникам ComfyUI (`cuda_malloc.py`, `main.py`, `comfy/cli_args.py`)
и открытым issues (например, [ComfyUI #12360](https://github.com/Comfy-Org/ComfyUI/issues/12360) —
та же сигнатура `Free (according to CUDA): 0 bytes` на AMD GPU, рекомендация та же:
`--disable-smart-memory`). На живой RX 580 мы это не гоняли — порядок действий отсортирован
от безболезненного к радикальному; если 2.7 не взлетит даже с `PYTORCH_NO_CUDA_MEMORY_CACHING=1`,
честный ответ — остаться на 2.2.1, как делает ComfyUI-Zluda.
