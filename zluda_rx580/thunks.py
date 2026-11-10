"""Машинный код заглушек, заменяющих ``cudaHostAlloc`` / ``cudaFreeHost``.

Заглушки собираются вручную (assembler не нужен): набор инструкций крошечный,
каждая кодировка проверена в ``tests/test_thunks.py`` по эталону из keystone,
а исполнение — эмуляцией в unicorn.

Смысл заглушек
---------------
PyTorch, начиная с 2.3.0, выделяет pinned-память через ``cudaHostAlloc``.
Под ZLUDA это ``hipHostMalloc(..., flags=0)``, который на gfx803 (RX 480–580)
запрашивает SVM с атомарными операциями и всегда завершается ошибкой
``hipErrorOutOfMemory`` == 2 == ``CUDA_ERROR_OUT_OF_MEMORY``.

Нам от этой памяти нужна только возможность копировать в неё данные с GPU
и читать их с CPU. Так ровно вёл себя PyTorch 2.2.1: обычный (pageable)
буфер на стеке + ``memcpy_and_sync``. Поэтому заглушка просто выделяет
выровненную по границе страницы область обычной памяти из CRT:

* ``cudaHostAlloc(void** pHost, size_t size, unsigned flags)``
  -> ``_aligned_malloc(size, 4096)``, возвращает 0 (``cudaSuccess``)
  или 2 (``cudaErrorMemoryAllocation``) / 1 (``cudaErrorInvalidValue``);
* ``cudaFreeHost(void* ptr)`` -> ``_aligned_free(ptr)``, всегда 0.

Флаги намеренно игнорируются: PyTorch всегда передаёт ``cudaHostAllocDefault``.

Память, выделенная ``_aligned_malloc``, не зарегистрирована в HIP как host
memory. ``ihipMemcpy`` в ROCm/clr сам определяет такой указатель и делает
копирование через внутренний staging-буфер с ожиданием завершения — то есть
тот же путь, которым шёл PyTorch 2.2.1. Результат корректен, но медленнее
настоящего pinned-буфера.
"""

from __future__ import annotations

import struct
from typing import Iterable, List, Sequence, Tuple

# Коды возврата cudaError_t, которые нам нужны.
CUDA_SUCCESS = 0
CUDA_ERROR_INVALID_VALUE = 1
CUDA_ERROR_OUT_OF_MEMORY = 2

PAGE_SIZE = 0x1000

# --- кодировки инструкций -------------------------------------------------
# Каждая запись: мнемоника -> байты. Imm64 подставляется отдельно.

OP_PUSH_RBX = b"\x53"
OP_POP_RBX = b"\x5b"
OP_RET = b"\xc3"
OP_XOR_EAX_EAX = b"\x31\xc0"
OP_CALL_RAX = b"\xff\xd0"


def sub_rsp_imm8(imm: int) -> bytes:
    return b"\x48\x83\xec" + bytes([imm])


def add_rsp_imm8(imm: int) -> bytes:
    return b"\x48\x83\xc4" + bytes([imm])


def mov_rbx_rcx() -> bytes:
    return b"\x48\x89\xcb"


def mov_rcx_rdx() -> bytes:
    return b"\x48\x89\xd1"


def test_rcx_rcx() -> bytes:
    return b"\x48\x85\xc9"


def test_rdx_rdx() -> bytes:
    return b"\x48\x85\xd2"


def test_rax_rax() -> bytes:
    return b"\x48\x85\xc0"


def mov_ecx_imm32(imm: int) -> bytes:
    return b"\xb9" + struct.pack("<I", imm)


def mov_edx_imm32(imm: int) -> bytes:
    return b"\xba" + struct.pack("<I", imm)


def mov_eax_imm32(imm: int) -> bytes:
    return b"\xb8" + struct.pack("<I", imm)


def mov_rax_imm64(imm: int) -> bytes:
    return b"\x48\xb8" + struct.pack("<Q", imm)


def mov_mem_rbx_rax() -> bytes:
    return b"\x48\x89\x03"


# --- микроассемблер -------------------------------------------------------

Item = Tuple
"""Элемент программы: ("bytes", b"..."), ("label", name), ("jz", name), ("jnz", name)."""


def assemble(items: Sequence[Item]) -> bytes:
    """Двухпроходная сборка. Поддерживаются только короткие переходы (rel8)."""
    sizes: List[int] = []
    for item in items:
        kind = item[0]
        if kind == "label":
            sizes.append(0)
        elif kind == "bytes":
            sizes.append(len(item[1]))
        elif kind in ("jz", "jnz"):
            sizes.append(2)
        else:
            raise ValueError("unknown item: %r" % (item,))

    offsets = {}
    positions = []
    cursor = 0
    for item, size in zip(items, sizes):
        if item[0] == "label":
            if item[1] in offsets:
                raise ValueError("duplicate label %r" % (item[1],))
            offsets[item[1]] = cursor
        positions.append(cursor)
        cursor += size

    out = bytearray()
    for item, position in zip(items, positions):
        kind = item[0]
        if kind == "label":
            continue
        if kind == "bytes":
            out += item[1]
        else:
            target = offsets[item[1]]
            rel = target - (position + 2)
            if not -128 <= rel <= 127:
                raise ValueError("jump to %r does not fit in rel8 (%d)" % (item[1], rel))
            opcode = 0x74 if kind == "jz" else 0x75
            out += bytes([opcode, rel & 0xFF])
    return bytes(out)


# --- сами заглушки --------------------------------------------------------


def build_host_alloc_thunk(aligned_malloc: int) -> bytes:
    """``cudaError_t f(void** pHost, size_t size, unsigned flags)``.

    Ассемблерный эквивалент (x86-64, соглашение о вызовах Windows)::

        push rbx
        sub  rsp, 0x20          ; 32 байта shadow space, rsp выровнен по 16
        mov  rbx, rcx           ; сохранить pHost
        test rcx, rcx
        jz   .invalid           ; pHost == NULL -> cudaErrorInvalidValue
        mov  rcx, rdx           ; size
        test rdx, rdx
        jnz  .have_size
        mov  ecx, 1             ; size == 0 -> выделяем 1 байт, как CUDA
    .have_size:
        mov  edx, 0x1000        ; выравнивание по границе страницы
        mov  rax, _aligned_malloc
        call rax
        test rax, rax
        jz   .oom
        mov  [rbx], rax
        xor  eax, eax           ; cudaSuccess
        add  rsp, 0x20
        pop  rbx
        ret
    .invalid:
        mov  eax, 1
        add  rsp, 0x20
        pop  rbx
        ret
    .oom:
        mov  eax, 2
        add  rsp, 0x20
        pop  rbx
        ret

    Выравнивание стека: на входе ``rsp % 16 == 8``; ``push rbx`` даёт 0,
    ``sub rsp, 0x20`` сохраняет 0 — на момент ``call`` стек выровнен.
    """
    if aligned_malloc == 0:
        raise ValueError("aligned_malloc address must not be 0")
    return assemble(
        [
            ("bytes", OP_PUSH_RBX),
            ("bytes", sub_rsp_imm8(0x20)),
            ("bytes", mov_rbx_rcx()),
            ("bytes", test_rcx_rcx()),
            ("jz", "invalid"),
            ("bytes", mov_rcx_rdx()),
            ("bytes", test_rdx_rdx()),
            ("jnz", "have_size"),
            ("bytes", mov_ecx_imm32(1)),
            ("label", "have_size"),
            ("bytes", mov_edx_imm32(PAGE_SIZE)),
            ("bytes", mov_rax_imm64(aligned_malloc)),
            ("bytes", OP_CALL_RAX),
            ("bytes", test_rax_rax()),
            ("jz", "oom"),
            ("bytes", mov_mem_rbx_rax()),
            ("bytes", OP_XOR_EAX_EAX),
            ("bytes", add_rsp_imm8(0x20)),
            ("bytes", OP_POP_RBX),
            ("bytes", OP_RET),
            ("label", "invalid"),
            ("bytes", mov_eax_imm32(CUDA_ERROR_INVALID_VALUE)),
            ("bytes", add_rsp_imm8(0x20)),
            ("bytes", OP_POP_RBX),
            ("bytes", OP_RET),
            ("label", "oom"),
            ("bytes", mov_eax_imm32(CUDA_ERROR_OUT_OF_MEMORY)),
            ("bytes", add_rsp_imm8(0x20)),
            ("bytes", OP_POP_RBX),
            ("bytes", OP_RET),
        ]
    )


def build_host_free_thunk(aligned_free: int) -> bytes:
    """``cudaError_t f(void* ptr)``.

    Ассемблерный эквивалент::

        sub  rsp, 0x28          ; 8 + 0x28 = 0x30 -> rsp выровнен по 16
        mov  rax, _aligned_free
        call rax                ; rcx уже равен ptr
        xor  eax, eax
        add  rsp, 0x28
        ret

    ``_aligned_free(NULL)`` в CRT безопасен, поэтому проверку на NULL
    не делаем.
    """
    if aligned_free == 0:
        raise ValueError("aligned_free address must not be 0")
    return assemble(
        [
            ("bytes", sub_rsp_imm8(0x28)),
            ("bytes", mov_rax_imm64(aligned_free)),
            ("bytes", OP_CALL_RAX),
            ("bytes", OP_XOR_EAX_EAX),
            ("bytes", add_rsp_imm8(0x28)),
            ("bytes", OP_RET),
        ]
    )


def describe(code: bytes) -> Iterable[str]:
    """Побайтовая разбивка заглушки — для логов и документации."""
    yield "length=%d bytes" % len(code)
    for i in range(0, len(code), 16):
        chunk = code[i : i + 16]
        yield "  %04x: %s" % (i, " ".join("%02x" % b for b in chunk))
