"""Установка и удаление автоматической активации через файл ``.pth``.

``.pth``-файл в ``site-packages`` выполняется интерпретатором при старте,
поэтому это единственный способ настроить окружение **до** ``import torch``
без правки чужих скриптов.

Запускается так (из нужного окружения)::

    python -m zluda_rx580.install_pth            # установить
    python -m zluda_rx580.install_pth --remove   # удалить
    python -m zluda_rx580.install_pth --show     # показать состояние

Файл содержит одну строку, которая добавляет каталог набора в ``sys.path``
и импортирует :mod:`zluda_rx580.activate`. Пакет при этом не копируется —
источник остаётся один.
"""

from __future__ import annotations

import os
import site
import sys
from typing import List, Optional

PTH_NAME = "zluda_rx580.pth"


def kit_root() -> str:
    """Каталог, в котором лежит сам пакет ``zluda_rx580``."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def site_dirs() -> List[str]:
    dirs: List[str] = []
    for function in ("getsitepackages", "getusersitepackages"):
        getter = getattr(site, function, None)
        if getter is None:
            continue
        try:
            value = getter()
        except Exception:
            continue
        if isinstance(value, str):
            dirs.append(value)
        else:
            dirs.extend(value)
    seen = set()
    result = []
    for directory in dirs:
        if directory and directory not in seen:
            seen.add(directory)
            result.append(directory)
    return result


def pth_path(target_dir: Optional[str] = None) -> Optional[str]:
    if target_dir is None:
        for directory in site_dirs():
            if os.path.isdir(directory):
                target_dir = directory
                break
    if target_dir is None:
        return None
    return os.path.join(target_dir, PTH_NAME)


def pth_line() -> str:
    # Прямые слеши Windows принимает, а в строке .pth они не требуют экранирования.
    root = kit_root().replace("\\", "/")
    if "'" in root:
        raise RuntimeError("kit path contains a quote: %s" % root)
    return "import sys; sys.path.insert(0, '%s'); import zluda_rx580.activate" % root


def install(target_dir: Optional[str] = None, quiet: bool = False) -> str:
    path = pth_path(target_dir)
    if path is None:
        raise RuntimeError("no writable site-packages directory found")
    directory = os.path.dirname(path)
    if not os.path.isdir(directory):
        os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(pth_line() + "\n")
    if not quiet:
        print("zluda_rx580: wrote %s" % path)
        print("zluda_rx580: kit root is %s" % kit_root())
    return path


def remove(target_dir: Optional[str] = None, quiet: bool = False) -> bool:
    removed = False
    for directory in ([target_dir] if target_dir else site_dirs()):
        path = os.path.join(directory, PTH_NAME)
        if os.path.exists(path):
            os.remove(path)
            removed = True
            if not quiet:
                print("zluda_rx580: removed %s" % path)
    if not removed and not quiet:
        print("zluda_rx580: nothing to remove")
    return removed


def status(target_dir: Optional[str] = None) -> str:
    path = pth_path(target_dir)
    if path is None or not os.path.exists(path):
        return "not installed"
    with open(path, "r", encoding="utf-8") as handle:
        return "installed at %s\n  %s" % (path, handle.read().strip())


def main(argv: Optional[List[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    target = None
    if "--dir" in argv:
        index = argv.index("--dir")
        target = argv[index + 1]
    if "--remove" in argv:
        remove(target)
        return 0
    if "--show" in argv:
        print(status(target))
        return 0
    install(target)
    print(status(target))
    print("zluda_rx580: set ZLUDA_RX580_DISABLE=1 to switch it off without uninstalling")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
