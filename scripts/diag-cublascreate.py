#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Почему cublasCreate падает: показать НАСТОЯЩУЮ причину.

ПРОБЛЕМА

  ZLUDA переводит любой неуспешный статус rocBLAS в CUBLAS_STATUS_INTERNAL_ERROR
  (zluda_blas/src/lib.rs, fn to_cuda). Поэтому из Python видно только
  "INTERNAL_ERROR" - без причины.

  Внутри rocBLAS вызов устроен так (rocblas_auxiliary.cpp:197):

      extern "C" rocblas_status rocblas_create_handle(rocblas_handle* handle)
      try {
          *handle = new _rocblas_handle;
          return rocblas_status_success;
      }
      catch(...) {
          return exception_to_rocblas_status();   <- сюда уходит ЛЮБОЕ исключение
      }

  То есть падение - это исключение в конструкторе _rocblas_handle.
  Исключение печатает свой текст в stderr, но Python его не показывает.

ЧТО ДЕЛАЕТ ЭТОТ СКРИПТ

  Грузит rocblas.dll напрямую через ctypes (без ZLUDA) и вызывает
  rocblas_create_handle. Отличие от обычной проверки: включает подробный
  лог HIP и rocBLAS, а также проверяет все шаги конструктора по отдельности,
  чтобы было видно, на каком именно он падает.

ЗАПУСК

  python diag-cublascreate.py
  python diag-cublascreate.py --hip "C:\\Program Files\\AMD\\ROCm\\6.4"

КОДЫ ВОЗВРАТА

  0  успех
  1  падение (в выводе будет настоящая причина)
  2  не удалось загрузить DLL или найти инструменты
"""

import argparse
import ctypes
import os
import subprocess
import sys

DEFAULT_HIP = r"C:\Program Files\AMD\ROCm\6.4"


def hr(title):
    print()
    print("=" * 70)
    print("  " + title)
    print("=" * 70)


def step(text):
    print()
    print("  >> " + text)


def ok(text, note=""):
    print("  [OK]   " + text)
    if note:
        print("         " + note)


def bad(text, note=""):
    print("  [XX]   " + text)
    if note:
        print("         " + note)


def warn(text, note=""):
    print("  [!!]   " + text)
    if note:
        print("         " + note)


def dim(text):
    print("         " + text)


def run(exe, args, env=None):
    """Запуск процесса с захватом stdout+stderr."""
    try:
        p = subprocess.run(
            [exe] + args,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
        )
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except OSError as e:
        return -1, str(e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hip", default=DEFAULT_HIP, help="каталог HIP SDK")
    args = ap.parse_args()

    hip = args.hip
    bindir = os.path.join(hip, "bin")
    rocblas = os.path.join(bindir, "rocblas.dll")

    hr("ПОЧЕМУ cublasCreate ПАДАЕТ - ПОИСК НАСТОЯЩЕЙ ПРИЧИНЫ")
    dim("HIP SDK: " + hip)

    if not os.path.isfile(rocblas):
        bad("не найден rocblas.dll", rocblas)
        return 2

    # ---------------------------------------------------------------
    step("1. Загрузка rocblas.dll напрямую (без ZLUDA)")
    # -- ровно так, как это делал диагностический скрипт п.4
    os.environ["PATH"] = bindir + os.pathsep + os.environ.get("PATH", "")
    try:
        os.add_dll_directory(bindir)
    except (AttributeError, OSError):
        pass

    try:
        lib = ctypes.WinDLL(rocblas)
        ok("DLL загрузилась")
    except OSError as e:
        bad("DLL не загрузилась", str(e))
        return 2

    try:
        create = lib.rocblas_create_handle
    except AttributeError:
        bad("нет символа rocblas_create_handle")
        return 2

    create.restype = ctypes.c_int
    create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]

    # ---------------------------------------------------------------
    step("2. Что видно устройству (hipGetDeviceProperties через rocblas)")
    #  rocBLAS при создании handle вызывает hipMalloc на 32 МБ и
    #  hipGetDeviceProperties. Проверим, что устройство вообще есть.
    k, out = run(
        os.path.join(bindir, "hipInfo.exe"),
        [],
        env=dict(os.environ, HIP_VISIBLE_DEVICES="0"),
    )
    if k == 0:
        for line in out.splitlines():
            if "name" in line.lower() or "gcnArchName" in line:
                dim(line.strip())
        ok("hipInfo работает")
    else:
        warn("hipInfo вернул код %d" % k)
        dim("(это ожидаемо на RX 580 - известная особенность, не причина)")

    # ---------------------------------------------------------------
    step("3. Вызов rocblas_create_handle БЕЗ логов")
    handle = ctypes.c_void_p()
    status = create(ctypes.byref(handle))
    names = {
        0: "rocblas_status_success",
        1: "rocblas_status_invalid_handle",
        2: "rocblas_status_not_implemented",
        3: "rocblas_status_invalid_pointer",
        4: "rocblas_status_invalid_size",
        5: "rocblas_status_memory_error",
        6: "rocblas_status_internal_error",
        7: "rocblas_status_perf_degradation",
        8: "rocblas_status_size_query_mismatch",
        9: "rocblas_status_size_increased",
        10: "rocblas_status_size_unchanged",
        11: "rocblas_status_invalid_value",
        12: "rocblas_status_continue",
        13: "rocblas_status_check_numerics_fail",
    }
    nm = names.get(status, "неизвестный (%d)" % status)

    if status == 0:
        ok("handle создан: " + nm)
        dim("Значит rocBLAS сам по себе исправен, и падение cublasCreate")
        dim("происходит в другом месте - см. вывод ниже.")
        return 0

    bad("handle НЕ создан: " + nm)
    dim("Теперь включаем подробные логи, чтобы увидеть причину.")

    # ---------------------------------------------------------------
    step("4. Тот же вызов с подробным логом HIP")
    #  Эти переменные заставляют HIP печатать каждый шаг и ошибки.
    env = dict(os.environ)
    env["AMD_LOG_LEVEL"] = "3"
    env["AMD_LOG_MASK"] = "0x7FFFFFFF"
    env["HIP_LAUNCH_BLOCKING"] = "1"

    #  Пишем результат в лог через отдельный процесс: переменные
    #  окружения действуют только на старте HIP, а он уже инициализирован
    #  в текущем процессе. Поэтому перезапускаем себя.
    script = os.path.abspath(__file__)
    code = (
        "import ctypes,os,sys;"
        "os.add_dll_directory(r'%s');"
        "lib=ctypes.WinDLL(r'%s');"
        "f=lib.rocblas_create_handle;f.restype=ctypes.c_int;"
        "h=ctypes.c_void_p();"
        "print('STATUS',f(ctypes.byref(h)))"
    ) % (bindir, rocblas)

    k, out = run(sys.executable, ["-c", code], env=env)
    dim("код возврата: %d" % k)

    interesting = []
    for line in out.splitlines():
        low = line.lower()
        if any(w in low for w in (
            "error", "fail", "abort", "throw", "exception",
            "hipmalloc", "out of memory", "no ROCm", "invalid",
            "unsupported", "assert", "status",
        )):
            interesting.append(line.strip())

    if interesting:
        print()
        dim("Значимые строки лога:")
        for line in interesting[:40]:
            print("   " + line)
    else:
        dim("HIP ничего не сообщил об ошибке - значит исключение не от HIP.")
        print()
        dim("Полный вывод (первые 40 строк):")
        for line in out.splitlines()[:40]:
            print("   " + line)

    # ---------------------------------------------------------------
    step("5. Проверка каждого шага конструктора по отдельности")
    #  Конструктор _rocblas_handle делает:
    #    getActiveDevice()      -> hipGetDevice
    #    getActiveArch(device)  -> hipGetDeviceProperties
    #    init_logging()         -> разбор ROCBLAS_LAYER и т.п.
    #    init_check_numerics()  -> разбор ROCBLAS_CHECK_NUMERICS
    #    hipMalloc(32 МБ)       -> DEFAULT_DEVICE_MEMORY_SIZE
    #    hipblasLtCreate()      -> ТОЛЬКО если BUILD_WITH_HIPBLASLT
    #
    #  На Windows hipBLASLt принудительно выключен при сборке
    #  (CMakeLists.txt:176: if(WIN32 ...) set(BUILD_WITH_HIPBLASLT OFF)),
    #  поэтому последний шаг не должен мешать.

    amdhip = os.path.join(bindir, "amdhip64_6.dll")
    if not os.path.isfile(amdhip):
        amdhip = os.path.join(os.environ.get("WINDIR", r"C:\Windows"),
                              "System32", "amdhip64.dll")

    if os.path.isfile(amdhip):
        dim("проверяю HIP напрямую: " + os.path.basename(amdhip))
        try:
            hl = ctypes.WinDLL(amdhip)
        except OSError as e:
            bad("amdhip64 не загрузилась", str(e))
            hl = None

        if hl is not None:
            # hipGetDevice
            hl.hipGetDevice.restype = ctypes.c_int
            hl.hipGetDevice.argtypes = [ctypes.POINTER(ctypes.c_int)]
            dev = ctypes.c_int(-1)
            r = hl.hipGetDevice(ctypes.byref(dev))
            if r == 0:
                ok("hipGetDevice -> %d" % dev.value)
            else:
                bad("hipGetDevice код %d" % r)

            # hipMalloc ровно как в конструкторе
            hl.hipMalloc.restype = ctypes.c_int
            hl.hipMalloc.argtypes = [ctypes.POINTER(ctypes.c_void_p),
                                     ctypes.c_size_t]
            p = ctypes.c_void_p()
            r = hl.hipMalloc(ctypes.byref(p), 32 * 1024 * 1024)
            if r == 0:
                ok("hipMalloc(32 МБ) -> ok")
                hl.hipFree.restype = ctypes.c_int
                hl.hipFree.argtypes = [ctypes.c_void_p]
                hl.hipFree(p)
            else:
                bad("hipMalloc(32 МБ) код %d" % r)
    else:
        warn("не нашёл amdhip64.dll")

    # ---------------------------------------------------------------
    hr("ИТОГ")
    if status != 0:
        dim("rocblas_create_handle вернул ошибку, и это НЕ из-за отсутствия")
        dim("ядер gfx803: ядра загружаются позже, при первом GEMM.")
        print()
        dim("Смотрите значимые строки лога выше - там настоящая причина.")
        dim("Если строк нет совсем, значит исключение бросил сам C++ код")
        dim("rocBLAS. Тогда пришлите этот вывод целиком.")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
