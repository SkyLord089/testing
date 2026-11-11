"""Диагностика: что именно ломается на конкретной машине.

Каждая проверка выполняется **в отдельном процессе**: падение драйвера или
``SIGSEGV`` в одной проверке не должны мешать остальным.

    python -m zluda_rx580.diagnose              # все проверки
    python -m zluda_rx580.diagnose --list       # список проверок
    python -m zluda_rx580.diagnose --check item
    python -m zluda_rx580.diagnose --patch      # то же, но с патчем cudaHostAlloc

Проверка ``patch`` — та самая, которая подтверждает или опровергает диагноз:
если ``pin`` падает с ``CUDA error: out of memory``, а с ``--patch`` проходит,
значит причина именно в выделении pinned-памяти.

Ключевые проверки
-----------------
``pin``       ``torch.empty(16).pin_memory()`` — прямой вызов ``cudaHostAlloc``.
              На torch 2.2.1 тоже падает, если диагноз верен.
``item``      ``tensor.sum().item()`` — с PyTorch 2.3.0 внутри выделяет
              pinned-память (aten/src/ATen/native/cuda/CUDAScalar.cu).
``nonzero``   ``torch.count_nonzero(x) > 0`` — та самая строка из
              ``comfy/samplers.py``, на которой обрывалась генерация.
``roundtrip`` сверяет значения, полученные через ``.item()`` и через ``.cpu()``:
              патч не должен менять результаты.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from typing import Callable, Dict, List, Optional, Tuple

MARK = "ZLRX580"

CHECKS: List[Tuple[str, str]] = [
    ("env", "окружение, версия PyTorch, файлы ZLUDA"),
    ("device", "виден ли GPU, имя устройства, объём памяти"),
    ("matmul", "матричное умножение на GPU (cuBLAS/rocBLAS)"),
    ("item", ".item() — выделяет pinned-память начиная с PyTorch 2.3"),
    ("pin", "pin_memory() — прямой вызов cudaHostAlloc"),
    ("nonzero", "count_nonzero(...) > 0 — строка из comfy/samplers.py"),
    ("roundtrip", "сверка значений .item() и .cpu()"),
    ("shim", "Python-заплатки вместо выделения pinned-памяти"),
    ("patch", "все проверки памяти после подмены cudaHostAlloc"),
    ("libs", "чьи DLL в torch/lib - NVIDIA или ZLUDA"),
    ("imports", "откуда torch берёт cudaHostAlloc: IAT или GetProcAddress"),
    ("blas", "найдёт ли rocBLAS библиотеку Tensile для gfx803"),
]
CHECK_NAMES = [name for name, _ in CHECKS]


# ---------------------------------------------------------------------------
# вспомогательное для дочернего процесса
# ---------------------------------------------------------------------------


def _info(text: str) -> None:
    print("%s INFO %s" % (MARK, text), flush=True)


def _detail(text: str) -> None:
    for line in str(text).splitlines() or [""]:
        print("%s DETAIL %s" % (MARK, line), flush=True)


def _result(name: str, status: str) -> None:
    print("%s RESULT %s %s" % (MARK, name, status), flush=True)


def _torch():
    import torch  # noqa: PLC0415

    return torch


# ---------------------------------------------------------------------------
# проверки
# ---------------------------------------------------------------------------


def check_env(apply_patch: bool) -> bool:
    _info("python %s (%s)" % (sys.version.split()[0], sys.executable))
    _info("platform %s" % sys.platform)
    try:
        import torch

        _info("torch %s" % torch.__version__)
        _info("torch.version.cuda %r" % torch.version.cuda)
        _info("torch.version.hip %r" % getattr(torch.version, "hip", None))
        _info("torch built with USE_ROCM: %s" % ("yes" if torch.version.hip else "no"))
        lib_dir = os.path.join(os.path.dirname(os.path.abspath(torch.__file__)), "lib")
        interesting = []
        try:
            for name in sorted(os.listdir(lib_dir)):
                lowered = name.lower()
                if lowered.startswith(("cudart", "cublas", "cusparse", "nvrtc", "cudnn", "zluda")):
                    interesting.append(name)
        except OSError as exc:
            _info("cannot list %s: %s" % (lib_dir, exc))
        _info("torch/lib CUDA runtime files: %s" % (", ".join(interesting) or "<none>"))
        from zluda_rx580.runtime import zluda_evidence

        evidence = zluda_evidence()
        _info("ZLUDA installation detected: %s" % bool(evidence))
        for item in evidence:
            _info("  %s" % item)
        if not evidence:
            _detail(
                "zluda\\zluda.exe не найден. nvrtc_cuda.dll в torch/lib признаком "
                "не считается - install_comfyui.bat создаёт его копией штатного "
                "nvrtc64_112_0.dll в любом случае. Запустите install_zluda.bat"
            )
    except Exception as exc:
        _detail("torch is not importable: %r" % (exc,))
        return False

    if os.name == "nt":
        injected = []
        try:
            from zluda_rx580.pinpatch import loaded_modules

            injected = [
                os.path.basename(path)
                for _base, path in loaded_modules()
                if os.path.basename(path).lower().startswith("zluda")
            ]
        except Exception as exc:
            _detail("cannot list loaded modules: %s" % exc)
        if injected:
            _info("ZLUDA injected into this process: %s" % ", ".join(sorted(set(injected))))
        else:
            _info("ZLUDA injected into this process: NO")
            _detail(
                "процесс запущен БЕЗ zluda.exe - CUDA здесь не появится, "
                "ошибка \"Found no NVIDIA driver\" ожидаема. "
                "Используйте run_diagnose.bat: он сам запускает проверки через zluda.exe"
            )

    if os.name == "nt":
        try:
            from zluda_rx580.pinpatch import resolve_crt_allocators

            aligned_malloc, aligned_free = resolve_crt_allocators()
            _info("ucrtbase _aligned_malloc=0x%x _aligned_free=0x%x" % (aligned_malloc, aligned_free))
        except Exception as exc:
            _detail("CRT allocators unavailable: %s" % exc)
    return True


def check_device(apply_patch: bool) -> bool:
    torch = _torch()
    if not torch.cuda.is_available():
        _detail("torch.cuda.is_available() == False")
        return False
    _info("device count %d" % torch.cuda.device_count())
    for index in range(torch.cuda.device_count()):
        props = torch.cuda.get_device_properties(index)
        _info(
            "gpu%d %s  total=%.2f GiB  capability=%s.%s"
            % (
                index,
                props.name,
                props.total_memory / 1024 ** 3,
                getattr(props, "major", "?"),
                getattr(props, "minor", "?"),
            )
        )
    return True


def check_matmul(apply_patch: bool) -> bool:
    """fp32 и fp16 проверяются раздельно.

    На живом RX 580 fp16-вариант упал с CUBLAS_STATUS_NOT_SUPPORTED: ZLUDA не
    поддерживает CUBLAS_GEMM_DEFAULT_TENSOR_OP для half. Если fp32 при этом
    работает, ComfyUI можно запускать с --force-fp32.
    """
    torch = _torch()
    fp32_ok = False
    for dtype in (torch.float32, torch.float16):
        label = "float32" if dtype == torch.float32 else "float16"
        try:
            a = torch.randn(256, 256, device="cuda", dtype=dtype)
            b = torch.randn(256, 256, device="cuda", dtype=dtype)
            c = (a @ b).float()
            torch.cuda.synchronize()
            reference = a.float().cpu() @ b.float().cpu()
            error = (c.cpu() - reference).abs().max().item()
            _info("matmul %-8s max |GPU - CPU| = %.6g" % (label, error))
            if error >= 1.0:
                _detail("matmul %s: расхождение с CPU слишком велико" % label)
                if dtype == torch.float32:
                    return False
                continue
            if dtype == torch.float32:
                fp32_ok = True
        except Exception as exc:
            message = str(exc).splitlines()[0]
            _info("matmul %-8s не работает: %s" % (label, message[:160]))
            if dtype == torch.float32:
                _detail("float32 matmul - обязательный минимум, без него генерация невозможна")
                return False
            _detail(
                "float16 matmul недоступен - запускайте ComfyUI с --force-fp32, "
                "иначе модель в fp16 упадёт на первом же слое"
            )
    return fp32_ok


def check_item(apply_patch: bool) -> bool:
    torch = _torch()
    value = torch.zeros(4, device="cuda").sum()
    _info("sum().item() = %s" % value.item())
    value2 = torch.arange(1, 17, device="cuda", dtype=torch.float32).sum()
    _info("arange sum .item() = %s" % value2.item())
    return True


def check_pin(apply_patch: bool) -> bool:
    torch = _torch()
    sizes = [1, 16, 4096, 1 << 20]
    tensors = []
    for size in sizes:
        tensor = torch.empty(size, dtype=torch.uint8).pin_memory()
        tensors.append(tensor)
        _info("pin_memory(%d bytes) -> 0x%x" % (size, tensor.data_ptr()))
    for tensor in tensors:
        tensor.copy_(torch.zeros(tensor.numel(), dtype=torch.uint8))
    _info("pinned tensors written from CPU: ok")
    return True


def check_nonzero(apply_patch: bool) -> bool:
    torch = _torch()
    latent = torch.randn(1, 4, 8, 8, device="cuda", dtype=torch.float16)
    count = torch.count_nonzero(latent)
    _info("count_nonzero = %s" % count)
    flag = count > 0
    _info("count_nonzero(...) > 0 = %s" % bool(flag))
    return True


def check_roundtrip(apply_patch: bool) -> bool:
    torch = _torch()
    host = (torch.arange(1, 4097, dtype=torch.float32) / 8.0)
    gpu = host.to("cuda")
    problems: List[str] = []

    gpu_sum = gpu.sum()
    item_value = gpu_sum.item()
    cpu_value = gpu_sum.cpu().item()
    _info("sum via .item()=%.9g  via .cpu()=%.9g" % (item_value, cpu_value))
    if item_value != cpu_value:
        problems.append("sum mismatch: %r vs %r" % (item_value, cpu_value))

    mask = gpu > 100.0
    mask_item = int(mask.sum().item())
    mask_cpu = int(mask.sum().cpu().item())
    _info("mask count via .item()=%d  via .cpu()=%d" % (mask_item, mask_cpu))
    if mask_item != mask_cpu:
        problems.append("mask count mismatch")

    nonzero_item = int(torch.count_nonzero(gpu).item())
    nonzero_cpu = int(torch.count_nonzero(gpu).cpu().item())
    _info("count_nonzero via .item()=%d  via .cpu()=%d" % (nonzero_item, nonzero_cpu))
    if nonzero_item != nonzero_cpu:
        problems.append("count_nonzero mismatch")

    copy_back = gpu.cpu()
    identical = bool(torch.equal(copy_back, host))
    _info("GPU->CPU copy identical to source: %s" % identical)
    if not identical:
        problems.append("copy round trip differs")

    pinned = torch.empty(64, dtype=torch.float32)
    pinned.copy_(host[:64])
    moved = pinned.to("cuda").cpu()
    pinned_ok = bool(torch.equal(moved, host[:64]))
    _info("pinned H2D->D2H identical: %s" % pinned_ok)
    if not pinned_ok:
        problems.append("pinned round trip differs")

    if problems:
        _detail("; ".join(problems))
        return False
    return True


# Порядок поиска библиотеки Tensile повторяет rocBLAS 5.7.1,
# library/src/tensile_host.cpp:622-696. Отклоняться от него нельзя: если
# проверка будет искать не там, где ищет rocBLAS, её результат ничего не стоит.
TENSILE_CANDIDATES = ("../../Tensile/library", "library", "rocblas/library")
TENSILE_FILENAMES = ("TensileLibrary_lazy_%s.dat", "TensileLibrary_%s.dat", "TensileLibrary.dat")


def check_blas(apply_patch: bool) -> bool:
    """Найдёт ли rocBLAS свою библиотеку Tensile для gfx803.

    ZLUDA реализует cuBLAS поверх rocBLAS (zluda_blas -> rocblas-sys), поэтому
    без TensileLibrary.dat умножение матриц не работает вовсе. На живой машине
    rocBLAS искал C:\\Program Files\\AMD\\ROCm\\5.7\\bin\\rocblas\\library\\TensileLibrary.dat,
    не нашёл и аварийно завершился.
    """
    if os.name != "nt":
        _detail("актуально только для Windows")
        return False

    arch = "gfx803"
    override = os.environ.get("ROCBLAS_TENSILE_LIBPATH", "")
    if override:
        _info("ROCBLAS_TENSILE_LIBPATH = %s (rocBLAS возьмёт только его)" % override)

    # Откуда rocBLAS берёт базовый каталог: путь к загруженной rocblas.dll.
    rocblas_dirs = []
    try:
        from zluda_rx580.pinpatch import loaded_modules

        for _base, path in loaded_modules():
            if os.path.basename(path).lower() == "rocblas.dll":
                rocblas_dirs.append(os.path.dirname(path))
    except Exception as exc:
        _detail("не удалось перечислить модули: %s" % exc)

    for var in ("HIP_PATH", "ROCM_PATH"):
        value = os.environ.get(var, "")
        if value:
            _info("%s = %s" % (var, value))
            candidate = os.path.join(value, "bin")
            if os.path.isdir(candidate) and candidate not in rocblas_dirs:
                rocblas_dirs.append(candidate)
    for fallback in (r"C:\Program Files\AMD\ROCm\5.7\bin", r"C:\Program Files\AMD\ROCm\6.0\bin"):
        if os.path.isdir(fallback) and fallback not in rocblas_dirs:
            rocblas_dirs.append(fallback)

    if not rocblas_dirs:
        _detail("rocblas.dll не найдена ни среди модулей, ни в ROCm - HIP SDK не виден")
        return False

    found_any = False
    for base in rocblas_dirs:
        _info("--- rocblas.dll в %s" % base)
        if not os.path.exists(os.path.join(base, "rocblas.dll")):
            _info("    самой rocblas.dll здесь нет")
        for relative in TENSILE_CANDIDATES:
            directory = os.path.normpath(os.path.join(base, relative))
            exists = os.path.isdir(directory)
            _info("    %-24s %s" % (relative, "есть" if exists else "нет"))
            if not exists:
                continue
            try:
                present = sorted(
                    name for name in os.listdir(directory) if name.lower().startswith("tensilelibrary")
                )
            except OSError as exc:
                _detail("    не прочитать %s: %s" % (directory, exc))
                continue
            found_any = True
            for template in TENSILE_FILENAMES:
                name = template % arch
                _info(
                    "    %-38s %s" % (name, "ЕСТЬ" if name in present else "нет")
                )
            archs = sorted({n.split("_")[-1][:-4] for n in present if n.count("_") == 1})
            if archs:
                _info("    архивы под архитектуры: %s" % ", ".join(archs))
            else:
                _info("    TensileLibrary*.dat в каталоге нет вовсе")
            if not any(arch in name for name in present):
                _detail(
                    "в %s нет ни одного файла для %s" % (directory, arch)
                )
    # Отдельно ищем TensileLibrary по всей установке ROCm: если библиотека
    # есть, но не там, куда смотрит rocBLAS, её можно подсказать через
    # ROCBLAS_TENSILE_LIBPATH. Без этого проверка только констатирует отказ.
    hits = _find_tensile_libraries(arch)
    if hits:
        _info("найденные TensileLibrary*%s*:" % arch)
        for path in hits:
            _info("    %s" % path)
        if not override:
            _info(
                "их можно подсказать rocBLAS: set ROCBLAS_TENSILE_LIBPATH=<каталог>"
            )
    else:
        _detail("ни одной TensileLibrary*%s* в установке ROCm не найдено" % arch)

    if not found_any:
        _detail(
            "ни один из каталогов, где rocBLAS ищет библиотеку, не существует. "
            "Поэтому Tensile не инициализируется и matmul падает"
        )
        return False
    return True


def _find_tensile_libraries(arch: str, limit: int = 20) -> List[str]:
    """Ищет файлы TensileLibrary для нужной архитектуры в установке ROCm."""
    roots = []
    for var in ("HIP_PATH", "ROCM_PATH"):
        value = os.environ.get(var, "")
        if value and os.path.isdir(value):
            roots.append(value)
    for base in (r"C:\Program Files\AMD\ROCm", r"C:\Program Files\AMD\ROCm\5.7"):
        if os.path.isdir(base) and base not in roots:
            roots.append(base)

    wanted = ("tensilelibrary_%s" % arch, "tensilelibrary_lazy_%s" % arch, "tensilelibrary.dat")
    hits: List[str] = []
    for root in roots:
        for current, directories, files in os.walk(root):
            # не спускаться вглубь бесконечно: библиотека лежит неглубоко
            depth = current[len(root) :].count(os.sep)
            if depth >= 4:
                directories[:] = []
                continue
            for name in files:
                lowered = name.lower()
                if lowered.startswith("tensilelibrary") and any(w in lowered for w in wanted):
                    hits.append(os.path.join(current, name))
                    if len(hits) >= limit:
                        return hits
    return hits


def check_libs(apply_patch: bool) -> bool:
    """Чьи на самом деле DLL лежат в torch/lib: NVIDIA или ZLUDA.

    install_comfyui.bat подменяет cublas/cusparse/nvrtc файлами из архива
    ZLUDA. У NVIDIA cublas64_11.dll весит сотни мегабайт, у ZLUDA - доли
    мегабайта, поэтому размер отвечает на вопрос однозначно.
    """
    if os.name != "nt":
        _detail("актуально только для Windows")
        return False
    torch = _torch()
    lib_dir = os.path.join(os.path.dirname(os.path.abspath(torch.__file__)), "lib")
    zluda_dir = os.environ.get("ZLRX_ROOT", "")
    interesting = (
        "cublas64_11.dll",
        "cublasLt64_11.dll",
        "cusparse64_11.dll",
        "cufft64_10.dll",
        "nvrtc64_112_0.dll",
        "nvrtc_cuda.dll",
        "cudart64_110.dll",
    )
    replaced = 0
    for name in interesting:
        path = os.path.join(lib_dir, name)
        if not os.path.exists(path):
            _info("%-22s отсутствует" % name)
            continue
        size = os.path.getsize(path)
        # ZLUDA-заглушки на порядок меньше настоящих библиотек NVIDIA.
        # Исключение - cudart64_110.dll: у NVIDIA он и так весит доли мегабайта,
        # и ZLUDA его не подменяет, поэтому по размеру о нём судить нельзя.
        swappable = name in ("cublas64_11.dll", "cusparse64_11.dll", "nvrtc64_112_0.dll")
        if name == "cudart64_110.dll":
            origin = "NVIDIA (ZLUDA его не подменяет)"
        elif size < 32 * 1024 * 1024:
            origin = "ZLUDA?" if swappable else "маленькая"
            if swappable:
                replaced += 1
        else:
            origin = "NVIDIA"
        _info("%-22s %10.2f MiB  %s" % (name, size / 1024 ** 2, origin))
    _info("подменено на ZLUDA: %d из 3 (cublas, cusparse, nvrtc)" % replaced)
    if replaced == 0:
        _detail(
            "в torch/lib лежат оригинальные библиотеки NVIDIA - подмена не выполнена. "
            "Запустите install_comfyui.bat: он напечатает, что заменил, а что пропустил"
        )
    if zluda_dir:
        _info("ZLRX_ROOT = %s" % zluda_dir)
    return True


def check_imports(apply_patch: bool) -> bool:
    """Откуда torch берёт функции pinned-памяти.

    Прошлый прогон показал 0 слотов IAT в 38 модулях. Это либо правда, либо
    сбой разбора PE. Проверка печатает по каждому интересному модулю таблицу
    импорта и ищет имена в сырых байтах файла: имя без записи в IAT означает
    GetProcAddress, и тогда подмена IAT невозможна в принципе.
    """
    if os.name != "nt":
        _detail("актуально только для Windows")
        return False
    _torch()  # модули должны быть загружены

    from zluda_rx580.pinpatch import import_summary, loaded_modules

    wanted = ("torch_cuda.dll", "c10_cuda.dll", "cudart64_110.dll", "torch_cpu.dll")
    paths = {os.path.basename(path).lower(): path for _base, path in loaded_modules() if path}
    missing = [name for name in wanted if name not in paths]
    if missing:
        _detail("в процессе нет: %s" % ", ".join(missing))

    iat_total = 0
    for name in wanted:
        path = paths.get(name)
        if not path:
            continue
        summary = import_summary(path)
        _info("--- %s" % name)
        _info("    импортирует DLL: %s" % (", ".join(summary.import_dlls) or "<нет>"))
        if summary.delay_dlls:
            _info("    delay-load: %s" % ", ".join(summary.delay_dlls))
        _info("    записей импорта: %d" % summary.descriptor_count)
        _info("    слоты pinned-памяти: %s" % (", ".join(summary.iat_matches) or "<нет>"))
        _info("    имена в сырых байтах: %s" % (", ".join(summary.raw_name_hits) or "<нет>"))
        _info("    вывод: %s" % summary.verdict())
        iat_total += len(summary.iat_matches)

    if iat_total:
        _info("итого слотов для подмены: %d" % iat_total)
        return True
    _detail(
        "слотов IAT нет. Если при этом имена в байтах есть - PyTorch получает их "
        "через GetProcAddress, и уровень 3 (подмена IAT) на этой машине не работает"
    )
    return False


def check_patch(apply_patch: bool) -> bool:
    if os.name != "nt":
        _detail("cudaHostAlloc patch is Windows-only")
        return False
    # Порядок обязателен: torch_cuda.dll появляется в процессе только после
    # import torch, до этого сканировать нечего (так и было в первом прогоне
    # на живой машине - "nothing to patch").
    torch = _torch()

    import zluda_rx580.pinpatch as pinpatch

    report = pinpatch.survey()
    for line in report.format().splitlines():
        _info(line)
    for note in report.notes:
        _detail(note)
    if not report.scanned:
        _detail("в процессе нет модулей torch - опрос не удался")
        return False
    if not report.slots:
        _detail(
            "ни один из %d модулей не импортирует %s через IAT"
            % (len(report.scanned), "/".join(pinpatch.TARGET_FUNCTIONS))
        )
        return False

    try:
        patch = pinpatch.install()
    except pinpatch.PatchError as exc:
        _detail(str(exc))
        return False
    _info("patch applied: %d slot(s)" % len(patch.report.slots))

    _info("is_pinned(torch.empty(8).pin_memory()) = %s" % torch.empty(8).pin_memory().is_pinned())
    value = torch.arange(1, 17, device="cuda", dtype=torch.float32).sum().item()
    _info("sum().item() after patch = %s (expected 136.0)" % value)
    if abs(value - 136.0) > 1e-3:
        _detail("wrong value through patched pinned memory")
        return False
    latent = torch.randn(1, 4, 16, 16, device="cuda", dtype=torch.float16)
    _info("count_nonzero(...) > 0 = %s" % bool(torch.count_nonzero(latent) > 0))
    torch.cuda.empty_cache()
    _info("empty_cache() after patched allocations: ok")
    patch.undo()
    _info("patch reverted")
    return True


def check_shim(apply_patch: bool) -> bool:
    """Проверяет Python-заплатки: значения должны совпадать, pinned не выделяется."""
    from zluda_rx580.runtime import apply_pinned_shims, pinned_shims_active

    torch = _torch()
    baseline_sum = torch.arange(1, 1025, device="cuda", dtype=torch.float32).sum().cpu().item()
    baseline_count = int(torch.count_nonzero(torch.arange(0, 64, device="cuda") > 7).cpu().item())

    already = pinned_shims_active(torch)
    applied = apply_pinned_shims(torch)
    for name in applied:
        _info("shim: %s" % name)
    if applied:
        _info("заплатки установлены сейчас: %d" % len(applied))
    elif already:
        # Автоактивация через .pth ставит их при старте интерпретатора,
        # поэтому повторный вызов ничего не возвращает. Это не отказ.
        _info("заплатки уже активны - их поставила автоактивация при старте")
    else:
        _detail("заплатки не установлены и не были установлены раньше")
        return False

    gpu_sum = torch.arange(1, 1025, device="cuda", dtype=torch.float32).sum()
    item_value = gpu_sum.item()
    _info("sum via patched .item() = %.9g (expected %.9g)" % (item_value, baseline_sum))
    if item_value != baseline_sum:
        _detail("patched .item() returned a wrong value")
        return False

    truth = torch.arange(0, 64, device="cuda") > 7
    count = int(torch.count_nonzero(truth).item())
    _info("count_nonzero via patch = %d (expected %d)" % (count, baseline_count))
    if count != baseline_count:
        _detail("patched count_nonzero returned a wrong value")
        return False

    flag_tensor = torch.count_nonzero(torch.arange(0, 64, device="cuda") > 7)
    _info("bool(flag_tensor) = %s" % bool(flag_tensor))
    _info("int(flag_tensor) = %d  float(flag_tensor) = %.1f" % (int(flag_tensor), float(flag_tensor)))
    empty = torch.zeros(3, device="cuda")
    _info("bool(count_nonzero(zeros)) = %s" % bool(torch.count_nonzero(empty)))
    return True


CHECK_FUNCTIONS: Dict[str, Callable[[bool], bool]] = {
    "env": check_env,
    "device": check_device,
    "matmul": check_matmul,
    "item": check_item,
    "pin": check_pin,
    "nonzero": check_nonzero,
    "roundtrip": check_roundtrip,
    "shim": check_shim,
    "patch": check_patch,
    "libs": check_libs,
    "imports": check_imports,
    "blas": check_blas,
}


# ---------------------------------------------------------------------------
# дочерний процесс
# ---------------------------------------------------------------------------


def _run_child(name: str, apply_patch: bool) -> int:
    # Окружение настраивается до импорта torch.
    try:
        from zluda_rx580.runtime import setup_env

        setup_env()
    except Exception as exc:
        _detail("setup_env failed: %r" % (exc,))
    if apply_patch and name != "patch":
        try:
            from zluda_rx580.pinpatch import install

            install()
            _info("cudaHostAlloc patch installed")
        except Exception as exc:
            _detail("patch failed: %s" % exc)
    function = CHECK_FUNCTIONS[name]
    try:
        ok = bool(function(apply_patch))
    except Exception as exc:
        import traceback

        _detail("%s: %s" % (type(exc).__name__, exc))
        _detail(traceback.format_exc(limit=6))
        ok = False
    _result(name, "PASS" if ok else "FAIL")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# родительский процесс
# ---------------------------------------------------------------------------


def _run_one(name: str, apply_patch: bool, timeout: int) -> Tuple[str, List[str], List[str]]:
    command = [sys.executable, "-m", "zluda_rx580.diagnose", "--child", name]
    if apply_patch:
        command.append("--patch")
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout,
            encoding="utf-8",
            errors="replace",
        )
    except subprocess.TimeoutExpired:
        return "TIMEOUT", [], [
            "process did not finish in %d s" % timeout,
            "первый запуск компилирует ядра ZLUDA и кэширует их в ZLUDA_CACHE_DIR; "
            "повторите проверку - со второй попытки обычно быстрее, "
            "или поднимите лимит: run_diagnose.bat --timeout 3600",
        ]
    info: List[str] = []
    details: List[str] = []
    status = "CRASH"
    for line in (completed.stdout or "").splitlines():
        if line.startswith(MARK + " INFO "):
            info.append(line[len(MARK) + 6 :])
        elif line.startswith(MARK + " DETAIL "):
            details.append(line[len(MARK) + 8 :])
        elif line.startswith(MARK + " RESULT "):
            status = line.rsplit(" ", 1)[-1]
    if status == "CRASH":
        tail = [line for line in (completed.stderr or "").splitlines() if line.strip()][-6:]
        details.extend(tail)
        details.append("exit code %s" % completed.returncode)
    return status, info, details


def main(argv: Optional[List[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)

    if "--child" in argv:
        index = argv.index("--child")
        return _run_child(argv[index + 1], "--patch" in argv)

    if "--list" in argv:
        for name, description in CHECKS:
            print("%-10s %s" % (name, description))
        return 0

    names = CHECK_NAMES
    if "--check" in argv:
        index = argv.index("--check")
        names = [argv[index + 1]]
    apply_patch = "--patch" in argv
    # Первый запуск ZLUDA компилирует PTX в AMDGCN, это минуты на ядро.
    # 120 с на живой машине не хватило: item/nonzero/roundtrip/shim отвалились
    # по TIMEOUT, хотя GPU к тому моменту уже работал.
    timeout = int(os.environ.get("ZLRX580_TIMEOUT", "1800"))
    if "--timeout" in argv:
        timeout = int(argv[argv.index("--timeout") + 1])
    print("таймаут на проверку: %d с (первый запуск компилирует ядра ZLUDA)" % timeout)

    print("=" * 72)
    print("zluda_rx580 diagnostics%s" % ("  (with cudaHostAlloc patch)" if apply_patch else ""))
    print("=" * 72)

    failed = []
    for name in names:
        description = dict(CHECKS)[name]
        print("\n[%s] %s" % (name, description))
        status, info, details = _run_one(name, apply_patch, timeout)
        for line in info:
            print("    %s" % line)
        for line in details:
            print("  ! %s" % line)
        print("  -> %s" % status)
        if status != "PASS":
            failed.append((name, status))

    print("\n" + "=" * 72)
    if failed:
        print("FAILED: %s" % ", ".join("%s (%s)" % item for item in failed))
    else:
        print("All checks passed.")
    print("=" * 72)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
