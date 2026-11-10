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
        lines.append("patched %d IAT slot(s)" % len(self.slots))
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
        if not self.slots:
            lines.append("  scanned modules:")
            for name in self.scanned:
                lines.append("    %s" % name)
        return "\n".join(lines)


def _kernel32():
    return ctypes.WinDLL("kernel32", use_last_error=True)


def _psapi():
    return ctypes.WinDLL("psapi", use_last_error=True)


def _raise(message: str) -> None:
    raise PatchError("%s (GetLastError=%d)" % (message, ctypes.get_last_error()))


def resolve_crt_allocators() -> Tuple[int, int]:
    """Адреса ``_aligned_malloc`` и ``_aligned_free`` в ``ucrtbase.dll``."""
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


def loaded_modules() -> List[Tuple[int, str]]:
    """Список ``(base, path)`` для всех загруженных в процесс модулей."""
    kernel32 = _kernel32()
    psapi = _psapi()

    psapi.EnumProcessModulesEx.restype = ctypes.c_int
    psapi.EnumProcessModulesEx.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_uint32,
    ]
    kernel32.GetModuleFileNameExW.restype = ctypes.c_uint32
    kernel32.GetModuleFileNameExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32]

    needed = ctypes.c_uint32(0)
    capacity = 1024
    handles = (ctypes.c_void_p * capacity)()
    ok = psapi.EnumProcessModulesEx(
        None, handles, ctypes.sizeof(handles), ctypes.byref(needed), LIST_MODULES_ALL
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
        length = kernel32.GetModuleFileNameExW(None, handle, buffer, 1024)
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


def write_pointer(address: int, value: int) -> int:
    """Пишет 8-байтный указатель, на время снимая защиту страницы.

    Возвращает прежнее значение.
    """
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
            write_pointer(slot.slot_address, slot.original)
        self._undone = True

    def __del__(self):  # pragma: no cover - best effort
        try:
            self.undo()
        except Exception:
            pass


def install(
    extra_dirs: Sequence[str] = (),
    functions: Sequence[str] = TARGET_FUNCTIONS,
    dry_run: bool = False,
) -> HostAllocPatch:
    """Устанавливает патч. Бросает :class:`PatchError`, если не нашлось ни одного слота."""
    if os.name != "nt":
        raise PatchError("this patch only applies to Windows")

    aligned_malloc, aligned_free = resolve_crt_allocators()
    page = ThunkPage(aligned_malloc, aligned_free)

    report = PatchReport()
    report.notes.append(
        "_aligned_malloc=0x%x _aligned_free=0x%x thunks at 0x%x" % (aligned_malloc, aligned_free, page.base)
    )

    candidates: Dict[int, str] = {}
    scan_all = os.environ.get("ZLUDA_RX580_SCAN_ALL", "").strip() not in ("", "0", "false", "False")
    for base, path in loaded_modules():
        if not path:
            continue
        lowered = path.lower()
        if not lowered.endswith((".dll", ".pyd")):
            continue
        if scan_all or "torch" in lowered or "\\nvidia\\" in lowered:
            candidates[base] = path
    if scan_all:
        report.notes.append("ZLUDA_RX580_SCAN_ALL=1 — scanning every loaded module")

    if not candidates:
        report.notes.append("no torch/nvidia modules are loaded in this process")
        raise PatchError("nothing to patch; import torch first")

    for base, path in sorted(candidates.items(), key=lambda item: item[1].lower()):
        report.scanned.append(os.path.basename(path))
        slots = find_target_slots(path, base, functions)
        if not slots:
            report.skipped.append(os.path.basename(path))
            continue
        for dll, function, address, delay_loaded in slots:
            new_value = page.host_alloc if function == "cudaHostAlloc" else page.host_free
            if dry_run:
                report.slots.append(
                    PatchedSlot(path, base, dll, function, address, delay_loaded, 0)
                )
                continue
            previous = write_pointer(address, new_value)
            report.slots.append(
                PatchedSlot(path, base, dll, function, address, delay_loaded, previous)
            )

    if not report.slots:
        report.notes.append(
            "none of the loaded modules import %s from cudart* — nothing to patch"
            % "/".join(functions)
        )
        raise PatchError("no cudaHostAlloc/cudaFreeHost import slots found")

    return HostAllocPatch(report, page)
