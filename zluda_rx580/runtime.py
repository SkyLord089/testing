"""Настройка окружения и PyTorch для ZLUDA на HIP SDK 5.7 (gfx803).

Две фазы, их нельзя путать:

* :func:`setup_env` — **до** ``import torch``. Меняет только переменные
  окружения, потому что часть решений PyTorch принимает один раз при загрузке
  бинарников (например, ``DISABLE_ADDMM_CUDA_LT`` читается через
  ``std::getenv`` в статической инициализации).
* :func:`on_torch_imported` — **сразу после** ``import torch``, до первой
  работы с CUDA. Ставит патч ``cudaHostAlloc`` и применяет откаты неподдерживаемых
  операций.

Всё идемпотентно: повторный вызов ничего не ломает.
"""

from __future__ import annotations

import logging
import os
from typing import Any, Dict, List, Optional

LOG = logging.getLogger("zluda_rx580")

ENV_DISABLE_KEY = "ZLUDA_RX580_DISABLE"
ENV_FORCE_KEY = "ZLUDA_RX580_FORCE"
ENV_HOST_REGISTER_KEY = "ZLUDA_RX580_HOST_REGISTER"
ENV_HIDE_ROCM_KEY = "ZLUDA_RX580_HIDE_ROCM"
ENV_NO_PIN_PATCH_KEY = "ZLUDA_RX580_NO_PIN_PATCH"
ENV_NO_PIN_SHIM_KEY = "ZLUDA_RX580_NO_PIN_SHIM"


def is_disabled() -> bool:
    return os.environ.get(ENV_DISABLE_KEY, "").strip() not in ("", "0", "false", "False")


def _flag(name: str) -> bool:
    return os.environ.get(name, "").strip() not in ("", "0", "false", "False")


def hide_rocm_environment() -> List[str]:
    """Убирает из окружения следы ROCm/HIP (как это делает форк ComfyUI-Zluda).

    По умолчанию **выключено** (``ZLUDA_RX580_HIDE_ROCM=1`` включает): у cu118-сборки
    PyTorch ``torch.version.hip`` и так равен ``None``, а вырезание каталогов ROCm из
    ``PATH`` может мешать другим инструментам в том же окружении.

    Самому ZLUDA это не вредит: ``amdhip64.dll`` и ``rocblas.dll`` — статические
    зависимости ``zluda.dll`` и разрешаются загрузчиком Windows в момент создания
    процесса, ещё до запуска Python.
    """
    removed: List[str] = []
    for name in ("ROCM_HOME", "HIP_HOME", "ROCM_VERSION"):
        if name in os.environ:
            os.environ.pop(name)
            removed.append(name)
    path = os.environ.get("PATH", "")
    if path:
        kept = [entry for entry in path.split(";") if "rocm" not in entry.lower()]
        if len(kept) != len(path.split(";")):
            os.environ["PATH"] = ";".join(kept)
            removed.append("PATH(rocm entries)")
    return removed


def setup_env() -> Dict[str, str]:
    """Вызывается до ``import torch``. Возвращает словарь внесённых изменений."""
    changes: Dict[str, str] = {}
    if is_disabled():
        return changes

    if _flag(ENV_HIDE_ROCM_KEY):
        for name in hide_rocm_environment():
            changes[name] = "<removed>"

    # cublasLt-путь в addmm на Windows включён начиная с PyTorch 2.4
    # (в aten/src/ATen/native/cuda/Blas.cpp исключение для MSVC снято),
    # а hipBLASLt в HIP SDK 5.7 его не поддерживает.
    # Blas.cpp:247-253 читает переменную через getenv.
    changes["DISABLE_ADDMM_CUDA_LT"] = os.environ.setdefault("DISABLE_ADDMM_CUDA_LT", "1")

    # Экспериментальный обход: вместо cudaHostAlloc использовать
    # malloc + cudaHostRegister (CachingHostAllocator.cpp:203-252).
    # На gfx803 это может упасть в registerPages() — см. docs/ROOT_CAUSE.md.
    if _flag(ENV_HOST_REGISTER_KEY):
        conf = os.environ.get("PYTORCH_CUDA_ALLOC_CONF", "")
        option = "pinned_use_cuda_host_register:True"
        if option not in conf:
            os.environ["PYTORCH_CUDA_ALLOC_CONF"] = (conf + "," + option) if conf else option
        changes["PYTORCH_CUDA_ALLOC_CONF"] = os.environ["PYTORCH_CUDA_ALLOC_CONF"]

    return changes


def looks_like_zluda_installation(torch_module: Any) -> bool:
    """Признаки ZLUDA, не требующие инициализации CUDA."""
    if _flag(ENV_FORCE_KEY):
        return True
    try:
        lib_dir = os.path.join(os.path.dirname(os.path.abspath(torch_module.__file__)), "lib")
        # patchzluda*.bat копируют оригинальный nvrtc в nvrtc_cuda.dll
        if os.path.exists(os.path.join(lib_dir, "nvrtc_cuda.dll")):
            return True
    except Exception:
        pass
    try:
        import zluda_rx580.pinpatch as pinpatch

        for _base, path in pinpatch.loaded_modules():
            name = os.path.basename(path).lower()
            if name.startswith("zluda"):
                return True
    except Exception:
        pass
    return False


def device_name(torch_module: Any) -> str:
    try:
        if torch_module.cuda.is_available():
            return torch_module.cuda.get_device_name(0)
    except Exception:
        pass
    return ""


def is_zluda_device(torch_module: Any) -> bool:
    if _flag(ENV_FORCE_KEY):
        return True
    return device_name(torch_module).endswith("[ZLUDA]")


def install_pinned_memory_patch(torch_module: Any) -> Optional[str]:
    """Ставит патч ``cudaHostAlloc``. Возвращает текст отчёта или ``None``."""
    if is_disabled() or _flag(ENV_NO_PIN_PATCH_KEY):
        return None
    try:
        from .pinpatch import PatchError, install
    except Exception as exc:  # pragma: no cover - Windows-only
        return "pinpatch module unavailable: %r" % (exc,)
    try:
        patch = install()
    except PatchError as exc:
        return "cudaHostAlloc patch NOT applied: %s" % (exc,)
    except Exception as exc:  # pragma: no cover - defensive
        return "cudaHostAlloc patch NOT applied: %r" % (exc,)
    _installed.append(patch)
    return patch.report.format()


_installed: List[Any] = []


def apply_pinned_shims(torch_module: Any) -> List[str]:
    """Убирает внутренние выделения pinned-памяти из частых операций.

    Это заплатка уровня Python: она не трогает бинарники и поэтому безопасна
    на любой машине. Цена — одна лишняя копия одного элемента с GPU на CPU
    (ровно то, что PyTorch 2.2.1 делал в ``.item()`` всегда).

    Что закрывает:

    * ``Tensor.item`` / ``__bool__`` / ``__float__`` / ``__int__`` / ``__index__``
      — в ``_local_scalar_dense_cuda`` начиная с 2.3.0 выделяется pinned-тензор;
    * ``torch.count_nonzero`` — в ``Nonzero.cu`` выделяется pinned-буфер,
      поэтому переписываем как ``(x != 0).sum()``, который остаётся на GPU.

    Что НЕ закрывает: вызовы этих операций изнутри C++ (например, из
    пользовательских расширений). Для них нужен патч ``cudaHostAlloc``.
    """
    done: List[str] = []
    if is_disabled() or _flag(ENV_NO_PIN_SHIM_KEY):
        return done
    if getattr(torch_module, "_zluda_rx580_pin_shims", False):
        return done

    tensor_type = getattr(torch_module, "Tensor", None)
    if tensor_type is None:
        LOG.debug("torch.Tensor is not available yet; skipping Tensor shims")
    else:
        done.extend(_patch_tensor_scalars(tensor_type))

    done.extend(_patch_count_nonzero(torch_module))

    if done:
        try:
            torch_module._zluda_rx580_pin_shims = True
        except Exception:
            pass
    return done


def _patch_tensor_scalars(tensor_type: Any) -> List[str]:
    def scalar_via_cpu(original):
        def wrapper(self, *args, **kwargs):
            try:
                if self.is_cuda and self.numel() == 1:
                    return original(self.detach().to("cpu", copy=True), *args, **kwargs)
            except Exception:
                pass
            return original(self, *args, **kwargs)

        wrapper._zluda_rx580_pin_shim = True
        return wrapper

    done: List[str] = []
    for name in ("item", "__bool__", "__float__", "__int__", "__index__"):
        original = getattr(tensor_type, name, None)
        if original is None or getattr(original, "_zluda_rx580_pin_shim", False):
            continue
        try:
            setattr(tensor_type, name, scalar_via_cpu(original))
            done.append("Tensor.%s -> cpu" % name)
        except Exception as exc:
            LOG.debug("Tensor.%s: %s", name, exc)
    return done


def _patch_count_nonzero(torch_module: Any) -> List[str]:
    original_count_nonzero = getattr(torch_module, "count_nonzero", None)
    if original_count_nonzero is None or getattr(
        original_count_nonzero, "_zluda_rx580_pin_shim", False
    ):
        return []

    def count_nonzero(input, dim=None):
        try:
            if getattr(input, "is_cuda", False):
                mask = input != 0
                return mask.sum() if dim is None else mask.sum(dim=dim)
        except Exception:
            pass
        return (
            original_count_nonzero(input)
            if dim is None
            else original_count_nonzero(input, dim)
        )

    count_nonzero._zluda_rx580_pin_shim = True
    try:
        torch_module.count_nonzero = count_nonzero
    except Exception as exc:
        LOG.debug("count_nonzero: %s", exc)
        return []
    return ["count_nonzero -> (x != 0).sum()"]


def apply_torch_tweaks(torch_module: Any) -> List[str]:
    """Откатывает операции, которых нет в ZLUDA 3.9.x на HIP 5.7."""
    done: List[str] = []
    torch = torch_module

    try:
        torch.backends.cudnn.enabled = False
        done.append("cudnn disabled")
    except Exception as exc:
        LOG.debug("cudnn: %s", exc)

    backend = getattr(torch.backends, "cuda", None)
    if backend is not None:
        for name, value in (
            ("enable_flash_sdp", False),
            ("enable_mem_efficient_sdp", False),
            ("enable_math_sdp", True),
        ):
            setter = getattr(backend, name, None)
            if callable(setter):
                try:
                    setter(value)
                    done.append("%s(%s)" % (name, value))
                except Exception as exc:
                    LOG.debug("%s: %s", name, exc)

    # stft/istft требуют cuFFT-путей, которых в ZLUDA для HIP 5.7 нет.
    for name in ("stft", "istft"):
        original = getattr(torch, name, None)
        if original is None or getattr(original, "_zluda_rx580_cpu_fallback", False):
            continue
        try:
            setattr(torch, name, _cpu_fallback(name, original))
            done.append("%s -> cpu" % name)
        except Exception as exc:
            LOG.debug("%s: %s", name, exc)

    original_topk = getattr(torch, "topk", None)
    if original_topk is not None and not getattr(original_topk, "_zluda_rx580_cpu_fallback", False):
        try:
            torch.topk = _cpu_topk(original_topk)
            done.append("topk -> cpu")
        except Exception as exc:
            LOG.debug("topk: %s", exc)

    jit = getattr(torch, "jit", None)
    if jit is not None and not getattr(jit.script, "_zluda_rx580_stub", False):
        def _script_stub(function, *_, **__):
            try:
                function.graph = torch._C.Graph()
            except Exception:
                pass
            return function

        _script_stub._zluda_rx580_stub = True
        jit.script = _script_stub
        done.append("jit.script stubbed")

    dynamo_config = getattr(getattr(torch, "_dynamo", None), "config", None)
    if dynamo_config is not None:
        try:
            dynamo_config.suppress_errors = True
            done.append("dynamo suppress_errors=True")
        except Exception:
            pass

    try:
        import onnxruntime as ort

        original_providers = ort.get_available_providers

        def filtered_providers():
            return [p for p in original_providers() if p != "CUDAExecutionProvider"]

        capi_state = getattr(getattr(ort, "capi", None), "_pybind_state", None)
        if capi_state is not None and not getattr(
            capi_state.get_available_providers, "_zluda_rx580_stub", False
        ):
            filtered_providers._zluda_rx580_stub = True
            capi_state.get_available_providers = filtered_providers
            done.append("onnxruntime CUDA EP hidden")
    except Exception:
        pass

    return done


def _cpu_fallback(name: str, original):
    def wrapper(input, *args, **kwargs):
        window = kwargs.get("window")
        if window is None and args:
            window = args[0]
            args = args[1:]
        kwargs = dict(kwargs)
        kwargs.pop("window", None)
        if window is None:
            result = original(input.cpu(), *args, **kwargs)
        else:
            result = original(input.cpu(), window.cpu(), *args, **kwargs)
        return result.to(input.device) if hasattr(result, "to") else result

    wrapper._zluda_rx580_cpu_fallback = True
    wrapper.__name__ = name
    return wrapper


def _cpu_topk(original):
    def wrapper(input, *args, **kwargs):
        device = input.device
        values, indices = original(input.cpu(), *args, **kwargs)
        import torch as _torch

        return _torch.return_types.topk((values.to(device), indices.to(device)))

    wrapper._zluda_rx580_cpu_fallback = True
    wrapper.__name__ = "topk"
    return wrapper


def on_torch_imported(torch_module: Any = None) -> None:
    """Точка входа после ``import torch``. Никогда не бросает исключение."""
    if is_disabled():
        return
    if getattr(torch_module, "_zluda_rx580_configured", False):
        return
    try:
        import torch as _torch
    except Exception:  # pragma: no cover
        return
    if torch_module is None:
        torch_module = _torch
    try:
        zluda = looks_like_zluda_installation(torch_module)
    except Exception as exc:  # pragma: no cover - must never break the host app
        LOG.warning("zluda_rx580: ZLUDA detection failed: %r", exc)
        return
    if not zluda:
        return

    # Стадии независимы: сбой одной не должен отменять остальные.
    try:
        report = install_pinned_memory_patch(torch_module)
        if report:
            for line in report.splitlines():
                LOG.info("zluda_rx580: %s", line)
    except Exception as exc:  # pragma: no cover
        LOG.warning("zluda_rx580: cudaHostAlloc patch failed: %r", exc)

    try:
        shims = apply_pinned_shims(torch_module)
        if shims:
            LOG.info("zluda_rx580: pinned-memory shims: %s", ", ".join(shims))
    except Exception as exc:  # pragma: no cover
        LOG.warning("zluda_rx580: pinned-memory shims failed: %r", exc)

    try:
        tweaks = apply_torch_tweaks(torch_module)
    except Exception as exc:  # pragma: no cover
        LOG.warning("zluda_rx580: torch tweaks failed: %r", exc)
        tweaks = []

    LOG.info(
        "zluda_rx580: active (torch %s, device %r); %s",
        getattr(torch_module, "__version__", "?"),
        device_name(torch_module) or "<not initialised>",
        ", ".join(tweaks) or "no tweaks applied",
    )
    try:
        torch_module._zluda_rx580_configured = True
    except Exception:
        pass
