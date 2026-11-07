#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Патч имени функции в таблице импорта rocblas.dll + живой тест.

СИТУАЦИЯ

  rocblas.dll требует от amdhip64_6.dll ровно 38 функций. Драйверный
  рантайм System32\\amdhip64.dll даёт 37 из них. Не хватает ОДНОЙ:

      hipGetDevicePropertiesR0600

  Это версионированный символ, появившийся в ROCm 6.0. У драйверного
  рантайма есть его предшественник hipGetDevicePropertiesR0000.

ЗАЧЕМ ЭТО МОЖЕТ СРАБОТАТЬ

  Имена одной длины - по 27 байт:

      hipGetDevicePropertiesR0600
      hipGetDevicePropertiesR0000

  Поэтому имя можно заменить прямо в таблице импорта DLL, байт в байт,
  не меняя размеров и смещений в файле. rocblas.dll начнёт просить
  R0000 вместо R0600 - а эта функция у драйверного рантайма есть.

ЧТО ЭТО ЗНАЧИТ НА ПРАКТИКЕ

  R0600 отдаёт структуру hipDeviceProp_t нового размера (в ROCm 6.0 в неё
  добавили поля). R0000 отдаёт старую, короткую. Если рантайм заполнит
  меньше байт, чем ждёт rocBLAS, хвост структуры останется неинициализирован.

  Для нас важно, что rocBLAS читает из неё: gcnArchName и параметры
  устройства. Они лежат в НАЧАЛЕ структуры, поэтому с большой вероятностью
  прочитаются правильно.

  НО: гарантии нет. Опаснее всего, если rocBLAS передаст в функцию буфер
  меньшего размера, чем ожидает рантайм - тогда рантайм запишет за его
  пределы. Поэтому скрипт сначала делает ЖИВОЙ ТЕСТ на копии во временной
  папке: смотрит, создаётся ли handle и не падает ли процесс.

ЧТО ДЕЛАЕТ СКРИПТ

  1. Копирует rocblas.dll во временную папку.
  2. Патчит в копии имя R0600 -> R0000 (с проверкой, что строка одна).
  3. Кладёт рядом системный amdhip64.dll под именем amdhip64_6.dll.
  4. Вызывает rocblas_create_handle и читает свойства устройства.
  5. Подробно рассказывает, что делать с результатом.

  Система НЕ меняется. Ничего не устанавливается насовсем.

ЗАПУСК

  python scripts\\patch-rocblas-import.py
  python scripts\\patch-rocblas-import.py --keep     (не удалять временную папку)

КОДЫ ВОЗВРАТА

  0  handle создан, устройство читается - патч годится
  1  не сработало - смотрите причину в выводе
  2  не удалось выполнить подготовку
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


# ----------------------------------------------------------------------
#  Патч таблицы импорта
# ----------------------------------------------------------------------
def _rva_to_off(data, rva):
    e = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    size_opt = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    sec = opt + size_opt
    for i in range(nsec):
        b = sec + i * 40
        va = struct.unpack_from("<I", data, b + 12)[0]
        raw_size = struct.unpack_from("<I", data, b + 16)[0]
        raw_ptr = struct.unpack_from("<I", data, b + 20)[0]
        if va <= rva < va + raw_size:
            return raw_ptr + (rva - va)
    return None


def patch_import_name(path, dll_name, old, new):
    """Заменить имя функции в таблице импорта указанной DLL.

    Возвращает число замен или -1 при ошибке. Длина old и new должна
    совпадать - иначе сместились бы все имена после неё.
    """
    if len(old) != len(new):
        return -1

    with open(path, "rb") as f:
        data = bytearray(f.read())

    if data[:2] != b"MZ":
        return -1
    e = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e:e + 4] != b"PE\0\0":
        return -1
    coff = e + 4
    size_opt = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    is64 = magic == 0x20B
    dd = opt + (112 if is64 else 96)

    imp_rva = struct.unpack_from("<I", data, dd + 8)[0]
    if not imp_rva:
        return 0
    o = _rva_to_off(data, imp_rva)
    if o is None:
        return -1

    replaced = 0
    target = dll_name.lower().encode("ascii")

    while o + 20 <= len(data):
        desc = bytes(data[o:o + 20])
        if desc == b"\0" * 20:
            break
        oft, _, _, name_rva, first_thunk = struct.unpack("<IIIII", desc)
        noff = _rva_to_off(data, name_rva)
        if noff is None:
            break
        end = data.find(b"\0", noff)
        cur = bytes(data[noff:end]).lower()
        if cur == target:
            thunk_rva = oft or first_thunk
            to = _rva_to_off(data, thunk_rva)
            if to is not None:
                step_sz = 8 if is64 else 4
                fmt = "<Q" if is64 else "<I"
                flag = (1 << 63) if is64 else (1 << 31)
                while to + step_sz <= len(data):
                    val = struct.unpack_from(fmt, data, to)[0]
                    if val == 0:
                        break
                    if not (val & flag):
                        name_off = _rva_to_off(data, (val & 0x7FFFFFFF) + 2)
                        if name_off is not None:
                            if bytes(data[name_off:name_off + len(old)]) == old:
                                data[name_off:name_off + len(old)] = new
                                replaced += 1
                    to += step_sz
            break
        o += 20

    if replaced:
        with open(path, "wb") as f:
            f.write(data)
    return replaced


def main():
    hip = DEFAULT_HIP
    keep = "--keep" in sys.argv
    for i, a in enumerate(sys.argv):
        if a == "--hip" and i + 1 < len(sys.argv):
            hip = sys.argv[i + 1]

    bindir = os.path.join(hip, "bin")
    rocblas = os.path.join(bindir, "rocblas.dll")
    hipblaslt = os.path.join(bindir, "hipblaslt.dll")
    sys_hip = os.path.join(os.environ.get("WINDIR", r"C:\Windows"),
                           "System32", "amdhip64.dll")

    hr("ПАТЧ ИМПОРТА rocblas.dll: R0600 -> R0000")
    dim("Проверяем на копии. Система не меняется.")

    for p in (rocblas, sys_hip):
        if not os.path.isfile(p):
            bad("нет файла", p)
            return 2

    tmp = tempfile.mkdtemp(prefix="rocblas-patch-")
    try:
        # ---- 1. копии ----
        step("1. Готовлю копии во временной папке")
        dst_roc = os.path.join(tmp, "rocblas.dll")
        shutil.copy2(rocblas, dst_roc)
        ok("rocblas.dll скопирован")
        if os.path.isfile(hipblaslt):
            shutil.copy2(hipblaslt, os.path.join(tmp, "hipblaslt.dll"))
            dim("hipblaslt.dll скопирован")
        # системный рантайм под именем, которого ждёт rocblas.dll
        shutil.copy2(sys_hip, os.path.join(tmp, "amdhip64_6.dll"))
        ok("System32\\amdhip64.dll -> amdhip64_6.dll")
        dim(tmp)

        # ---- 2. патч ----
        step("2. Патчу имя функции в таблице импорта")
        n = patch_import_name(dst_roc, "amdhip64_6.dll", OLD, NEW)
        if n < 0:
            bad("не удалось пропатчить (не PE или длины не совпали)")
            return 2
        if n == 0:
            bad("строка %s в таблице импорта не найдена" % OLD.decode())
            return 2
        ok("заменено вхождений: %d" % n, "%s -> %s" % (OLD.decode(), NEW.decode()))

        # ---- 3. живой тест ----
        step("3. Живой тест: создаётся ли handle и читается ли карта")
        code = r'''
import ctypes, os
d = r"%s"
os.add_dll_directory(d)
try:
    lib = ctypes.WinDLL(os.path.join(d, "rocblas.dll"))
except OSError as e:
    print("LOADFAIL", e)
    raise SystemExit(3)
print("LOADED ok")

lib.rocblas_create_handle.restype = ctypes.c_int
h = ctypes.c_void_p()
r = lib.rocblas_create_handle(ctypes.byref(h))
print("CREATE_STATUS", r)

# напрямую спросим у рантайма, что он знает про устройство
try:
    hl = ctypes.WinDLL(os.path.join(d, "amdhip64_6.dll"))
    hl.hipGetDeviceCount.restype = ctypes.c_int
    n = ctypes.c_int(-1)
    rc = hl.hipGetDeviceCount(ctypes.byref(n))
    print("DEVICE_COUNT", rc, n.value)
except Exception as e:
    print("DEVICE_COUNT_FAIL", e)

# и проверим, что R0000 вообще вызывается
try:
    buf = ctypes.create_string_buffer(1024)
    hl.hipGetDevicePropertiesR0000.restype = ctypes.c_int
    hl.hipGetDevicePropertiesR0000.argtypes = [ctypes.c_void_p, ctypes.c_int]
    rc = hl.hipGetDevicePropertiesR0000(buf, 0)
    print("R0000_STATUS", rc)
except Exception as e:
    print("R0000_FAIL", e)
''' % tmp

        env = dict(os.environ)
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

        # ---- 4. разбор ----
        print()
        created = False
        for line in out.splitlines():
            if line.startswith("CREATE_STATUS "):
                st = int(line.split()[1])
                if st == 0:
                    created = True
                    ok("rocblas_create_handle -> УСПЕХ")
                else:
                    bad("rocblas_create_handle -> статус %d" % st)
                    if st == 6:
                        dim("6 = internal_error: исключение внутри конструктора.")

        if "LOADFAIL" in out:
            bad("rocblas.dll не загрузился")
            dim("Патч не помог: не хватает ещё чего-то, кроме R0600.")
        elif created:
            ok("ГЛАВНОЕ: handle создаётся с драйверным рантаймом")
            dim("Значит одной этой функции и не хватало.")
        else:
            warn("handle не создан, но DLL загрузилась")
            dim("Смотрите DEVICE_COUNT и R0000_STATUS выше.")

        # ---- 5. итог ----
        hr("ИТОГ")
        if created:
            print("  Патч работает. Что делать дальше:")
            print()
            dim("Запустите установку с флагом -Apply:")
            dim("  python scripts\\patch-rocblas-import.py --apply")
            print()
            dim("Скрипт сделает бэкап rocblas.dll, применит патч к настоящему")
            dim("файлу в SDK и положит рядом драйверный рантайм под нужным")
            dim("именем. Откат - флагом -Restore.")
            print()
            warn("Остаётся риск: структура свойств устройства у R0000 короче,")
            dim("чем у R0600. rocBLAS читает из неё gcnArchName, и он лежит")
            dim("в начале - скорее всего прочитается верно. Но это надо")
            dim("проверить настоящим GEMM, а не только созданием handle.")
        else:
            print("  Так не вышло. Смотрите вывод выше.")
            print()
            dim("Если DLL загрузилась, но handle не создан - значит рантайм")
            dim("не видит карту по другой причине, либо структур несовместим.")
            print()
            dim("Тогда остаётся rocBLAS из HIP SDK 5.7.1: там gfx803")
            dim("в списке сборки, ядра уже готовы, а имя рантайма старое.")

        if keep:
            dim("")
            dim("Временная папка сохранена: " + tmp)
        return 0 if created else 1
    finally:
        if not keep:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
