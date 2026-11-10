"""Подключение набора к процессу Python.

Импорт этого модуля:

1. настраивает переменные окружения **до** загрузки PyTorch;
2. ставит хук, который выполнит :func:`zluda_rx580.runtime.on_torch_imported`
   сразу после того, как ``import torch`` завершится.

Два способа подключить:

* файл ``.pth`` в ``site-packages`` (ставится :mod:`zluda_rx580.install_pth`) —
  работает для всех скриптов в окружении;
* строка ``import zluda_rx580.activate`` в самом начале ``main.py`` — работает
  точечно и видна в коде.

Модуль никогда не бросает исключение наружу: если что-то пошло не так,
приложение должно продолжать работать как ни в чём не бывало.
"""

from __future__ import annotations

import logging
import sys

LOG = logging.getLogger("zluda_rx580")

_INSTALLED = False


class _TorchLoaderProxy:
    """Прокси загрузчика: выполняет модуль, затем вызывает наши настройки."""

    def __init__(self, loader):
        self._loader = loader

    def __getattr__(self, item):
        return getattr(self._loader, item)

    def __repr__(self):  # pragma: no cover - diagnostics only
        return "_TorchLoaderProxy(%r)" % (self._loader,)

    def create_module(self, spec):
        create = getattr(self._loader, "create_module", None)
        if create is None:
            return None
        return create(spec)

    def exec_module(self, module):
        self._loader.exec_module(module)
        try:
            from zluda_rx580.runtime import on_torch_imported

            on_torch_imported(module)
        except Exception as exc:  # pragma: no cover - defensive
            LOG.debug("zluda_rx580: post-import hook failed: %r", exc)


class _TorchFinder:
    """``sys.meta_path`` finder, перехватывающий только модуль ``torch``."""

    def __init__(self):
        self._attempted = False

    def find_spec(self, fullname, path=None, target=None):
        if fullname != "torch" or self._attempted:
            return None
        self._attempted = True
        for finder in list(sys.meta_path):
            if finder is self:
                continue
            find = getattr(finder, "find_spec", None)
            if find is None:
                continue
            try:
                spec = find(fullname, path, target)
            except Exception:
                continue
            if spec is not None and spec.loader is not None:
                spec.loader = _TorchLoaderProxy(spec.loader)
                return spec
        return None


def install_import_hook() -> bool:
    """Ставит finder в ``sys.meta_path``. ``False``, если torch уже импортирован."""
    global _INSTALLED
    if _INSTALLED:
        return True
    if "torch" in sys.modules:
        # Поздно: применяем настройки немедленно.
        try:
            from zluda_rx580.runtime import on_torch_imported

            on_torch_imported(sys.modules["torch"])
        except Exception as exc:  # pragma: no cover - defensive
            LOG.debug("zluda_rx580: immediate setup failed: %r", exc)
        _INSTALLED = True
        return False
    sys.meta_path.insert(0, _TorchFinder())
    _INSTALLED = True
    return True


def main() -> None:
    try:
        from zluda_rx580.runtime import is_disabled, setup_env

        if is_disabled():
            return
        for name, value in setup_env().items():
            LOG.debug("zluda_rx580: %s=%s", name, value)
        install_import_hook()
    except Exception as exc:  # pragma: no cover - must never break the host app
        try:
            sys.stderr.write("zluda_rx580: activation failed: %r\n" % (exc,))
        except Exception:
            pass


main()
