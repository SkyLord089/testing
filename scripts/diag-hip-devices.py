#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Какой amdhip64 реально загружается и видит ли он карту.

ЧТО ВЫЯСНИЛОСЬ

  rocblas_create_handle падает с rocblas_status_internal_error, а причина -
  hipErrorNoDevice (код 100): HIP не видит ни одного устройства.
  Причём это происходит с amdhip64_6.dll из HIP SDK 6.4.

  При этом известно, что amdhip64.dll из System32 (от драйвера) устройство
  видит: hipInit=0, 1 устройство.

ЗАЧЕМ ЭТОТ СКРИПТ

  rocblas.dll линкуется с hip::host (library/CMakeLists.txt:78), то есть
  грузит amdhip64 по имени, а какая из двух DLL победит - зависит от
  порядка поиска. Скрипт показывает:

    1. какие amdhip64* лежат на диске и их версии;
    2. какие из них РЕАЛЬНО попали в процесс (по списку загруженных модулей);
    3. сколько устройств видит каждая из них при прямой загрузке.

ЗАПУСК

  python scripts\\diag-hip-devices.py
"""

import ctypes
import os
import sys
from ctypes import wintypes

DEFAULT_HIP = r"C:\Program Files\AMD\ROCm\6.4"

# Значения hipError_t
HIP_ERRORS = {
    0: "hipSuccess",
    1: "hipErrorInvalidValue",
    100: "hipErrorNoDevice",
    101: "hipErrorInvalidDevice",
    209: "hipErrorNoBinaryForGpu",
    303: "hipErrorOutOfMemory",
    400: "hipErrorInitializationError",
    401: "hipErrorInvalidDeviceFunction",
    719: "hipErrorLaunchFailure",
}

# Значения uint32_t в hipGetDeviceCount
HIP_RET = {0: "ok", 100: "hipErrorNoDevice (устройств нет)", 101: "hipErrorInvalidDevice"}


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


def file_version(path):
    """Версия файла через Win32 API, без внешних утилит."""
    try:
        size = ctypes.windll.version.GetFileVersionInfoSizeW(path, None)
        if not size:
            return "?"
        buf = ctypes.create_string_buffer(size)
        if not ctypes.windll.version.GetFileVersionInfoW(path, 0, size, buf):
            return "?"
        ptr = ctypes.c_void_p()
        ln = wintypes.UINT()
        if not ctypes.windll.version.VerQueryValueW(
            buf, "\\", ctypes.byref(ptr), ctypes.byref(ln)
        ):
            return "?"
        # VS_FIXEDFILEINFO
        fixed = ctypes.cast(ptr, ctypes.POINTER(ctypes.c_uint32 * 13)).contents
        ms, ls = fixed[2], fixed[3]
        return "%d.%d.%d.%d" % (
            (ms >> 16) & 0xFFFF, ms & 0xFFFF,
            (ls >> 16) & 0xFFFF, ls & 0xFFFF,
        )
    except Exception:
        return "?"


def loaded_modules():
    """Какие модули уже загружены в процесс (через toolhelp API)."""
    TH32CS_SNAPMODULE = 0x00000008
    TH32CS_SNAPMODULE32 = 0x00000010

    class MODULEENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("th32ModuleID", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("GlblcntUsage", wintypes.DWORD),
            ("ProccntUsage", wintypes.DWORD),
            ("modBaseAddr", ctypes.c_void_p),
            ("modBaseSize", wintypes.DWORD),
            ("hModule", wintypes.HMODULE),
            ("szModule", wintypes.WCHAR * 256),
            ("szExePath", wintypes.WCHAR * 260),
        ]

    k32 = ctypes.windll.kernel32
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0)
    if snap == -1:
        return []
    out = []
    try:
        me = MODULEENTRY32W()
        me.dwSize = ctypes.sizeof(MODULEENTRY32W)
        has = k32.Module32FirstW(snap, ctypes.byref(me))
        while has:
            out.append((me.szModule, me.szExePath))
            has = k32.Module32NextW(snap, ctypes.byref(me))
    finally:
        k32.CloseHandle(snap)
    return out


def probe(dll_path, label):
    """Загрузить DLL по полному пути и спросить число устройств."""
    step(label)
    dim(dll_path)
    if not os.path.isfile(dll_path):
        bad("файла нет")
        return None

    dim("версия: " + file_version(dll_path))
    try:
        # ВАЖНО: грузим по полному пути. Windows кэширует модуль по имени
        # файла, поэтому если amdhip64 уже загружен из другого места,
        # мы получим ЕГО, а не то, что просили. Скрипт это проверяет ниже.
        lib = ctypes.WinDLL(dll_path)
    except OSError as e:
        bad("не загрузилась", str(e))
        return None

    mods = dict((n.lower(), p) for n, p in loaded_modules())
    for name, path in mods.items():
        if "amdhip64" in name:
            dim("в процессе: %s -> %s" % (name, path))

    try:
        lib.hipGetDeviceCount.restype = ctypes.c_int
        lib.hipGetDeviceCount.argtypes = [ctypes.POINTER(ctypes.c_int)]
        n = ctypes.c_int(-1)
        r = lib.hipGetDeviceCount(ctypes.byref(n))
        if r == 0 and n.value > 0:
            ok("устройств: %d" % n.value)
            return True
        bad("hipGetDeviceCount -> код %d (%s)"
            % (r, HIP_ERRORS.get(r, "?")))
        return False
    except AttributeError:
        bad("нет символа hipGetDeviceCount")
        return None


def main():
    hip = DEFAULT_HIP
    for i, a in enumerate(sys.argv):
        if a == "--hip" and i + 1 < len(sys.argv):
            hip = sys.argv[i + 1]

    bindir = os.path.join(hip, "bin")
    sdk_hip = os.path.join(bindir, "amdhip64_6.dll")
    sys_hip = os.path.join(os.environ.get("WINDIR", r"C:\Windows"),
                           "System32", "amdhip64.dll")

    hr("КАКОЙ amdhip64 ГРУЗИТСЯ И ВИДИТ ЛИ ОН КАРТУ")
    dim("Python: " + sys.version.split()[0])
    dim("HIP SDK: " + hip)

    # ---------------------------------------------------------------
    step("1. Какие amdhip64 есть на диске")
    found = []
    for p in (sdk_hip, sys_hip):
        if os.path.isfile(p):
            found.append(p)
            ok(p, "версия " + file_version(p))
        else:
            warn("нет: " + p)
    if not found:
        bad("не нашёл ни одного amdhip64")
        return 2

    # ---------------------------------------------------------------
    step("2. Два разных рантайма - проверяем КАЖДЫЙ отдельно")
    #  Ключевой момент: их нельзя проверить в одном процессе. Windows
    #  загружает модуль по имени файла один раз, и второй WinDLL вернёт
    #  уже загруженный. Поэтому запускаем по дочернему процессу.
    import subprocess

    results = {}
    for p in found:
        code = (
            "import ctypes;"
            "lib=ctypes.WinDLL(r'%s');"
            "lib.hipGetDeviceCount.restype=ctypes.c_int;"
            "n=ctypes.c_int(-1);"
            "r=lib.hipGetDeviceCount(ctypes.byref(n));"
            "print('RESULT',r,n.value)"
        ) % p
        try:
            pr = subprocess.run([sys.executable, "-c", code],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
            out = pr.stdout.decode("utf-8", "replace")
        except OSError as e:
            out = str(e)
        hit = [l for l in out.splitlines() if l.startswith("RESULT")]
        if hit:
            _, r, n = hit[0].split()
            results[p] = (int(r), int(n))
        else:
            results[p] = None
            dim("вывод: " + out.strip()[:200])

    for p, res in results.items():
        name = os.path.basename(p)
        if res is None:
            bad(name + ": не удалось проверить")
        elif res[0] == 0 and res[1] > 0:
            ok("%s: устройств %d" % (name, res[1]))
        else:
            bad("%s: код %d, устройств %d" % (name, res[0], res[1]))

    # ---------------------------------------------------------------
    hr("ВЫВОД")

    sdk_res = results.get(sdk_hip)
    sys_res = results.get(sys_hip)

    sdk_good = sdk_res is not None and sdk_res[0] == 0 and sdk_res[1] > 0
    sys_good = sys_res is not None and sys_res[0] == 0 and sys_res[1] > 0

    if sys_good and not sdk_good:
        print("  amdhip64.dll от драйвера (System32) видит карту,")
        print("  amdhip64_6.dll из HIP SDK 6.4 - НЕТ.")
        print()
        dim("Это и есть причина, почему rocblas_create_handle падает:")
        dim("rocBLAS берёт тот рантайм, который найдёт первым, а найденный")
        dim("рантайм устройств не видит.")
        print()
        print("  Дальше два пути:")
        dim("  A. Заставить rocBLAS грузить System32\\amdhip64.dll.")
        dim("     Проверка: запуск с PATH, где System32 раньше bin SDK.")
        dim("  B. Починить сам HIP SDK (amdhip64_6.dll не видит карту).")
        print()
        dim("Проверить путь A можно так (в текущей сессии PowerShell):")
        dim('  $env:PATH = "C:\\Windows\\System32;" + ($env:PATH -split ";" |')
        dim('    Where-Object { $_ -notlike "*ROCm*bin*" }) -join ";"')
        dim("  затем снова запустить этот скрипт")
    elif sdk_good and not sys_good:
        print("  amdhip64_6.dll из HIP SDK видит карту, а System32 - нет.")
        dim("Значит проблема была в другом. Пришлите вывод.")
    elif sdk_good and sys_good:
        print("  Оба рантайма видят карту.")
        dim("Тогда hipGetDevice=100 в прошлой диагностике был вызван")
        dim("загруженным в процесс модулем - надо смотреть, какой именно.")
        dim("Пришлите вывод раздела 1 и 2.")
    else:
        print("  Ни один рантайм карту не видит.")
        dim("Это означает проблему на уровне драйвера или доступа к GPU.")
        dim("Пришлите вывод - дальше будем смотреть драйвер.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
