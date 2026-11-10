"""Тесты парсера PE (zluda_rx580.pe).

Основной тест строит синтетический PE32+ образ прямо в памяти — это настоящая
проверка кода, а не заглушка: парсер разбирает заголовки, каталог импорта и
каталог отложенного импорта и возвращает RVA слотов IAT.

Дополнительно, если задан путь к настоящей Windows-DLL (переменная
``ZLRX580_TEST_DLL``), проверяется разбор реального файла; если установлен
``pefile`` — результаты сверяются с ним.
"""

from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from zluda_rx580 import pe as pe_mod  # noqa: E402


# ---------------------------------------------------------------------------
# конструктор синтетического образа
# ---------------------------------------------------------------------------

HEADER_SIZE = 0x200
TEXT_VA, TEXT_RAW_PTR, TEXT_RAW_SIZE = 0x1000, 0x200, 0x200
DATA_VA, DATA_RAW_PTR, DATA_RAW_SIZE = 0x2000, 0x400, 0x800
IMAGE_BASE = 0x180000000

IMPORT_DIR_RVA = 0x2000
INT_TABLE_RVA = 0x2100
DLL_NAME_RVA = 0x2200
IMPORT_BY_NAME_RVA = 0x2220
IAT_RVA = 0x2300

DELAY_DIR_RVA = 0x2400
DELAY_INT_RVA = 0x2480
DELAY_IAT_RVA = 0x2500

DLL_NAME = b"cudart64_110.dll"
FUNC_NAMES = [b"cudaHostAlloc", b"cudaFreeHost"]


def _cstr(value: bytes) -> bytes:
    return value + b"\0"


def build_synthetic_pe(delay_load: bool = True) -> bytes:
    """PE32+ DLL с двумя импортами из cudart64_110.dll."""
    data = bytearray(HEADER_SIZE + TEXT_RAW_SIZE + DATA_RAW_SIZE)

    # DOS header
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)

    # PE signature + COFF header
    struct.pack_into("<I", data, 0x80, pe_mod.NT_SIGNATURE)
    struct.pack_into(
        "<HHIIIHH",
        data,
        0x84,
        pe_mod.IMAGE_FILE_MACHINE_AMD64,  # Machine
        2,  # NumberOfSections
        0,  # TimeDateStamp
        0,  # PointerToSymbolTable
        0,  # NumberOfSymbols
        240,  # SizeOfOptionalHeader (PE32+)
        0x2022,  # Characteristics: DLL | EXECUTABLE | LARGE_ADDRESS_AWARE
    )

    # Optional header (PE32+)
    opt = 0x98
    struct.pack_into("<H", data, opt, pe_mod.OPTIONAL_HEADER_MAGIC_PE32_PLUS)
    struct.pack_into("<Q", data, opt + 24, IMAGE_BASE)
    struct.pack_into("<I", data, opt + 108, 16)  # NumberOfRvaAndSizes
    directories = opt + 112
    struct.pack_into("<II", data, directories + 8 * pe_mod.DIR_IMPORT, IMPORT_DIR_RVA, 40)
    if delay_load:
        struct.pack_into("<II", data, directories + 8 * pe_mod.DIR_DELAY_IMPORT, DELAY_DIR_RVA, 32)

    # Section headers
    sections = 0x98 + 240
    struct.pack_into(
        "<8sIIIIIIHHI",
        data,
        sections,
        b".text\0\0\0",
        0x100,
        TEXT_VA,
        TEXT_RAW_SIZE,
        TEXT_RAW_PTR,
        0,
        0,
        0,
        0,
        0x60000020,
    )
    struct.pack_into(
        "<8sIIIIIIHHI",
        data,
        sections + 40,
        b".data\0\0\0",
        0x800,
        DATA_VA,
        DATA_RAW_SIZE,
        DATA_RAW_PTR,
        0,
        0,
        0,
        0,
        0xC0000040,
    )

    def put(rva: int, blob: bytes) -> None:
        offset = DATA_RAW_PTR + (rva - DATA_VA)
        data[offset : offset + len(blob)] = blob

    # Import directory: один дескриптор + завершающий нулевой.
    put(
        IMPORT_DIR_RVA,
        struct.pack(
            "<IIIII",
            INT_TABLE_RVA,  # OriginalFirstThunk
            0,  # TimeDateStamp
            0,  # ForwarderChain
            DLL_NAME_RVA,  # Name
            IAT_RVA,  # FirstThunk (IAT)
        )
        + b"\0" * 20,
    )

    # Lookup table: два символа по имени + ноль.
    put(
        INT_TABLE_RVA,
        struct.pack(
            "<QQQ",
            IMPORT_BY_NAME_RVA,
            IMPORT_BY_NAME_RVA + 2 + len(FUNC_NAMES[0]) + 1,
            0,
        ),
    )

    put(DLL_NAME_RVA, _cstr(DLL_NAME))
    offset = IMPORT_BY_NAME_RVA
    for name in FUNC_NAMES:
        put(offset, struct.pack("<H", 0) + _cstr(name))
        offset += 2 + len(name) + 1

    # IAT: два заполненных слота + ноль.
    put(IAT_RVA, struct.pack("<QQQ", 0x1111, 0x2222, 0))

    if delay_load:
        put(
            DELAY_DIR_RVA,
            struct.pack(
                "<IIIIIIII",
                pe_mod.DELAYLOAD_RVA_BASED,
                DLL_NAME_RVA,
                0,  # module handle
                DELAY_IAT_RVA,
                DELAY_INT_RVA,
                0,
                0,
                0,
            )
            + b"\0" * 32,
        )
        put(DELAY_INT_RVA, struct.pack("<QQ", IMPORT_BY_NAME_RVA, 0))
        put(DELAY_IAT_RVA, struct.pack("<QQ", 0x3333, 0))

    return bytes(data)


class SyntheticPETests(unittest.TestCase):
    def setUp(self):
        self.image = pe_mod.PEImage(build_synthetic_pe(), "synthetic")

    def test_headers(self):
        self.assertTrue(self.image.is_pe32_plus)
        self.assertTrue(self.image.is_dll)
        self.assertEqual(self.image.machine, pe_mod.IMAGE_FILE_MACHINE_AMD64)
        self.assertEqual(self.image.image_base, IMAGE_BASE)
        self.assertEqual([s.name for s in self.image.sections], [".text", ".data"])

    def test_rva_mapping(self):
        self.assertEqual(self.image.rva_to_offset(DATA_VA), DATA_RAW_PTR)
        self.assertEqual(self.image.rva_to_offset(DATA_VA + 0x10), DATA_RAW_PTR + 0x10)
        self.assertIsNone(self.image.rva_to_offset(0x9000))

    def test_import_descriptor(self):
        descriptors = self.image.import_descriptors()
        self.assertEqual(len(descriptors), 1)
        descriptor = descriptors[0]
        self.assertEqual(descriptor.dll, "cudart64_110.dll")
        self.assertEqual(descriptor.iat_rva, IAT_RVA)
        self.assertFalse(descriptor.delay_loaded)
        self.assertEqual([slot.name for slot in descriptor.slots], ["cudaHostAlloc", "cudaFreeHost"])
        self.assertEqual(
            [slot.iat_rva for slot in descriptor.slots], [IAT_RVA, IAT_RVA + 8]
        )

    def test_delay_import_descriptor(self):
        descriptors = self.image.delay_import_descriptors()
        self.assertEqual(len(descriptors), 1)
        descriptor = descriptors[0]
        self.assertTrue(descriptor.delay_loaded)
        self.assertEqual(descriptor.dll, "cudart64_110.dll")
        self.assertEqual([slot.name for slot in descriptor.slots], ["cudaHostAlloc"])
        self.assertEqual(descriptor.slots[0].iat_rva, DELAY_IAT_RVA)

    def test_no_delay_directory(self):
        image = pe_mod.PEImage(build_synthetic_pe(delay_load=False), "synthetic")
        self.assertEqual(image.delay_import_descriptors(), [])
        self.assertEqual(len(image.import_descriptors()), 1)

    def test_find_helper(self):
        descriptor = self.image.import_descriptors()[0]
        slot = descriptor.find("cudaFreeHost")
        self.assertIsNotNone(slot)
        self.assertEqual(slot.iat_rva, IAT_RVA + 8)
        self.assertIsNone(descriptor.find("cudaMalloc"))


class ParserRobustnessTests(unittest.TestCase):
    def test_rejects_non_pe(self):
        with self.assertRaises(pe_mod.PEFormatError):
            pe_mod.PEImage(b"not a pe image at all" * 16, "garbage")

    def test_rejects_bad_nt_signature(self):
        data = bytearray(build_synthetic_pe())
        data[0x80:0x84] = b"XX\0\0"
        with self.assertRaises(pe_mod.PEFormatError):
            pe_mod.PEImage(bytes(data), "bad-nt")

    def test_rejects_truncated_e_lfanew(self):
        data = bytearray(build_synthetic_pe())
        struct.pack_into("<I", data, 0x3C, 0xFFFF0000)
        with self.assertRaises(pe_mod.PEFormatError):
            pe_mod.PEImage(bytes(data), "bad-lfanew")

    def test_rejects_unknown_optional_magic(self):
        data = bytearray(build_synthetic_pe())
        struct.pack_into("<H", data, 0x98, 0x0999)
        with self.assertRaises(pe_mod.PEFormatError):
            pe_mod.PEImage(bytes(data), "bad-magic")


@unittest.skipUnless(os.environ.get("ZLRX580_TEST_DLL"), "set ZLRX580_TEST_DLL to a real PE file")
class RealDLLTests(unittest.TestCase):
    def test_parse_real_dll(self):
        path = os.environ["ZLRX580_TEST_DLL"]
        with open(path, "rb") as handle:
            image = pe_mod.PEImage(handle.read(), path)
        descriptors = image.all_import_descriptors()
        self.assertTrue(descriptors)
        for descriptor in descriptors:
            self.assertTrue(descriptor.dll)
            self.assertTrue(descriptor.iat_rva)
            self.assertTrue(descriptor.slots)


try:  # pragma: no cover - опциональная сверка с pefile
    import pefile as _pefile
except ImportError:  # pragma: no cover
    _pefile = None


@unittest.skipUnless(_pefile, "pefile is not installed")
class PefileCrossCheckTests(unittest.TestCase):
    def test_matches_pefile_on_synthetic_image(self):
        blob = build_synthetic_pe()
        ours = pe_mod.PEImage(blob, "synthetic")
        try:
            theirs = _pefile.PE(data=blob)
        except Exception as exc:  # pragma: no cover - pefile строже нашего парсера
            self.skipTest("pefile refused the synthetic image: %r" % (exc,))
        ours_map = {
            d.dll: sorted(s.name for s in d.slots) for d in ours.import_descriptors()
        }
        theirs_map = {
            e.dll.decode(): sorted(i.name.decode() for i in e.imports)
            for e in theirs.DIRECTORY_ENTRY_IMPORT
        }
        self.assertEqual(ours_map, theirs_map)


if __name__ == "__main__":
    unittest.main()
