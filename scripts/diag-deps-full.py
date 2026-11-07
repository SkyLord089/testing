#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Полный обход зависимостей: какой модуль и какой функции не хватает.

ПОЧЕМУ ЭТО НУЖНО

  Патч R0600 -> R0000 в rocblas.dll применился успешно (1 вхождение),
  но загрузка всё равно дала WinError 127 - "не найдена процедура".

  WinError 127 означает именно ОТСУТСТВИЕ ФУНКЦИИ, а не файла
  (за отсутствующий файл был бы 126). И виноват не обязательно rocblas.dll:
  он на этапе загрузки тянет hipblaslt.dll, а тот тоже собран против
  рантайма SDK 6.4 и имеет свои импорты.

  Проверять надо всю цепочку, а не один файл.

ЧТО ДЕЛАЕТ СКРИПТ

  1. Собирает временную папку: rocblas.dll (пропатченный), hipblaslt.dll,
     и системный amdhip64.dll, переименованный в amdhip64_6.dll.
  2. Читает таблицы импорта КАЖДОГО модуля в папке и таблицы экспорта
     всех доступных модулей (свой разбор PE, dumpbin не нужен).
  3. Сверяет: есть ли у целевого модуля каждая запрошенная функция.
  4. Печатает точный список: кто требует, от кого, чего нет.
  5. В конце делает живой тест загрузки.

  Система не меняется.

ЗАПУСК

  python scripts\\diag-deps-full.py
  python scripts\\diag-deps-full.py --keep
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

DEFAULT_HIP = r"C:\Program Files\AMD\ROCm\6.4"
OLD = b"hipGetDevicePropertiesR0600"
NEW = b"hipGetDevicePropertiesR0000"

# что синтетически подменяем: имя файла -> откуда взять
SWAP = {"amdhip64_6.dll": "System32\\amdhip64.dll"}

# эти системные модули не анализируем - это Windows и CRT
SKIP = (
    "kernel32", "ntdll", "api-ms-win", "msvcp", "vcruntime", "ucrtbase",
    "ole32", "shell32", "shlwapi", "bcrypt", "user32", "advapi32",
    "ws2_32", "psapi", "version", "dbghelp", "crypt32", "iphlpapi",
    "dxgi", "d3d", "opengl32", "gdi32", "setupapi", "cfgmgr32",
)


def hr(t):
    print()
    print("=" * 70)
    print("  " + t)
    print("=" * 70)


def step(t):
    print()
    print("  >> " + t)


def ok(t, n=""):
    print("  [OK]   " + t)
    if n:
        print("         " + n)


def bad(t, n=""):
    print("  [XX]   " + t)
    if n:
        print("         " + n)


def warn(t, n=""):
    print("  [!!]   " + t)
    if n:
        print("         " + n)


def dim(t):
    print("         " + t)


# ======================================================================
#  Разбор PE
# ======================================================================
class PE(object):
    def __init__(self, path):
        self.path = path
        self.ok = False
        self.exports = []
        self.imports = {}
        try:
            with open(path, "rb") as f:
                self.d = f.read()
        except OSError:
            return
        if len(self.d) < 0x40 or self.d[:2] != b"MZ":
            return
        e = struct.unpack_from("<I", self.d, 0x3C)[0]
        if self.d[e:e + 4] != b"PE\0\0":
            return
        coff = e + 4
        nsec = struct.unpack_from("<H", self.d, coff + 2)[0]
        size_opt = struct.unpack_from("<H", self.d, coff + 16)[0]
        opt = coff + 20
        magic = struct.unpack_from("<H", self.d, opt)[0]
        self.is64 = magic == 0x20B
        self.dd = opt + (112 if self.is64 else 96)
        sec = opt + size_opt
        self.sections = []
        for i in range(nsec):
            b = sec + i * 40
            self.sections.append((
                struct.unpack_from("<I", self.d, b + 12)[0],
                struct.unpack_from("<I", self.d, b + 16)[0],
                struct.unpack_from("<I", self.d, b + 20)[0],
            ))
        self.ok = True
        self.exports = self._exports()
        self.imports = self._imports()

    def off(self, rva):
        for va, size, ptr in self.sections:
            if va <= rva < va + size:
                return ptr + (rva - va)
        return None

    def cstr(self, rva):
        o = self.off(rva)
        if o is None:
            return None
        end = self.d.find(b"\0", o)
        if end < 0:
            return None
        return self.d[o:end].decode("ascii", "replace")

    def _exports(self):
        exp_rva = struct.unpack_from("<I", self.d, self.dd)[0]
        if not exp_rva:
            return []
        o = self.off(exp_rva)
        if o is None or o + 40 > len(self.d):
            return []
        n_names = struct.unpack_from("<I", self.d, o + 24)[0]
        addr_names = struct.unpack_from("<I", self.d, o + 32)[0]
        if not n_names or not addr_names:
            return []
        no = self.off(addr_names)
        if no is None:
            return []
        out = []
        for i in range(n_names):
            if no + i * 4 + 4 > len(self.d):
                break
            nm = self.cstr(struct.unpack_from("<I", self.d, no + i * 4)[0])
            if nm:
                out.append(nm)
        return out

    def _imports(self):
        imp_rva = struct.unpack_from("<I", self.d, self.dd + 8)[0]
        if not imp_rva:
            return {}
        o = self.off(imp_rva)
        if o is None:
            return {}
        out = {}
        while o + 20 <= len(self.d):
            desc = self.d[o:o + 20]
            if desc == b"\0" * 20:
                break
            oft, _, _, name_rva, first_thunk = struct.unpack("<IIIII", desc)
            dll = self.cstr(name_rva)
            if not dll:
                break
            names = []
            to = self.off(oft or first_thunk)
            if to is not None:
                step_sz = 8 if self.is64 else 4
                fmt = "<Q" if self.is64 else "<I"
                flag = (1 << 63) if self.is64 else (1 << 31)
                while to + step_sz <= len(self.d):
                    val = struct.unpack_from(fmt, self.d, to)[0]
                    if val == 0:
                        break
                    if val & flag:
                        names.append("ordinal:%d" % (val & 0xFFFF))
                    else:
                        fn = self.cstr((val & 0x7FFFFFFF) + 2)
                        if fn:
                            names.append(fn)
                    to += step_sz
            out[dll] = names
            o += 20
        return out


def patch_import(path, dll_name, old, new):
    if len(old) != len(new):
        return -1
    with open(path, "rb") as f:
        data = bytearray(f.read())
    e = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e + 4
    size_opt = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    is64 = struct.unpack_from("<H", data, opt)[0] == 0x20B
    dd = opt + (112 if is64 else 96)
    imp_rva = struct.unpack_from("<I", data, dd + 8)[0]
    if not imp_rva:
        return 0

    def r2o(rva):
        nsec = struct.unpack_from("<H", data, coff + 2)[0]
        sec = opt + size_opt
        for i in range(nsec):
            b = sec + i * 40
            va = struct.unpack_from("<I", data, b + 12)[0]
            raw_size = struct.unpack_from("<I", data, b + 16)[0]
            raw_ptr = struct.unpack_from("<I", data, b + 20)[0]
            if va <= rva < va + raw_size:
                return raw_ptr + (rva - va)
        return None

    o = r2o(imp_rva)
    if o is None:
        return -1
    replaced = 0
    target = dll_name.lower().encode("ascii")
    while o + 20 <= len(data):
        desc = bytes(data[o:o + 20])
        if desc == b"\0" * 20:
            break
        oft, _, _, name_rva, first_thunk = struct.unpack("<IIIII", desc)
        noff = r2o(name_rva)
        if noff is None:
            break
        end = data.find(b"\0", noff)
        if bytes(data[noff:end]).lower() != target:
            o += 20
            continue
        to = r2o(oft or first_thunk)
        if to is not None:
            step_sz = 8 if is64 else 4
            fmt = "<Q" if is64 else "<I"
            flag = (1 << 63) if is64 else (1 << 31)
            while to + step_sz <= len(data):
                val = struct.unpack_from(fmt, data, to)[0]
                if val == 0:
                    break
                if not (val & flag):
                    n_off = r2o((val & 0x7FFFFFFF) + 2)
                    if n_off is not None and bytes(data[n_off:n_off + len(old)]) == old:
                        data[n_off:n_off + len(old)] = new
                        replaced += 1
                to += step_sz
        break
    if replaced:
        with open(path, "wb") as f:
            f.write(data)
    return replaced


def is_system(name):
    n = name.lower()
    return any(n.startswith(s) for s in SKIP)


def main():
    hip = DEFAULT_HIP
    keep = "--keep" in sys.argv
    for i, a in enumerate(sys.argv):
        if a == "--hip" and i + 1 < len(sys.argv):
            hip = sys.argv[i + 1]

    bindir = os.path.join(hip, "bin")
    sys32 = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "System32")

    hr("ПОЛНЫЙ ОБХОД ЗАВИСИМОСТЕЙ rocblas.dll")

    tmp = tempfile.mkdtemp(prefix="deps-full-")
    try:
        # ---- 1. собираем комплект ----
        step("1. Комплект во временной папке")
        # все DLL, которые rocblas тянет из своего bin, копируем как есть
        for name in ("rocblas.dll", "hipblaslt.dll"):
            src = os.path.join(bindir, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(tmp, name))
                dim("скопирован " + name)

        # подменяем рантайм драйверным
        for dst_name, src_desc in SWAP.items():
            rel = src_desc.split("\\", 1)[1] if "\\" in src_desc else src_desc
            src = os.path.join(sys32, rel)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(tmp, dst_name))
                ok(src_desc + "  ->  " + dst_name)
            else:
                bad("не найден " + src)
                return 2
        dim(tmp)

        # ---- 2. патчим rocblas ----
        step("2. Патч R0600 -> R0000 в rocblas.dll")
        n = patch_import(os.path.join(tmp, "rocblas.dll"), "amdhip64_6.dll", OLD, NEW)
        if n > 0:
            ok("заменено вхождений: %d" % n)
        elif n == 0:
            warn("вхождений не найдено (возможно, уже пропатчен)")
        else:
            bad("патч не применился")
            return 2

        # ---- 3. таблицы экспорта доступных модулей ----
        step("3. Что экспортируют доступные модули")
        avail = {}
        for name in sorted(os.listdir(tmp)):
            if not name.lower().endswith(".dll"):
                continue
            pe = PE(os.path.join(tmp, name))
            if pe.ok and pe.exports:
                avail[name.lower()] = set(pe.exports)
                dim("%-22s %4d функций" % (name, len(pe.exports)))
        # плюс SDK-шные DLL, которые могут понадобиться
        for name in sorted(os.listdir(bindir)):
            low = name.lower()
            if not low.endswith(".dll") or low in avail:
                continue
            if low.startswith(("rocblas", "amdhip64_6", "hipblaslt")):
                continue
            pe = PE(os.path.join(bindir, name))
            if pe.ok and pe.exports:
                avail[low] = set(pe.exports)

        # ---- 4. сверка ----
        step("4. Сверка импортов со списком экспорта")
        problems = []
        for name in sorted(os.listdir(tmp)):
            low = name.lower()
            if not low.endswith(".dll"):
                continue
            pe = PE(os.path.join(tmp, name))
            if not pe.ok:
                continue
            for dll, fns in pe.imports.items():
                dl = dll.lower()
                if is_system(dl):
                    continue
                exported = avail.get(dl)
                if exported is None:
                    warn("%s требует %s - модуль недоступен для сверки" % (name, dll))
                    continue
                missing = [f for f in fns
                           if not f.startswith("ordinal:") and f not in exported]
                if missing:
                    problems.append((name, dll, missing))
                    bad("%s -> %s : нет %d функций" % (name, dll, len(missing)))
                    for f in missing[:15]:
                        dim("    " + f)
                    if len(missing) > 15:
                        dim("    ... и ещё %d" % (len(missing) - 15))

        print()
        if not problems:
            ok("все зависимости разрешаются")
        else:
            bad("НАЙДЕНО ПРОБЛЕМ: %d" % len(problems))

        # ---- 5. живой тест ----
        step("5. Живой тест загрузки")
        code = r'''
import ctypes, os
d = r"%s"
os.add_dll_directory(d)
for name in ("hipblaslt.dll", "rocblas.dll"):
    try:
        ctypes.WinDLL(os.path.join(d, name))
        print("LOADED", name)
    except OSError as e:
        print("LOADFAIL", name, e)
''' % tmp
        env = dict(os.environ)
        env["PATH"] = ";".join(
            p for p in env.get("PATH", "").split(";")
            if "ROCm" not in p and "rocm" not in p
        )
        pr = subprocess.run([sys.executable, "-c", code],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env)
        for line in pr.stdout.decode("utf-8", "replace").strip().splitlines():
            dim(line)

        hr("ИТОГ")
        if not problems:
            print("  Все импорты разрешаются статически.")
            dim("Если загрузка всё равно падает, причина не в отсутствии")
            dim("функций, а в чём-то ещё. Пришлите вывод.")
        else:
            print("  Причина WinError 127 найдена - список выше.")
            print()
            dim("Для каждой недостающей функции есть два варианта:")
            dim("  * если у драйверного рантайма есть аналог с другим именем,")
            dim("    можно пропатчить имя так же, как R0600 -> R0000;")
            dim("  * если аналога нет, этот модуль не заставить работать")
            dim("    с рантаймом драйвера, и нужен rocBLAS 5.7.1.")

        if keep:
            dim("")
            dim("Папка сохранена: " + tmp)
        return 0 if not problems else 1
    finally:
        if not keep:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
