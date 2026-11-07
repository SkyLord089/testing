#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Что именно импортирует rocblas.dll - и почему он не видит карту.

СИТУАЦИЯ

  Замерено на этой машине:

    C:\\Program Files\\AMD\\ROCm\\6.4\\bin\\amdhip64_6.dll  v10.0.3652.0  -> 0 устройств
    C:\\WINDOWS\\System32\\amdhip64.dll                    v10.0.3584.0  -> 1 устройство

  То есть рантайм от драйвера карту видит, а рантайм из HIP SDK - нет.

РЕШАЮЩИЙ ВОПРОС

  С какой из этих двух DLL связан rocblas.dll?

  По документации AMD, начиная с HIP SDK 6.1.2, рантайм разделён на две DLL:
  amdhip64.dll (старое имя) и amdhip64_6.dll (новое). Библиотеки SDK
  собираются против НОВОГО имени, то есть жёстко импортируют amdhip64_6.dll.

  Если это так, то rocblas.dll из SDK 6.4 физически не может работать на
  RX 580: он всегда грузит рантайм, который карту не видит. Перестановка
  PATH в этом случае не помогает вообще - имя в таблице импорта задано
  жёстко.

ЧТО ДЕЛАЕТ СКРИПТ

  1. Читает таблицу импорта PE-файлов (чистый Python, без dumpbin):
     rocblas.dll, amdhip64_6.dll, nvcuda.dll и cublas64_11.dll из venv.
  2. Показывает, какое имя рантайма прописано в каждом.
  3. Запускает оба рантайма с AMD_LOG_LEVEL=3 и печатает значимые строки
     трассировки - чтобы было видно, отбрасывается ли gfx803 явно.

ЗАПУСК

  python scripts\\diag-pe-imports.py
"""

import os
import struct
import subprocess
import sys

DEFAULT_HIP = r"C:\Program Files\AMD\ROCm\6.4"
DEFAULT_VENV = r"D:\Apps\rx-580-comfyui\ComfyUI-Zluda\venv"


# ----------------------------------------------------------------------
#  Разбор таблицы импорта PE (32/64 бит)
# ----------------------------------------------------------------------
def pe_imports(path):
    """Вернуть список имён DLL из таблицы импорта. Пустой список при ошибке."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None

    if len(data) < 0x40 or data[:2] != b"MZ":
        return None

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return None

    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", data, coff + 2)[0]
    size_opt = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20

    magic = struct.unpack_from("<H", data, opt)[0]
    is64 = magic == 0x20B

    # каталог импорта - запись 1 в DataDirectory
    dd_off = opt + (112 if is64 else 96)
    imp_rva = struct.unpack_from("<I", data, dd_off + 8)[0]
    if imp_rva == 0:
        return []

    # таблица секций
    sec_off = opt + size_opt
    sections = []
    for i in range(num_sections):
        base = sec_off + i * 40
        vaddr = struct.unpack_from("<I", data, base + 12)[0]
        raw_size = struct.unpack_from("<I", data, base + 16)[0]
        raw_ptr = struct.unpack_from("<I", data, base + 20)[0]
        sections.append((vaddr, raw_size, raw_ptr))

    def rva_to_off(rva):
        for vaddr, raw_size, raw_ptr in sections:
            if vaddr <= rva < vaddr + raw_size:
                return raw_ptr + (rva - vaddr)
        return None

    names = []
    off = rva_to_off(imp_rva)
    if off is None:
        return []
    while True:
        if off + 20 > len(data):
            break
        desc = data[off:off + 20]
        if desc == b"\0" * 20:
            break
        name_rva = struct.unpack_from("<I", desc, 12)[0]
        if name_rva == 0:
            break
        noff = rva_to_off(name_rva)
        if noff is None:
            break
        end = data.find(b"\0", noff)
        if end < 0:
            break
        names.append(data[noff:end].decode("ascii", "replace"))
        off += 20
    return names


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


def show_imports(label, path, interesting=("amdhip64", "comgr", "hsa")):
    step(label)
    dim(path)
    if not os.path.isfile(path):
        bad("файла нет")
        return []
    imps = pe_imports(path)
    if imps is None:
        bad("не PE-файл или не читается")
        return []
    if not imps:
        warn("таблица импорта пуста или не найдена")
        return []
    hit = [i for i in imps if any(k in i.lower() for k in interesting)]
    for i in imps:
        mark = "  <== " if i in hit else "      "
        print("      %s%s" % (mark, i))
    print()
    if hit:
        ok("нужные зависимости: " + ", ".join(hit))
    else:
        warn("не нашёл amdhip64/comgr среди импортов")
    return imps


def trace_enum(dll_path, label):
    """Запустить hipGetDeviceCount с трассировкой HIP и вернуть текст лога."""
    step(label)
    code = (
        "import ctypes,os;"
        "lib=ctypes.WinDLL(r'%s');"
        "lib.hipGetDeviceCount.restype=ctypes.c_int;"
        "n=ctypes.c_int(-1);"
        "r=lib.hipGetDeviceCount(ctypes.byref(n));"
        "print('RESULT %%d %%d' %% (r,n.value))"
    ) % dll_path

    env = dict(os.environ)
    env["AMD_LOG_LEVEL"] = "3"
    env["AMD_LOG_MASK"] = "0x7FFFFFFF"
    env["AMD_LOG_LEVEL_FILE"] = ""

    try:
        p = subprocess.run([sys.executable, "-c", code],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           env=env)
        out = p.stdout.decode("utf-8", "replace")
    except OSError as e:
        bad("не удалось запустить", str(e))
        return ""

    res = [l for l in out.splitlines() if l.startswith("RESULT")]
    if res:
        _, r, n = res[0].split()
        if int(r) == 0 and int(n) > 0:
            ok("устройств: %s" % n)
        else:
            bad("код %s, устройств %s" % (r, n))
    return out


def main():
    hip = DEFAULT_HIP
    venv = DEFAULT_VENV
    for i, a in enumerate(sys.argv):
        if a == "--hip" and i + 1 < len(sys.argv):
            hip = sys.argv[i + 1]
        if a == "--venv" and i + 1 < len(sys.argv):
            venv = sys.argv[i + 1]

    bindir = os.path.join(hip, "bin")
    sdk_hip = os.path.join(bindir, "amdhip64_6.dll")
    sys_hip = os.path.join(os.environ.get("WINDIR", r"C:\Windows"),
                           "System32", "amdhip64.dll")

    hr("ЧТО ИМПОРТИРУЕТ rocblas.dll И ПОЧЕМУ КАРТА НЕ ВИДНА")

    # ---- 1. Таблицы импорта ----
    rocblas_imps = show_imports("1. rocblas.dll (HIP SDK 6.4)",
                                os.path.join(bindir, "rocblas.dll"))

    venv_scripts = os.path.join(venv, "Scripts")
    show_imports("2. nvcuda.dll (ZLUDA)",
                 os.path.join(venv_scripts, "nvcuda.dll"))
    show_imports("3. cublas64_11.dll (ZLUDA, через него идёт cublasCreate)",
                 os.path.join(venv_scripts, "cublas64_11.dll"))

    # ---- 2. Трассировка перечисления устройств ----
    hr("ТРАССИРОВКА ПЕРЕЧИСЛЕНИЯ УСТРОЙСТВ")
    sdk_trace = trace_enum(sdk_hip, "amdhip64_6.dll (HIP SDK 6.4)")
    sys_trace = trace_enum(sys_hip, "amdhip64.dll (драйвер, System32)")

    keys = ("gfx", "agent", "isa", "hsa", "dxcore", "adapter", "device",
            "error", "fail", "unsupported", "skip", "filter")

    for label, text in (("SDK 6.4", sdk_trace), ("System32", sys_trace)):
        lines = [l.strip() for l in text.splitlines()
                 if any(k in l.lower() for k in keys)
                 and not l.startswith("RESULT")]
        print()
        dim("%s - значимые строки (%d):" % (label, len(lines)))
        for line in lines[:25]:
            print("   " + line[:200])

    # ---- 3. Вывод ----
    hr("ВЫВОД")

    rocblas_uses_six = any("amdhip64_6" in i.lower() for i in (rocblas_imps or []))
    rocblas_uses_old = any(i.lower() == "amdhip64.dll" for i in (rocblas_imps or []))

    if rocblas_uses_six and not rocblas_uses_old:
        print("  rocblas.dll импортирует ИМЕННО amdhip64_6.dll.")
        print()
        dim("Имя в таблице импорта задано жёстко. Это значит:")
        dim("  * rocBLAS из SDK 6.4 ВСЕГДА грузит рантайм SDK,")
        dim("  * а этот рантайм карту не видит (0 устройств),")
        dim("  * и перестановка PATH тут не поможет вообще.")
        print()
        dim("Отсюда следствие: rocBLAS из HIP SDK 6.4 в принципе не может")
        dim("работать на RX 580. Дело не в ядрах Tensile - до них дело")
        dim("даже не доходит, потому что handle не создаётся.")
        print()
        dim("Рабочий обход: взять rocBLAS из HIP SDK 5.7.1. Там:")
        dim("  * gfx803 есть в TARGET_LIST, то есть ядра уже собраны;")
        dim("  * rocblas.dll импортирует старое имя amdhip64.dll,")
        dim("    а его в System32 видит карту.")
    elif rocblas_uses_old:
        print("  rocblas.dll импортирует старое имя amdhip64.dll.")
        dim("Тогда он должен брать рантайм драйвера. Смотрите трассировку.")
    else:
        print("  Не удалось определить зависимость rocblas.dll.")
        dim("Пришлите раздел 1 - разберём по таблице импорта.")

    print()
    dim("Если в трассировке SDK видно, что агент gfx803 найден, но")
    dim("отброшен - это подтверждает, что рантайм 6.x не поддерживает")
    dim("Polaris. Тогда решение - rocBLAS 5.7.1, а не сборка ядер.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
