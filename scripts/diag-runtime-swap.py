#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Можно ли подменить amdhip64_6.dll драйверным рантаймом.

УСТАНОВЛЕННЫЙ ФАКТ

    rocblas.dll  -> amdhip64_6.dll   (HIP SDK 6.4)   -> 0 устройств
    nvcuda.dll   -> amdhip64.dll     (System32, драйвер) -> 1 устройство

  Поэтому nvcuda (ядро ZLUDA) работает, а cublas (через rocblas.dll) - нет.

ЧТО ПРОВЕРЯЕТСЯ

  Две вещи, обе без установки чего-либо:

  1. СТАТИЧЕСКИ. rocblas.dll импортирует функции из amdhip64_6.dll по именам.
     Сравниваем этот список с экспортами System32\\amdhip64.dll.
     Если хоть одной функции нет - подмена невозможна, DLL просто не
     загрузится (WinError 127: не найдена процедура).

  2. ЖИВОЙ ТЕСТ. В отдельной временной папке собираем комплект:
         rocblas.dll          копия из SDK
         amdhip64_6.dll       КОПИЯ System32\\amdhip64.dll под нужным именем
     и вызываем rocblas_create_handle. Систему не трогаем.
     Если handle создастся - подмена решает проблему целиком.

ЗАПУСК

  python scripts\\diag-runtime-swap.py
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

DEFAULT_HIP = r"C:\Program Files\AMD\ROCm\6.4"
IMPORT_DLL = "amdhip64_6.dll"


# ======================================================================
#  Минимальный разбор PE: импорт по именам и таблица экспорта
# ======================================================================
class PE(object):
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        self.ok = False
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
                struct.unpack_from("<I", self.d, b + 12)[0],   # VA
                struct.unpack_from("<I", self.d, b + 16)[0],   # raw size
                struct.unpack_from("<I", self.d, b + 20)[0],   # raw ptr
            ))
        self.ok = True

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

    # ---- импорт: {имя_dll: [(имя_функции | 'ordinal:N')]} ----
    def imports(self):
        if not self.ok:
            return {}
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
            thunk_rva = oft or first_thunk
            to = self.off(thunk_rva)
            if to is not None:
                step = 8 if self.is64 else 4
                fmt = "<Q" if self.is64 else "<I"
                flag = (1 << 63) if self.is64 else (1 << 31)
                while to + step <= len(self.d):
                    val = struct.unpack_from(fmt, self.d, to)[0]
                    if val == 0:
                        break
                    if val & flag:
                        names.append("ordinal:%d" % (val & 0xFFFF))
                    else:
                        fn = self.cstr((val & 0x7FFFFFFF) + 2)
                        if fn:
                            names.append(fn)
                    to += step
            out[dll.lower()] = names
            o += 20
        return out

    # ---- экспорт: список имён ----
    #  IMAGE_EXPORT_DIRECTORY:
    #    +0  Characteristics      +20 NumberOfFunctions
    #    +12 Name                 +24 NumberOfNames
    #    +16 Base                 +32 AddressOfNames
    def exports(self):
        if not self.ok:
            return []
        exp_rva = struct.unpack_from("<I", self.d, self.dd)[0]
        if not exp_rva:
            return []
        o = self.off(exp_rva)
        if o is None:
            return []
        if o + 40 > len(self.d):
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
            rva = struct.unpack_from("<I", self.d, no + i * 4)[0]
            nm = self.cstr(rva)
            if nm:
                out.append(nm)
        return out


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


def main():
    hip = DEFAULT_HIP
    for i, a in enumerate(sys.argv):
        if a == "--hip" and i + 1 < len(sys.argv):
            hip = sys.argv[i + 1]

    bindir = os.path.join(hip, "bin")
    rocblas = os.path.join(bindir, "rocblas.dll")
    sdk_hip = os.path.join(bindir, IMPORT_DLL)
    sys_hip = os.path.join(os.environ.get("WINDIR", r"C:\Windows"),
                           "System32", "amdhip64.dll")

    hr("МОЖНО ЛИ ПОДМЕНИТЬ amdhip64_6.dll ДРАЙВЕРНЫМ РАНТАЙМОМ")

    for p in (rocblas, sdk_hip, sys_hip):
        if not os.path.isfile(p):
            bad("нет файла", p)
            return 2

    # ------------------------------------------------------------------
    step("1. Какие функции rocblas.dll требует от amdhip64_6.dll")
    pe = PE(rocblas)
    if not pe.ok:
        bad("не удалось разобрать rocblas.dll")
        return 2
    imps = pe.imports()
    needed = imps.get(IMPORT_DLL.lower(), [])
    if not needed:
        warn("rocblas.dll не импортирует %s по именам" % IMPORT_DLL)
        dim("Возможно импорт идёт по ординалам или через задержку.")
        for dll, fns in imps.items():
            if "hip" in dll or "comgr" in dll:
                dim("%s: %d функций" % (dll, len(fns)))
        needed = []

    if needed:
        ok("требуется функций: %d" % len(needed))
        for f in needed[:12]:
            dim(f)
        if len(needed) > 12:
            dim("... и ещё %d" % (len(needed) - 12))

    # ------------------------------------------------------------------
    step("2. Проверка через GetProcAddress (надёжнее разбора PE)")
    #  Грузим драйверный рантайм напрямую и спрашиваем по каждой функции,
    #  есть ли она. Это ровно то, что делает загрузчик Windows при импорте,
    #  поэтому результат совпадает с реальным поведением.
    import ctypes
    try:
        sys_lib = ctypes.WinDLL(sys_hip)
    except OSError as e:
        bad("драйверный рантайм не загрузился", str(e))
        return 2
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.GetProcAddress.restype = ctypes.c_void_p
    k32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    handle = sys_lib._handle

    if needed:
        missing = []
        present = 0
        for fn in needed:
            if fn.startswith("ordinal:"):
                continue
            addr = k32.GetProcAddress(handle, fn.encode("ascii"))
            if addr:
                present += 1
            else:
                missing.append(fn)

        ok("есть функций: %d из %d" % (present, present + len(missing)))
        if missing:
            bad("НЕ ХВАТАЕТ %d функций" % len(missing))
            for f in missing[:20]:
                dim("нет: " + f)
            if len(missing) > 20:
                dim("... и ещё %d" % (len(missing) - 20))
            dim("")
            dim("Подмена не сработает: rocblas.dll не загрузится")
            dim("(WinError 127 - не найдена процедура).")
        else:
            ok("ВСЕ требуемые функции есть у драйверного рантайма")
            dim("Статическая проверка пройдена - живой тест ниже решает.")
    else:
        warn("список требований пуст, проверять нечего")

    # для справки: сколько всего экспортирует драйверный рантайм
    sys_exp = PE(sys_hip).exports() if PE(sys_hip).ok else []
    if sys_exp:
        dim("драйверный рантайм экспортирует: %d функций" % len(sys_exp))
    sdk_exp = PE(sdk_hip).exports() if PE(sdk_hip).ok else []
    if sdk_exp:
        dim("SDK-шный amdhip64_6.dll экспортирует: %d функций" % len(sdk_exp))
        if sys_exp:
            only_sdk = sorted(set(sdk_exp) - set(sys_exp))
            if only_sdk:
                dim("есть только в SDK (первые 10): " + ", ".join(only_sdk[:10]))

    # ------------------------------------------------------------------
    step("4. Живой тест в отдельной папке (систему не трогаем)")
    tmp = tempfile.mkdtemp(prefix="hipsdk-swap-")
    try:
        shutil.copy2(rocblas, os.path.join(tmp, "rocblas.dll"))
        # hipblaslt.dll тоже импортируется rocblas.dll - копируем, если есть
        hlt = os.path.join(bindir, "hipblaslt.dll")
        if os.path.isfile(hlt):
            shutil.copy2(hlt, os.path.join(tmp, "hipblaslt.dll"))
        # ГЛАВНОЕ: драйверный рантайм под именем, которое ждёт rocblas.dll
        shutil.copy2(sys_hip, os.path.join(tmp, IMPORT_DLL))
        dim("собрано в: " + tmp)

        code = r"""
import ctypes, os, sys
d = r"%s"
os.add_dll_directory(d)
try:
    lib = ctypes.WinDLL(os.path.join(d, "rocblas.dll"))
except OSError as e:
    print("LOADFAIL", e)
    raise SystemExit(3)

# какой именно amdhip64 попал в процесс
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.GetModuleHandleW.restype = ctypes.c_void_p
k32.GetModuleFileNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint]
h = k32.GetModuleHandleW("amdhip64_6.dll")
if h:
    buf = ctypes.create_unicode_buffer(1024)
    k32.GetModuleFileNameW(h, buf, 1024)
    print("MODULE", buf.value)
else:
    print("MODULE not-loaded")

lib.rocblas_create_handle.restype = ctypes.c_int
hnd = ctypes.c_void_p()
r = lib.rocblas_create_handle(ctypes.byref(hnd))
print("STATUS %%d" %% r)
""" % tmp

        env = dict(os.environ)
        # убираем bin SDK из PATH, чтобы не перехватил настоящий amdhip64_6.dll
        env["PATH"] = ";".join(
            p for p in env.get("PATH", "").split(";")
            if "ROCm" not in p and "rocm" not in p
        )
        pr = subprocess.run([sys.executable, "-c", code],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env)
        out = pr.stdout.decode("utf-8", "replace").strip()
        for line in out.splitlines():
            dim(line)

        status = None
        loaded = None
        for line in out.splitlines():
            if line.startswith("STATUS "):
                status = int(line.split()[1])
            if line.startswith("MODULE "):
                loaded = line[7:].strip()

        print()
        if "LOADFAIL" in out:
            bad("rocblas.dll не загрузился с этой подменой")
            dim("Это и есть WinError 127 - не хватает функций.")
        elif status == 0:
            ok("rocblas_create_handle СОЗДАЛ HANDLE")
            dim("Подмена рантайма решает проблему.")
            if loaded:
                dim("загружен модуль: " + loaded)
        elif status is not None:
            bad("handle не создан, статус %d" % status)
            if loaded:
                dim("загружен модуль: " + loaded)
            dim("6 = rocblas_status_internal_error (как было раньше).")
        else:
            warn("тест не дал статуса")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ------------------------------------------------------------------
    hr("ЧТО ДАЛЬШЕ")
    dim("Если подмена сработала, то исправление - копия System32\\amdhip64.dll")
    dim("под именем amdhip64_6.dll в bin HIP SDK, с бэкапом оригинала.")
    dim("Это НЕ то же самое, что перестановка PATH: имя файла совпадает,")
    dim("поэтому rocblas.dll загрузит именно эту копию.")
    dim("")
    dim("Если не сработало - путь через rocBLAS 5.7.1:")
    dim("там gfx803 в списке сборки, ядра уже готовы, а имя рантайма")
    dim("старое (amdhip64.dll), то есть драйверный рантайм подходит.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
