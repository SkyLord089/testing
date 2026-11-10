"""Набор заплат для ComfyUI + ZLUDA на AMD Polaris (RX 480/570/580, gfx803).

Импорт пакета **безопасен и ничего не делает**: ни CUDA, ни torch здесь не
инициализируются. Вся работа выполняется явно:

* :func:`zluda_rx580.runtime.setup_env` — до ``import torch``;
* :func:`zluda_rx580.runtime.on_torch_imported` — сразу после;
* :mod:`zluda_rx580.diagnose` — диагностика из командной строки.

Автоматическое подключение — через :mod:`zluda_rx580.autostart` (файл ``.pth``
в ``site-packages``). Отключается переменной ``ZLUDA_RX580_DISABLE=1``.
"""

__version__ = "0.1.0"

__all__ = ["__version__", "runtime", "pe", "thunks"]


def __getattr__(name):
    # Ленивая загрузка подмодулей: ``import zluda_rx580`` не должен тянуть ctypes.
    if name in ("runtime", "pe", "thunks"):
        import importlib

        return importlib.import_module("zluda_rx580." + name)
    raise AttributeError("module %r has no attribute %r" % (__name__, name))
