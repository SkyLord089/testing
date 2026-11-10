"""Минимальный парсер PE/COFF — только то, что нужно для поиска таблицы импорта.

Сторонних зависимостей нет (pefile не требуется): модуль работает на чистом
``struct`` и читает файл целиком в память.

Нужны две вещи:

* каталог импорта (``IMAGE_DIRECTORY_ENTRY_IMPORT``) — обычная таблица импорта;
* каталог отложенного импорта (``IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT``).

Для каждого импортируемого символа возвращаем **RVA слота в IAT** — именно туда
загрузчик Windows пишет адрес реальной функции и именно туда мы пишем адрес
своей заглушки.

Работает и с 64-битными (PE32+), и с 32-битными (PE32) образами.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterator, List, Optional

DOS_MAGIC = b"MZ"
NT_SIGNATURE = 0x00004550

IMAGE_FILE_MACHINE_I386 = 0x014C
IMAGE_FILE_MACHINE_AMD64 = 0x8664

OPTIONAL_HEADER_MAGIC_PE32 = 0x10B
OPTIONAL_HEADER_MAGIC_PE32_PLUS = 0x20B

# IMAGE_DIRECTORY_ENTRY_*
DIR_IMPORT = 1
DIR_DELAY_IMPORT = 13

IMAGE_ORDINAL_FLAG32 = 0x80000000
IMAGE_ORDINAL_FLAG64 = 0x8000000000000000

# IMAGE_DELAYLOAD_DESCRIPTOR.Attributes
DELAYLOAD_RVA_BASED = 1


class PEFormatError(ValueError):
    """Файл не является корректным PE-образом."""


@dataclass(frozen=True)
class ImportSlot:
    """Один импортируемый символ.

    ``name`` — имя символа (``None`` при импорте по ординалу),
    ``ordinal`` — ординал (``None`` при импорте по имени),
    ``iat_rva`` — RVA слота в таблице адресов импорта.
    """

    name: Optional[str]
    ordinal: Optional[int]
    iat_rva: int

    @property
    def label(self) -> str:
        return self.name if self.name is not None else "ordinal#%d" % (self.ordinal or 0)


@dataclass(frozen=True)
class ImportDescriptor:
    """Одна запись в каталоге импорта (одна импортируемая DLL)."""

    dll: str
    iat_rva: int
    delay_loaded: bool
    slots: List[ImportSlot] = field(default_factory=list)

    def find(self, name: str) -> Optional[ImportSlot]:
        for slot in self.slots:
            if slot.name == name:
                return slot
        return None


@dataclass(frozen=True)
class Section:
    name: str
    virtual_address: int
    virtual_size: int
    size_of_raw_data: int
    pointer_to_raw_data: int


class PEImage:
    """Разобранный заголовок PE-образа."""

    def __init__(self, data: bytes, source: str = "<memory>"):
        self.data = data
        self.source = source
        if len(data) < 0x40 or data[:2] != DOS_MAGIC:
            raise PEFormatError("%s: no MZ signature" % source)

        (e_lfanew,) = struct.unpack_from("<I", data, 0x3C)
        if e_lfanew + 24 > len(data):
            raise PEFormatError("%s: e_lfanew out of range" % source)
        (signature,) = struct.unpack_from("<I", data, e_lfanew)
        if signature != NT_SIGNATURE:
            raise PEFormatError("%s: no PE signature" % source)

        coff = e_lfanew + 4
        (
            self.machine,
            self.number_of_sections,
            _ts,
            _sym_ptr,
            _sym_count,
            self.size_of_optional_header,
            self.characteristics,
        ) = struct.unpack_from("<HHIIIHH", data, coff)

        opt = coff + 20
        if opt + 2 > len(data):
            raise PEFormatError("%s: truncated optional header" % source)
        (self.optional_magic,) = struct.unpack_from("<H", data, opt)
        if self.optional_magic == OPTIONAL_HEADER_MAGIC_PE32_PLUS:
            self.is_pe32_plus = True
            self.image_base = struct.unpack_from("<Q", data, opt + 24)[0]
            dir_count_off, dir_off = opt + 108, opt + 112
            self.ordinal_flag = IMAGE_ORDINAL_FLAG64
        elif self.optional_magic == OPTIONAL_HEADER_MAGIC_PE32:
            self.is_pe32_plus = False
            self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
            dir_count_off, dir_off = opt + 92, opt + 96
            self.ordinal_flag = IMAGE_ORDINAL_FLAG32
        else:
            raise PEFormatError("%s: unknown optional magic 0x%x" % (source, self.optional_magic))

        self.is_dll = bool(self.characteristics & 0x2000)
        (number_of_rva_and_sizes,) = struct.unpack_from("<I", data, dir_count_off)
        number_of_rva_and_sizes = min(number_of_rva_and_sizes, 16)
        self.data_directories = []
        for i in range(number_of_rva_and_sizes):
            rva, size = struct.unpack_from("<II", data, dir_off + 8 * i)
            self.data_directories.append((rva, size))

        sec_off = opt + self.size_of_optional_header
        self.sections: List[Section] = []
        for i in range(self.number_of_sections):
            base = sec_off + 40 * i
            if base + 40 > len(data):
                break
            raw = data[base : base + 8]
            name = raw.split(b"\0", 1)[0].decode("latin-1", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, base + 8)
            self.sections.append(Section(name, vaddr, vsize, rawsize, rawptr))

    # -- вспомогательное ---------------------------------------------------

    def directory(self, index: int) -> tuple:
        if index < len(self.data_directories):
            return self.data_directories[index]
        return (0, 0)

    def rva_to_offset(self, rva: int) -> Optional[int]:
        """RVA -> смещение в файле. ``None``, если RVA не попадает ни в одну секцию."""
        for sec in self.sections:
            size = sec.virtual_size or sec.size_of_raw_data
            if sec.virtual_address <= rva < sec.virtual_address + max(size, sec.size_of_raw_data):
                delta = rva - sec.virtual_address
                if delta >= sec.size_of_raw_data:
                    # Хвост секции (например .bss) физически отсутствует в файле.
                    return None
                return sec.pointer_to_raw_data + delta
        return None

    def read_cstring(self, offset: int, limit: int = 512) -> str:
        end = self.data.find(b"\0", offset, offset + limit)
        if end < 0:
            end = min(offset + limit, len(self.data))
        return self.data[offset:end].decode("latin-1", "replace")

    def _read_thunks(self, table_rva: int, iat_rva: int) -> List[ImportSlot]:
        slots: List[ImportSlot] = []
        entry_size = 8 if self.is_pe32_plus else 4
        fmt = "<Q" if self.is_pe32_plus else "<I"
        for i in range(4096):  # защита от зацикливания на повреждённом образе
            off = self.rva_to_offset(table_rva + entry_size * i)
            if off is None or off + entry_size > len(self.data):
                break
            (value,) = struct.unpack_from(fmt, self.data, off)
            if value == 0:
                break
            slot_rva = iat_rva + entry_size * i
            if value & self.ordinal_flag:
                slots.append(ImportSlot(None, value & 0xFFFF, slot_rva))
            else:
                name_off = self.rva_to_offset(value & 0x7FFFFFFF)
                if name_off is None or name_off + 2 > len(self.data):
                    break
                slots.append(ImportSlot(self.read_cstring(name_off + 2), None, slot_rva))
        return slots

    # -- каталоги ----------------------------------------------------------

    def import_descriptors(self) -> List[ImportDescriptor]:
        return list(self._iter_descriptors(DIR_IMPORT, delay=False))

    def delay_import_descriptors(self) -> List[ImportDescriptor]:
        return list(self._iter_descriptors(DIR_DELAY_IMPORT, delay=True))

    def all_import_descriptors(self) -> List[ImportDescriptor]:
        return self.import_descriptors() + self.delay_import_descriptors()

    def _iter_descriptors(self, directory: int, delay: bool) -> Iterator[ImportDescriptor]:
        rva, size = self.directory(directory)
        if rva == 0:
            return
        entry_size = 32 if delay else 20
        count = max(1, size // entry_size) if size else 4096
        for i in range(min(count + 1, 4096)):
            off = self.rva_to_offset(rva + entry_size * i)
            if off is None or off + entry_size > len(self.data):
                return
            if delay:
                attrs, name_rva, _hmod, iat_rva, int_rva = struct.unpack_from(
                    "<IIIII", self.data, off
                )
                if (attrs & DELAYLOAD_RVA_BASED) == 0:
                    # Старый формат: поля заданы как VA, а не RVA.
                    name_rva -= self.image_base
                    iat_rva -= self.image_base
                    int_rva -= self.image_base
            else:
                orig_first_thunk, _ts, _chain, name_rva, iat_rva = struct.unpack_from(
                    "<IIIII", self.data, off
                )
                int_rva = orig_first_thunk
            if name_rva == 0 and iat_rva == 0:
                return  # завершающий нулевой дескриптор
            name_off = self.rva_to_offset(name_rva)
            if name_off is None:
                continue
            dll = self.read_cstring(name_off)
            table_rva = int_rva or iat_rva
            yield ImportDescriptor(
                dll=dll,
                iat_rva=iat_rva,
                delay_loaded=delay,
                slots=self._read_thunks(table_rva, iat_rva),
            )
