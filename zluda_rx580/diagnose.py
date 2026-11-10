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
        from zluda_rx580.runtime import looks_like_zluda_installation

        _info("ZLUDA installation detected: %s" % looks_like_zluda_installation(torch))
    except Exception as exc:
        _detail("torch is not importable: %r" % (exc,))
        return False

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
    torch = _torch()
    a = torch.randn(256, 256, device="cuda", dtype=torch.float16)
    b = torch.randn(256, 256, device="cuda", dtype=torch.float16)
    c = (a @ b).float()
    torch.cuda.synchronize()
    reference = (a.float().cpu() @ b.float().cpu())
    error = (c.cpu() - reference).abs().max().item()
    _info("max |GPU - CPU| = %.6g" % error)
    return error < 1.0


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


def check_patch(apply_patch: bool) -> bool:
    if os.name != "nt":
        _detail("cudaHostAlloc patch is Windows-only")
        return False
    from zluda_rx580.pinpatch import PatchError, install

    try:
        patch = install()
    except PatchError as exc:
        _detail(str(exc))
        return False
    for line in patch.report.format().splitlines():
        _info(line)

    torch = _torch()
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
    from zluda_rx580.runtime import apply_pinned_shims

    torch = _torch()
    baseline_sum = torch.arange(1, 1025, device="cuda", dtype=torch.float32).sum().cpu().item()
    baseline_count = int(torch.count_nonzero(torch.arange(0, 64, device="cuda") > 7).cpu().item())

    applied = apply_pinned_shims(torch)
    for name in applied:
        _info("shim: %s" % name)
    if not applied:
        _detail("no shims were applied")
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
        return "TIMEOUT", [], ["process did not finish in %d s" % timeout]
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
    timeout = 120
    if "--timeout" in argv:
        timeout = int(argv[argv.index("--timeout") + 1])

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
