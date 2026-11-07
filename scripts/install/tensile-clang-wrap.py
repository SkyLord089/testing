#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Обёртка вокруг компилятора для сборки ядер Tensile.

ЗАЧЕМ ЭТО НУЖНО

  Tensile запускает clang++ напрямую через subprocess. Если компиляция падает,
  он печатает текст ошибки, но делает это внутри дочернего процесса joblib -
  и вывод теряется. Снаружи видно только:

      subprocess.CalledProcessError: Command '[...]' returned non-zero exit status 1

  То есть НЕ ВИДНО, на что именно ругается clang. Эта обёртка перехватывает
  вызов, сохраняет полный stderr в лог и при необходимости подмешивает
  дополнительные флаги.

КАК ЭТО ПОДКЛЮЧАЕТСЯ

  Tensile читает переменную окружения Tensile_CXX_COMPILER_LAUNCHER и
  ставит её содержимое ПЕРЕД именем компилятора:

      <launcher> <clang++.exe> <аргументы...>

  Значит достаточно положить сюда python.exe и этот файл.

ВАЖНО ПРО ПУТИ

  Tensile разбирает переменную через shlex.split() в режиме posix, а в этом
  режиме обратный слэш - экранирующий символ. Путь вида C:\\rb\\wrap.py
  превратился бы в C:rbwrap.py. Поэтому в переменной окружения пути
  пишутся ТОЛЬКО через прямой слэш. Windows такое понимает.

ПЕРЕМЕННЫЕ ОКРУЖЕНИЯ

  TENSILE_WRAP_LOG     файл, куда писать отчёт (обязательно для лога)
  TENSILE_WRAP_EXTRA   дополнительные флаги через пробел, подмешиваются
                       перед аргументами компилятора

КОД ВОЗВРАТА

  Всегда код компилятора - обёртка ничего не скрывает и не подменяет.
"""

import os
import sys
import subprocess


def main():
    argv = sys.argv[1:]
    if not argv:
        sys.stderr.write("tensile-clang-wrap: нет аргументов\n")
        return 2

    compiler = argv[0]
    cargs = argv[1:]

    extra_raw = os.environ.get("TENSILE_WRAP_EXTRA", "").strip()
    extra = extra_raw.split() if extra_raw else []

    log_path = os.environ.get("TENSILE_WRAP_LOG", "")

    # Дополнительные флаги ставим сразу после имени компилятора.
    cmd = [compiler] + extra + cargs

    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as e:
        if log_path:
            with open(log_path, "a", encoding="utf-8", errors="replace") as f:
                f.write("\n=== НЕ УДАЛОСЬ ЗАПУСТИТЬ КОМПИЛЯТОР ===\n")
                f.write(" ".join(cmd) + "\n")
                f.write("%s\n" % e)
        sys.stderr.write("tensile-clang-wrap: %s\n" % e)
        return 1

    if proc.stdout:
        sys.stdout.write(proc.stdout.decode("utf-8", "replace"))

    if proc.returncode != 0 and log_path:
        # Пишем только падения: полный лог успешных сборок не нужен и весит много.
        try:
            with open(log_path, "a", encoding="utf-8", errors="replace") as f:
                f.write("\n" + "=" * 70 + "\n")
                f.write("КОД %d\n" % proc.returncode)
                f.write("ФЛАГИ ОБЁРТКИ: %s\n" % (" ".join(extra) if extra else "(нет)"))
                f.write("КОМАНДА:\n%s\n" % " ".join(cmd))
                f.write("-" * 70 + "\nSTDERR:\n")
                f.write(proc.stderr.decode("utf-8", "replace"))
                f.write("\n")
        except OSError:
            pass

    if proc.stderr:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))

    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
