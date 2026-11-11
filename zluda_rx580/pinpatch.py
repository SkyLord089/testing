"""Замена ``cudaHostAlloc`` / ``cudaFreeHost`` в уже загруженных DLL.

Только для Windows x86-64. Модуль ничего не делает при импорте — все действия
выполняются явно через :func:`install`.

Как это работает
----------------
``torch_cuda.dll`` (и, возможно, ``c10_cuda.dll``) импортирует
``cudaHostAlloc`` из ``cudart64_110.dll``. Windows-загрузчик пишет адрес
настоящей функции в слот таблицы адресов импорта (IAT) внутри образа torch.
Мы переписываем этот слот на адрес своей заглушки — файлы на диске не
меняются, меняется только память процесса.

Для каждого слота нужен адрес в памяти, поэтому:

1. находим ``_aligned_malloc`` / ``_aligned_free`` в ``ucrtbase.dll``;
2. выделяем исполняемую страницу и кладём туда машинный код заглушек
   (:mod:`zluda_rx580.thunks`);
3. проходим по загруженным модулям ``torch``, читаем их PE-заголовки с диска,
   находим слоты IAT для ``cudart64_*.dll!cudaHostAlloc`` / ``!cudaFreeHost``;
4. ``VirtualProtect`` -> запись указателя -> восстановление защиты.

Если ``_aligned_malloc`` или ``_aligned_free`` не найдены, патч **не
применяется** и возвращается ошибка: молча выравнивать память по-другому
нельзя, иначе PyTorch получит неверно выровненные тензоры.
"""

from __future__ import annotations

import ctypes
import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

from . import pe as pe_mod
from .thunks import build_host_alloc_thunk, build_host_free_thunk

PAGE_EXECUTE_READWRITE = 0x40
PAGE_EXECUTE_READ = 0x20
PAGE_READWRITE = 0x04

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000

PROCESS_QUERY_INFORMATION = 0x0400
LIST_MODULES_ALL = 0x03

TARGET_FUNCTIONS = ("cudaHostAlloc", "cudaFreeHost")
RUNTIME_DLL_PREFIXES = ("cudart", "cudart64", "cudart32")


class PatchError(RuntimeError):
    pass


@dataclass
class PatchedSlot:
    module_path: str
    module_base: int
    dll: str
    function: str
    slot_address: int
    delay_loaded: bool
    original: int


@dataclass
class PatchReport:
    slots: List[PatchedSlot] = field(default_factory=list)
    scanned: List[str] = field(default_factory=list)
    skipped: List[str] = field(default_factory=list)
    notes: List[str] = field(default_factory=list)

    @property
    def applied(self) -> bool:
        return bool(self.slots)

    def format(self) -> str:
        lines = []
        lines.append("IAT slots for %s: %d" % ("/".join(TARGET_FUNCTIONS), len(self.slots)))
        for slot in self.slots:
            lines.append(
                "  %s!%s  slot=0x%x  was=0x%x  %s"
                % (
                    os.path.basename(slot.module_path),
                    slot.function,
                    slot.slot_address,
                    slot.original,
                    "(delay-load)" if slot.delay_loaded else "",
                )
            )
        for note in self.notes:
            lines.append("  note: %s" % note)
        if self.scanned:
            lines.append("  scanned %d module(s):" % len(self.scanned))
            for name in self.scanned:
                mark = "" if name in self.skipped else "  <- слот найден"
                lines.append("    %s%s" % (name, mark))
        return "\n".join(lines)


def _kernel32():
    return ctypes.WinDLL("kernel32", use_last_error=True)


def _psapi():
    return ctypes.WinDLL("psapi", use_last_error=True)


def _raise(message: str) -> None:
    raise PatchError("%s (GetLastError=%d)" % (message, ctypes.get_last_error()))


def resolve_crt_allocators() -> Tuple[int, int]:
    """Адреса ``_aligned_malloc`` и ``_aligned_free`` в ``ucrtbase.dll``."""
    require_windows()
    kernel32 = _kernel32()
    kernel32.LoadLibraryW.restype = ctypes.c_void_p
    kernel32.LoadLibraryW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetProcAddress.restype = ctypes.c_void_p
    kernel32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    handle = kernel32.LoadLibraryW("ucrtbase.dll")
    if not handle:
        _raise("LoadLibraryW('ucrtbase.dll') failed")
    aligned_malloc = kernel32.GetProcAddress(handle, b"_aligned_malloc")
    aligned_free = kernel32.GetProcAddress(handle, b"_aligned_free")
    if not aligned_malloc or not aligned_free:
        raise PatchError(
            "ucrtbase.dll does not export _aligned_malloc/_aligned_free "
            "(malloc=0x%x free=0x%x) — refusing to patch" % (aligned_malloc or 0, aligned_free or 0)
        )
    return int(aligned_malloc), int(aligned_free)


class ThunkPage:
    """Страница с машинным кодом заглушек."""

    def __init__(self, aligned_malloc: int, aligned_free: int):
        require_windows()
        kernel32 = _kernel32()
        kernel32.VirtualAlloc.restype = ctypes.c_void_p
        kernel32.VirtualAlloc.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]
        kernel32.VirtualProtect.restype = ctypes.c_int
        kernel32.VirtualProtect.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
        ]

        self.alloc_code = build_host_alloc_thunk(aligned_malloc)
        self.free_code = build_host_free_thunk(aligned_free)
        blob = self.alloc_code + self.free_code

        size = (len(blob) + 0xFFF) & ~0xFFF
        base = kernel32.VirtualAlloc(None, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
        if not base:
            _raise("VirtualAlloc failed")
        self.base = int(base)
        self.size = size
        ctypes.memmove(self.base, blob, len(blob))

        old = ctypes.c_uint32(0)
        if not kernel32.VirtualProtect(self.base, size, PAGE_EXECUTE_READ, ctypes.byref(old)):
            _raise("VirtualProtect(EXECUTE_READ) failed")
        self._flush_instruction_cache()

    @property
    def host_alloc(self) -> int:
        return self.base

    @property
    def host_free(self) -> int:
        return self.base + len(self.alloc_code)

    def _flush_instruction_cache(self) -> None:
        kernel32 = _kernel32()
        if hasattr(kernel32, "FlushInstructionCache"):
            kernel32.FlushInstructionCache.restype = ctypes.c_int
            kernel32.FlushInstructionCache.argtypes = [
                ctypes.c_void_p,
                ctypes.c_void_p,
                ctypes.c_size_t,
            ]
            kernel32.FlushInstructionCache(
                ctypes.c_void_p(-1), ctypes.c_void_p(self.base), self.size
            )


# Какой DLL экспортирует нужную функцию. Ошибка здесь стоит дорого:
# GetModuleFileNameExW живёт в psapi.dll, а в kernel32.dll есть только
# K32GetModuleFileNameExW, поэтому брать её из kernel32 нельзя.
PSAPI_FUNCTIONS = ("EnumProcessModulesEx", "GetModuleFileNameExW")


def require_windows() -> None:
    """Чистая ошибка вместо AttributeError из недр ctypes на не-Windows."""
    if os.name != "nt":
        raise PatchError("Windows-only: this platform is %r" % (os.name,))


# EnumProcessModulesEx и GetModuleFileNameExW требуют валидный дескриптор
# процесса: NULL не принимается и даёт GetLastError=6 (ERROR_INVALID_HANDLE).
# Обе функции есть и в psapi.dll, и в kernel32.dll под именами с префиксом K32,
# поэтому пробуем оба варианта.
PROCESS_MODULE_FUNCTIONS = (
    ("EnumProcessModulesEx", (("kernel32", "K32EnumProcessModulesEx"), ("psapi", "EnumProcessModulesEx"))),
    ("GetModuleFileNameExW", (("kernel32", "K32GetModuleFileNameExW"), ("psapi", "GetModuleFileNameExW"))),
)


def current_process() -> int:
    """Псевдодескриптор текущего процесса — то, что ждут Process Status API."""
    require_windows()
    kernel32 = _kernel32()
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    kernel32.GetCurrentProcess.argtypes = []
    handle = kernel32.GetCurrentProcess()
    if not handle:
        _raise("GetCurrentProcess failed")
    return int(handle)


def _resolve_process_api(name: str):
    """Возвращает функцию из первой DLL, которая её экспортирует."""
    candidates = PROCESS_MODULE_FUNCTIONS_DICT.get(name)
    if not candidates:
        raise PatchError("no DLL mapping declared for %r" % (name,))
    errors = []
    for dll_name, export in candidates:
        try:
            library = _kernel32() if dll_name == "kernel32" else _psapi()
            function = getattr(library, export)
        except (AttributeError, OSError) as exc:
            errors.append("%s!%s: %s" % (dll_name, export, exc))
            continue
        return function
    raise PatchError("cannot resolve %s (%s)" % (name, "; ".join(errors)))


PROCESS_MODULE_FUNCTIONS_DICT = dict(PROCESS_MODULE_FUNCTIONS)


def loaded_modules() -> List[Tuple[int, str]]:
    """Список ``(base, path)`` для всех загруженных в процесс модулей."""
    require_windows()

    enum_modules = _resolve_process_api("EnumProcessModulesEx")
    enum_modules.restype = ctypes.c_int
    enum_modules.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_uint32,
    ]
    module_file_name = _resolve_process_api("GetModuleFileNameExW")
    module_file_name.restype = ctypes.c_uint32
    module_file_name.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.c_uint32,
    ]

    process = current_process()
    needed = ctypes.c_uint32(0)
    capacity = 1024
    handles = (ctypes.c_void_p * capacity)()
    ok = enum_modules(
        process, handles, ctypes.sizeof(handles), ctypes.byref(needed), LIST_MODULES_ALL
    )
    if not ok:
        _raise("EnumProcessModulesEx failed")
    count = min(capacity, needed.value // ctypes.sizeof(ctypes.c_void_p))

    modules = []
    buffer = ctypes.create_unicode_buffer(1024)
    for i in range(count):
        handle = handles[i]
        if not handle:
            continue
        length = module_file_name(process, handle, buffer, 1024)
        path = buffer.value[:length] if length else ""
        modules.append((int(handle), path))
    return modules


def torch_module_paths(extra_dirs: Sequence[str] = ()) -> List[str]:
    """Каталоги, в которых лежат бинарники PyTorch."""
    dirs = set(extra_dirs)
    try:
        import torch  # noqa: F401  (импортируется лениво, только на Windows)

        torch_root = os.path.dirname(os.path.abspath(torch.__file__))
        dirs.add(torch_root)
        dirs.add(os.path.join(torch_root, "lib"))
    except Exception as exc:  # pragma: no cover - Windows-only path
        raise PatchError("torch is not importable: %r" % (exc,))
    return sorted(dirs)


def _wants_dll(dll_name: str) -> bool:
    stem = dll_name.lower()
    return any(stem.startswith(prefix) for prefix in RUNTIME_DLL_PREFIXES)


def find_target_slots(
    module_path: str, module_base: int, functions: Sequence[str] = TARGET_FUNCTIONS
) -> List[Tuple[str, str, int, bool]]:
    """Ищет в PE-файле слоты IAT для ``cudart*!functions``.

    Возвращает список ``(dll, function, slot_address, delay_loaded)``.
    """
    try:
        with open(module_path, "rb") as handle:
            data = handle.read()
        image = pe_mod.PEImage(data, module_path)
    except (OSError, pe_mod.PEFormatError):
        return []
    if not image.is_pe32_plus:
        return []

    found = []
    for descriptor in image.all_import_descriptors():
        if not _wants_dll(descriptor.dll):
            continue
        for slot in descriptor.slots:
            if slot.name in functions:
                found.append(
                    (descriptor.dll, slot.name, module_base + slot.iat_rva, descriptor.delay_loaded)
                )
    return found


# Имена, по которым PyTorch добирается до pinned-памяти. Если имя лежит в
# файле, но не в таблице импорта - его получают через GetProcAddress, и тогда
# подмена IAT невозможна в принципе.
HOST_ALLOC_NAMES = (
    "cudaHostAlloc",
    "cudaFreeHost",
    "cudaMallocHost",
    "cudaHostRegister",
    "cuMemHostAlloc",
    "cuMemAllocHost",
    "cuMemFreeHost",
)


@dataclass
class ImportSummary:
    module_path: str
    parsed: bool = False
    is_pe32_plus: bool = False
    import_dlls: List[str] = field(default_factory=list)
    delay_dlls: List[str] = field(default_factory=list)
    iat_matches: List[str] = field(default_factory=list)
    raw_name_hits: List[str] = field(default_factory=list)
    error: str = ""

    @property
    def descriptor_count(self) -> int:
        return len(self.import_dlls) + len(self.delay_dlls)

    def verdict(self) -> str:
        """Что означает сочетание признаков."""
        if os.path.basename(self.module_path).lower().startswith(RUNTIME_DLL_PREFIXES):
            # cudart сам экспортирует эти функции, поэтому их имена лежат в
            # файле и без записей в таблице импорта. Это не динамическое
            # разрешение, и слоты IAT здесь не нужны.
            return "это сама cudart: она экспортирует эти функции, слоты IAT тут не при чём"
        if not self.parsed:
            return "PE не разобран (%s) - по этому модулю выводов нет" % (self.error or "?")
        if self.iat_matches:
            return "есть слоты IAT: %s" % ", ".join(self.iat_matches)
        if self.raw_name_hits:
            return (
                "имена %s в файле есть, но в таблице импорта их нет - "
                "значит их получают через GetProcAddress, подмена IAT не сработает"
                % "/".join(self.raw_name_hits)
            )
        if not self.import_dlls:
            return "таблица импорта пуста - разбор скорее всего не удался"
        return "модуль не обращается к pinned-памяти"


def import_summary(module_path: str, names: Sequence[str] = HOST_ALLOC_NAMES) -> ImportSummary:
    """Читает PE и отвечает: откуда модуль берёт функции pinned-памяти.

    Отдельно ищет имена в сырых байтах файла. Имя в файле без записи в IAT
    означает динамическое разрешение через ``GetProcAddress``.
    """
    summary = ImportSummary(module_path)
    try:
        with open(module_path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        summary.error = str(exc)
        return summary

    for name in names:
        if name.encode("ascii") in data:
            summary.raw_name_hits.append(name)

    try:
        image = pe_mod.PEImage(data, module_path)
    except pe_mod.PEFormatError as exc:
        summary.error = str(exc)
        return summary

    summary.parsed = True
    summary.is_pe32_plus = image.is_pe32_plus

    for descriptor in image.import_descriptors():
        summary.import_dlls.append(descriptor.dll)
        for slot in descriptor.slots:
            if slot.name in names:
                summary.iat_matches.append("%s!%s" % (descriptor.dll, slot.name))
    for descriptor in image.delay_import_descriptors():
        summary.delay_dlls.append(descriptor.dll)
        for slot in descriptor.slots:
            if slot.name in names:
                summary.iat_matches.append("%s!%s (delay)" % (descriptor.dll, slot.name))
    return summary


def write_pointer(address: int, value: int) -> int:
    """Пишет 8-байтный указатель, на время снимая защиту страницы.

    Возвращает прежнее значение.
    """
    require_windows()
    kernel32 = _kernel32()
    kernel32.VirtualProtect.restype = ctypes.c_int
    kernel32.VirtualProtect.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
    ]

    old = ctypes.c_uint32(0)
    if not kernel32.VirtualProtect(
        ctypes.c_void_p(address), ctypes.c_size_t(8), PAGE_READWRITE, ctypes.byref(old)
    ):
        _raise("VirtualProtect(RW) failed for 0x%x" % address)
    try:
        previous = int(ctypes.cast(address, ctypes.POINTER(ctypes.c_uint64)).contents.value)
        ctypes.cast(address, ctypes.POINTER(ctypes.c_uint64)).contents.value = value
    finally:
        restored = ctypes.c_uint32(0)
        kernel32.VirtualProtect(
            ctypes.c_void_p(address), ctypes.c_size_t(8), old.value, ctypes.byref(restored)
        )
    return previous


class HostAllocPatch:
    """Установленный патч. ``undo()`` возвращает исходные значения IAT."""

    def __init__(self, report: PatchReport, page: ThunkPage):
        self.report = report
        self.page = page
        self._undone = False

    def undo(self) -> None:
        if self._undone:
            return
        for slot in self.report.slots:
            if not slot.original:
                # dry-run: прежнее значение не читали, писать в слот нельзя
                continue
            write_pointer(slot.slot_address, slot.original)
        self._undone = True

    def __del__(self):  # pragma: no cover - best effort
        try:
            self.undo()
        except Exception:
            pass


def _candidate_modules() -> Dict[int, str]:
    """Модули, в которых имеет смысл искать слоты IAT."""
    scan_all = os.environ.get("ZLUDA_RX580_SCAN_ALL", "").strip() not in ("", "0", "false", "False")
    candidates: Dict[int, str] = {}
    for base, path in loaded_modules():
        if not path:
            continue
        lowered = path.lower()
        if not lowered.endswith((".dll", ".pyd")):
            continue
        if scan_all or "torch" in lowered or "\\nvidia\\" in lowered:
            candidates[base] = path
    return candidates


def survey(
    extra_dirs: Sequence[str] = (),
    functions: Sequence[str] = TARGET_FUNCTIONS,
) -> PatchReport:
    """Читает PE-заголовки загруженных модулей и ищет слоты IAT.

    Ничего не пишет и не выделяет памяти, поэтому подходит для диагностики:
    отчёт возвращается даже тогда, когда слотов не нашлось.
    """
    if os.name != "nt":
        raise PatchError("this scan only applies to Windows")

    report = PatchReport()
    candidates = _candidate_modules()
    if os.environ.get("ZLUDA_RX580_SCAN_ALL", "").strip() not in ("", "0", "false", "False"):
        report.notes.append("ZLUDA_RX580_SCAN_ALL=1 — scanning every loaded module")
    if extra_dirs:
        report.notes.append("extra dirs: %s" % ", ".join(extra_dirs))

    if not candidates:
        report.notes.append(
            "no torch/nvidia modules are loaded in this process — import torch before scanning"
        )
        return report

    for base, path in sorted(candidates.items(), key=lambda item: item[1].lower()):
        report.scanned.append(os.path.basename(path))
        slots = find_target_slots(path, base, functions)
        if not slots:
            report.skipped.append(os.path.basename(path))
            continue
        for dll, function, address, delay_loaded in slots:
            report.slots.append(
                PatchedSlot(path, base, dll, function, address, delay_loaded, 0)
            )

    if not report.slots:
        report.notes.append(
            "none of the %d scanned module(s) import %s from cudart*"
            % (len(report.scanned), "/".join(functions))
        )
    return report


def install(
    extra_dirs: Sequence[str] = (),
    functions: Sequence[str] = TARGET_FUNCTIONS,
    dry_run: bool = False,
) -> HostAllocPatch:
    """Устанавливает патч. Бросает :class:`PatchError`, если не нашлось ни одного слота."""
    if os.name != "nt":
        raise PatchError("this patch only applies to Windows")

    report = survey(extra_dirs, functions)
    if not report.scanned:
        raise PatchError("nothing to patch; import torch first")

    aligned_malloc, aligned_free = resolve_crt_allocators()
    page = ThunkPage(aligned_malloc, aligned_free)
    report.notes.insert(
        0,
        "_aligned_malloc=0x%x _aligned_free=0x%x thunks at 0x%x" % (aligned_malloc, aligned_free, page.base),
    )

    if dry_run:
        return HostAllocPatch(report, page)

    if not report.slots:
        raise PatchError("no cudaHostAlloc/cudaFreeHost import slots found")

    patched: List[PatchedSlot] = []
    for slot in report.slots:
        new_value = page.host_alloc if slot.function == "cudaHostAlloc" else page.host_free
        slot.original = write_pointer(slot.slot_address, new_value)
        patched.append(slot)
    report.slots = patched

    return HostAllocPatch(report, page)
