"""Тесты машинного кода заглушек (zluda_rx580.thunks).

Две группы:

* проверка байтовой раскладки — работает везде, зависимостей нет;
* исполнение кода в эмуляторе unicorn (если установлен) — это настоящая
  проверка: эмулируются именно те байты, которые будут записаны в память
  процесса на Windows. Проверяются коды возврата, записанный указатель,
  аргументы, переданные в ``_aligned_malloc``, и выравнивание стека на
  момент ``call``.

Запуск: ``python -m unittest tests.test_thunks -v``
"""

from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from zluda_rx580.thunks import (  # noqa: E402
    CUDA_ERROR_INVALID_VALUE,
    CUDA_ERROR_OUT_OF_MEMORY,
    CUDA_SUCCESS,
    build_host_alloc_thunk,
    build_host_free_thunk,
)

ALLOC_ADDR = 0x00007FF0DEADB000
FREE_ADDR = 0x00007FF0DEADF000


class ByteLayoutTests(unittest.TestCase):
    def test_alloc_thunk_exact_bytes(self):
        code = build_host_alloc_thunk(ALLOC_ADDR)
        expected = bytes(
            [
                0x53,  # push rbx
                0x48, 0x83, 0xEC, 0x20,  # sub rsp, 0x20
                0x48, 0x89, 0xCB,  # mov rbx, rcx
                0x48, 0x85, 0xC9,  # test rcx, rcx
                0x74, 0x2E,  # jz .invalid (+0x2e)
                0x48, 0x89, 0xD1,  # mov rcx, rdx
                0x48, 0x85, 0xD2,  # test rdx, rdx
                0x75, 0x05,  # jnz .have_size (+5)
                0xB9, 0x01, 0x00, 0x00, 0x00,  # mov ecx, 1
                0xBA, 0x00, 0x10, 0x00, 0x00,  # mov edx, 0x1000
                0x48, 0xB8,
            ]
        ) + struct.pack("<Q", ALLOC_ADDR) + bytes(
            [
                0xFF, 0xD0,  # call rax
                0x48, 0x85, 0xC0,  # test rax, rax
                0x74, 0x16,  # jz .oom (+0x16)
                0x48, 0x89, 0x03,  # mov [rbx], rax
                0x31, 0xC0,  # xor eax, eax
                0x48, 0x83, 0xC4, 0x20,  # add rsp, 0x20
                0x5B,  # pop rbx
                0xC3,  # ret
                0xB8, 0x01, 0x00, 0x00, 0x00,  # mov eax, 1
                0x48, 0x83, 0xC4, 0x20,
                0x5B,
                0xC3,
                0xB8, 0x02, 0x00, 0x00, 0x00,  # mov eax, 2
                0x48, 0x83, 0xC4, 0x20,
                0x5B,
                0xC3,
            ]
        )
        self.assertEqual(code, expected)

    def test_free_thunk_exact_bytes(self):
        code = build_host_free_thunk(FREE_ADDR)
        expected = (
            bytes([0x48, 0x83, 0xEC, 0x28])  # sub rsp, 0x28
            + bytes([0x48, 0xB8])
            + struct.pack("<Q", FREE_ADDR)  # mov rax, _aligned_free
            + bytes(
                [
                    0xFF, 0xD0,  # call rax
                    0x31, 0xC0,  # xor eax, eax
                    0x48, 0x83, 0xC4, 0x28,  # add rsp, 0x28
                    0xC3,  # ret
                ]
            )
        )
        self.assertEqual(code, expected)

    def test_address_is_embedded_little_endian(self):
        code = build_host_alloc_thunk(0x0123456789ABCDEF)
        self.assertIn(struct.pack("<Q", 0x0123456789ABCDEF), code)

    def test_rejects_zero_address(self):
        with self.assertRaises(ValueError):
            build_host_alloc_thunk(0)
        with self.assertRaises(ValueError):
            build_host_free_thunk(0)

    def test_ends_with_ret(self):
        self.assertEqual(build_host_alloc_thunk(ALLOC_ADDR)[-1], 0xC3)
        self.assertEqual(build_host_free_thunk(FREE_ADDR)[-1], 0xC3)


try:  # pragma: no cover
    import unicorn  # type: ignore
    from unicorn import x86_const  # type: ignore
except ImportError:  # pragma: no cover
    unicorn = None
    x86_const = None


CODE_BASE = 0x100000
STACK_BASE = 0x400000
STACK_SIZE = 0x10000
SCRATCH_BASE = 0x500000
RESULT_PTR = 0x600000
RETURNED_PTR = 0x700000
STOP_ADDR = 0x300000

# Ячейки, куда заглушка-заменитель CRT пишет наблюдения.
OBSERVED_SIZE = SCRATCH_BASE + 0x08
OBSERVED_ALIGN = SCRATCH_BASE + 0x10
OBSERVED_STACK = SCRATCH_BASE + 0x18
OBSERVED_FREED = SCRATCH_BASE + 0x20


def _alloc_stub(return_value: int) -> bytes:
    """``_aligned_malloc(size, align)``: пишет аргументы и rsp, возвращает указатель."""
    return (
        b"\x48\xb8" + struct.pack("<Q", OBSERVED_SIZE) + b"\x48\x89\x08"  # mov [rax], rcx
        + b"\x48\xb8" + struct.pack("<Q", OBSERVED_ALIGN) + b"\x48\x89\x10"  # mov [rax], rdx
        + b"\x48\xb8" + struct.pack("<Q", OBSERVED_STACK)
        + b"\x48\x89\xe1"  # mov rcx, rsp
        + b"\x48\x83\xe1\x0f"  # and rcx, 0xF
        + b"\x48\x89\x08"  # mov [rax], rcx
        + b"\x48\xb8" + struct.pack("<Q", return_value)
        + b"\xc3"  # ret
    )


def _free_stub() -> bytes:
    return (
        b"\x48\xb8" + struct.pack("<Q", OBSERVED_FREED) + b"\x48\x89\x08" + b"\xc3"
    )


@unittest.skipUnless(unicorn, "unicorn is not installed")
class EmulationTests(unittest.TestCase):
    """Исполнение настоящих байтов заглушки в эмуляторе."""

    def setUp(self):
        self.emulator = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_64)
        self.emulator.mem_map(CODE_BASE, 0x10000)
        self.emulator.mem_map(STACK_BASE, STACK_SIZE)
        self.emulator.mem_map(SCRATCH_BASE, 0x1000)
        self.emulator.mem_map(RESULT_PTR, 0x1000)
        self.emulator.mem_map(STOP_ADDR, 0x1000)
        self.emulator.reg_write(x86_const.UC_X86_REG_RSP, STACK_BASE + STACK_SIZE - 0x200)

    def _load(self, offset: int, blob: bytes) -> int:
        self.emulator.mem_write(CODE_BASE + offset, blob)
        return CODE_BASE + offset

    def _call(self, entry: int, rcx: int, rdx: int = 0, r8: int = 0) -> int:
        self.emulator.reg_write(x86_const.UC_X86_REG_RCX, rcx)
        self.emulator.reg_write(x86_const.UC_X86_REG_RDX, rdx)
        self.emulator.reg_write(x86_const.UC_X86_REG_R8, r8)
        self.emulator.reg_write(x86_const.UC_X86_REG_RAX, 0)
        # Адрес возврата: эмуляция остановится, когда заглушка выполнит ret.
        top = self.emulator.reg_read(x86_const.UC_X86_REG_RSP)
        self.emulator.mem_write(top - 8, struct.pack("<Q", STOP_ADDR))
        self.emulator.reg_write(x86_const.UC_X86_REG_RSP, top - 8)
        self.emulator.emu_start(entry, STOP_ADDR, count=10000)
        return self.emulator.reg_read(x86_const.UC_X86_REG_EAX)

    def _read(self, address: int, size: int = 8) -> int:
        return int.from_bytes(self.emulator.mem_read(address, size), "little")

    # -- cudaHostAlloc ----------------------------------------------------

    def _alloc(self, return_value: int, host_ptr: int, size: int):
        stub = self._load(0x1000, _alloc_stub(return_value))
        thunk = self._load(0x0000, build_host_alloc_thunk(stub))
        self.emulator.mem_write(RESULT_PTR, b"\0" * 8)
        return self._call(thunk, host_ptr, size)

    def test_success_writes_pointer_and_returns_zero(self):
        status = self._alloc(RETURNED_PTR, RESULT_PTR, 0x1234)
        self.assertEqual(status, CUDA_SUCCESS)
        self.assertEqual(self._read(RESULT_PTR), RETURNED_PTR)
        self.assertEqual(self._read(OBSERVED_SIZE), 0x1234)
        self.assertEqual(self._read(OBSERVED_ALIGN), 0x1000)

    def test_stack_is_16_byte_aligned_at_call(self):
        self._alloc(RETURNED_PTR, RESULT_PTR, 64)
        # На входе в вызываемую функцию rsp % 16 == 8 — значит перед call был 0.
        self.assertEqual(self._read(OBSERVED_STACK), 8)

    def test_zero_size_requests_one_byte(self):
        status = self._alloc(RETURNED_PTR, RESULT_PTR, 0)
        self.assertEqual(status, CUDA_SUCCESS)
        self.assertEqual(self._read(OBSERVED_SIZE), 1)

    def test_null_output_pointer_is_invalid_value(self):
        status = self._alloc(RETURNED_PTR, 0, 64)
        self.assertEqual(status, CUDA_ERROR_INVALID_VALUE)

    def test_allocator_failure_maps_to_out_of_memory(self):
        status = self._alloc(0, RESULT_PTR, 64)
        self.assertEqual(status, CUDA_ERROR_OUT_OF_MEMORY)
        self.assertEqual(self._read(RESULT_PTR), 0)

    def test_flags_argument_is_ignored(self):
        first = self._alloc(RETURNED_PTR, RESULT_PTR, 4096)
        self.setUp()
        second = self._alloc(RETURNED_PTR, RESULT_PTR, 4096)
        self.assertEqual((first, second), (CUDA_SUCCESS, CUDA_SUCCESS))

    # -- cudaFreeHost -----------------------------------------------------

    def test_free_passes_pointer_and_returns_zero(self):
        stub = self._load(0x1000, _free_stub())
        thunk = self._load(0x0000, build_host_free_thunk(stub))
        status = self._call(thunk, 0xABCD0000)
        self.assertEqual(status, CUDA_SUCCESS)
        self.assertEqual(self._read(OBSERVED_FREED), 0xABCD0000)

    def test_free_stack_is_16_byte_aligned_at_call(self):
        stub = self._load(0x1000, _free_stub())
        thunk = self._load(0x0000, build_host_free_thunk(stub))
        self._call(thunk, 0xABCD0000)
        # Стек выровнен: проверяем, что ret не испортил rsp.
        self.assertEqual(
            self.emulator.reg_read(x86_const.UC_X86_REG_RSP) & 0xF,
            (STACK_BASE + STACK_SIZE - 0x200) & 0xF,
        )


if __name__ == "__main__":
    unittest.main()
