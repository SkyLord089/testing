#!/usr/bin/env python
"""Тест операций PyTorch на ZLUDA для AMD gfx803 (RX 580) + HIP SDK 6.4+.

Запускать В АКТИВИРОВАННОМ venv ComfyUI-Zluda:

    venv\\Scripts\\python.exe scripts\\verify-torch27.py
    venv\\Scripts\\python.exe scripts\\verify-torch27.py --sections gemm,conv

Каждая секция:
  * выполняет операцию на GPU,
  * сверяет результат с CPU-эталоном в double,
  * печатает PASS / FAIL / SKIP и (для FAIL) подсказку, куда смотреть.

Код возврата: 0 — всё, что должно было пройти, прошло; 1 — есть FAIL.
"""

from __future__ import annotations

import argparse
import os
import platform
import sys
import time
import traceback

# --- до импорта torch: то, что должно быть выставлено на ZLUDA ---
os.environ.setdefault("DISABLE_ADDMM_CUDA_LT", "1")

# --- цвета (не ломаемся на старых консолях) ---
class C:
    G = "\033[92m"
    R = "\033[91m"
    Y = "\033[93m"
    B = "\033[96m"
    D = "\033[90m"
    X = "\033[0m"

def c(text: str, color: str) -> str:
    if not sys.stdout.isatty() and os.name == "nt":
        # Windows-консоль без VT — без цветов
        return text
    return f"{color}{text}{C.X}"

RESULTS: list[tuple[str, str, str]] = []

def report(section: str, status: str, note: str = "") -> None:
    RESULTS.append((section, status, note))
    color = {"PASS": C.G, "FAIL": C.R, "SKIP": C.Y, "WARN": C.Y}.get(status, C.B)
    print(f"  [{c(status, color)}] {section}" + (f"  — {note}" if note else ""))

def rel_err(gpu, cpu):
    import torch
    gpu = gpu.detach().float().cpu()
    cpu = cpu.detach().double().cpu()
    denom = cpu.abs().max().clamp_min(1e-8)
    return float((gpu - cpu.float()).abs().max() / denom)

# --------------------------------------------------------------------------- #
# Секции
# --------------------------------------------------------------------------- #
def section_env(torch):
    print(c("\n=== ENV ===", C.B))
    print(f"  python      : {sys.version.split()[0]} ({platform.architecture()[0]})")
    print(f"  torch       : {torch.__version__}")
    print(f"  torch.cuda  : {torch.version.cuda}   hip: {torch.version.hip}")
    print(f"  available   : {torch.cuda.is_available()}")
    print(f"  DISABLE_ADDMM_CUDA_LT = {os.environ.get('DISABLE_ADDMM_CUDA_LT')}")
    print(f"  ZLUDA_COMGR_LOG_LEVEL = {os.environ.get('ZLUDA_COMGR_LOG_LEVEL')}")
    print(f"  ZLUDA_CACHE_DIR       = {os.environ.get('ZLUDA_CACHE_DIR')}")
    print(f"  ZLUDA_WAVE64_SLOW_MODE= {os.environ.get('ZLUDA_WAVE64_SLOW_MODE')}")

    if not torch.cuda.is_available():
        report("CUDA available", "FAIL", "torch.cuda.is_available() == False — см. docs/01-diagnosis.md#2")
        return False

    name = torch.cuda.get_device_name(0)
    cnt = torch.cuda.device_count()
    props = torch.cuda.get_device_properties(0)
    print(f"  devices     : {cnt}")
    print(f"  device[0]   : {name}")
    for attr in ("total_memory", "multi_processor_count", "major", "minor"):
        if hasattr(props, attr):
            print(f"    {attr}: {getattr(props, attr)}")
    if hasattr(props, "gcnArchName"):
        print(f"    gcnArchName: {props.gcnArchName}")

    is_zluda = name.endswith("[ZLUDA]")
    report("ZLUDA detected", "PASS" if is_zluda else "WARN",
           "" if is_zluda else "имя устройства без [ZLUDA] — возможно, это настоящая NVIDIA или CPU-фоллбек")
    return True


def section_basic(torch):
    print(c("\n=== BASIC (копирование/заполнение/индексация) ===", C.B))
    try:
        x = torch.arange(1024, device="cuda", dtype=torch.float32)
        y = x.reshape(32, 32) * 2.0
        z = (y + 1.0).sum().item()
        expected = float((torch.arange(1024, dtype=torch.float32).reshape(32, 32) * 2.0 + 1.0).sum())
        ok = abs(z - expected) < 1e-2
        report("arange/reshape/mul/add/sum", "PASS" if ok else "FAIL", f"sum={z:.2f} ожидалось {expected:.2f}")
    except Exception as e:
        report("arange/reshape/mul/add/sum", "FAIL", f"{type(e).__name__}: {e}")
        traceback.print_exc()

    try:
        a = torch.randn(1000, device="cuda")
        v, i = torch.topk(a, 5)
        cpu_v, cpu_i = torch.topk(a.cpu(), 5)
        ok = torch.equal(i.cpu(), cpu_i)
        report("topk (в ComfyUI есть патч на CPU-фоллбек)", "PASS" if ok else "FAIL",
               "" if ok else "индексы не совпали с CPU")
    except Exception as e:
        report("topk", "FAIL", f"{type(e).__name__}: {e}")


def section_gemm(torch):
    print(c("\n=== GEMM (addmm/mm/bmm) — главный подозреваемый ===", C.B))
    torch.manual_seed(0)

    # 1) mm  fp32 — чистый cublasSgemm путь
    try:
        a = torch.randn(512, 512)
        b = torch.randn(512, 512)
        out = (a.cuda() @ b.cuda()).cpu()
        e = rel_err(out, a.double() @ b.double())
        report("mm fp32 512x512", "PASS" if e < 1e-3 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("mm fp32 512x512", "FAIL", f"{type(ex).__name__}: {ex}")
        traceback.print_exc()

    # 2) addmm БЕЗ bias-вектора (используется в Linear без bias)
    try:
        a = torch.randn(256, 512)
        b = torch.randn(512, 512)
        out = torch.addmm(torch.zeros(256, 512, device="cuda"), a.cuda(), b.cuda()).cpu()
        e = rel_err(out, torch.addmm(torch.zeros(256, 512), a, b).double())
        report("addmm beta=0", "PASS" if e < 1e-3 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("addmm beta=0", "FAIL", f"{type(ex).__name__}: {ex}")

    # 3) addmm С bias-вектором — именно этот путь в torch 2.4+ уходит в cublasLt
    try:
        a = torch.randn(256, 512)
        b = torch.randn(512, 512)
        bias = torch.randn(512)
        out = torch.addmm(bias.cuda(), a.cuda(), b.cuda()).cpu()
        e = rel_err(out, (torch.addmm(bias, a, b)).double())
        report("addmm bias (cublasLt-ветка!)", "PASS" if e < 1e-3 else "FAIL",
               f"rel_err={e:.2e}" + ("" if e < 1e-3 else " — см. docs/06-torch27.md#1"))
    except Exception as ex:
        report("addmm bias (cublasLt-ветка!)", "FAIL",
               f"{type(ex).__name__}: {ex}  — вероятно CUBLAS_STATUS_NOT_SUPPORTED из cublasLt")
        traceback.print_exc()

    # 4) nn.Linear с bias (как в реальных моделях)
    try:
        lin = torch.nn.Linear(512, 384).cuda()
        x = torch.randn(64, 512)
        with torch.no_grad():
            out = lin(x.cuda()).cpu()
            ref = torch.nn.functional.linear(x, lin.weight.detach().cpu(), lin.bias.detach().cpu())
        e = rel_err(out, ref.double())
        report("nn.Linear(512->384) с bias", "PASS" if e < 1e-3 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("nn.Linear(512->384) с bias", "FAIL", f"{type(ex).__name__}: {ex}")

    # 5) fp16 GEMM
    try:
        a = torch.randn(512, 512, dtype=torch.float16)
        b = torch.randn(512, 512, dtype=torch.float16)
        out = (a.cuda() @ b.cuda()).float().cpu()
        e = rel_err(out, a.double() @ b.double())
        report("mm fp16", "PASS" if e < 1e-2 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("mm fp16", "FAIL", f"{type(ex).__name__}: {ex}")

    # 6) bf16 GEMM
    try:
        a = torch.randn(512, 512, dtype=torch.bfloat16)
        b = torch.randn(512, 512, dtype=torch.bfloat16)
        out = (a.cuda() @ b.cuda()).float().cpu()
        e = rel_err(out, a.double() @ b.double())
        report("mm bf16", "PASS" if e < 1e-2 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("mm bf16", "FAIL", f"{type(ex).__name__}: {ex}")

    # 7) bmm
    try:
        a = torch.randn(8, 128, 256)
        b = torch.randn(8, 256, 128)
        out = torch.bmm(a.cuda(), b.cuda()).cpu()
        e = rel_err(out, torch.bmm(a.double(), b.double()))
        report("bmm fp32", "PASS" if e < 1e-3 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("bmm fp32", "FAIL", f"{type(ex).__name__}: {ex}")


def section_conv(torch):
    print(c("\n=== CONV2D / VAE-подобные свертки ===", C.B))
    torch.manual_seed(1)
    try:
        conv = torch.nn.Conv2d(32, 64, kernel_size=3, padding=1).cuda()
        x = torch.randn(2, 32, 64, 64)
        with torch.no_grad():
            out = conv(x.cuda()).cpu()
            ref = torch.nn.functional.conv2d(x, conv.weight.detach().cpu(), conv.bias.detach().cpu(), padding=1)
        e = rel_err(out, ref.double())
        report("conv2d 3x3 (тут может включиться cuDNN/MIOpen)", "PASS" if e < 5e-3 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("conv2d 3x3", "FAIL", f"{type(ex).__name__}: {ex}")
        traceback.print_exc()

    try:
        conv = torch.nn.ConvTranspose2d(64, 32, kernel_size=4, stride=2, padding=1).cuda()
        x = torch.randn(1, 64, 32, 32)
        with torch.no_grad():
            out = conv(x.cuda()).cpu()
            ref = torch.nn.functional.conv_transpose2d(x, conv.weight.detach().cpu(), conv.bias.detach().cpu(),
                                                       stride=2, padding=1)
        e = rel_err(out, ref.double())
        report("conv_transpose2d (VAE decode)", "PASS" if e < 5e-3 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("conv_transpose2d", "FAIL", f"{type(ex).__name__}: {ex}")


def section_attention(torch):
    print(c("\n=== ATTENTION (SDPA) ===", C.B))
    torch.manual_seed(2)
    try:
        import torch.nn.functional as F
        q = torch.randn(2, 8, 256, 64)
        k = torch.randn(2, 8, 256, 64)
        v = torch.randn(2, 8, 256, 64)
        out = F.scaled_dot_product_attention(q.cuda(), k.cuda(), v.cuda()).cpu()
        ref = F.scaled_dot_product_attention(q.double(), k.double(), v.double())
        e = rel_err(out, ref)
        report("SDPA fp32", "PASS" if e < 5e-3 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("SDPA fp32", "FAIL", f"{type(ex).__name__}: {ex}")
        traceback.print_exc()

    try:
        import torch.nn.functional as F
        q = torch.randn(2, 8, 128, 64, dtype=torch.float16)
        k = torch.randn(2, 8, 128, 64, dtype=torch.float16)
        v = torch.randn(2, 8, 128, 64, dtype=torch.float16)
        out = F.scaled_dot_product_attention(q.cuda(), k.cuda(), v.cuda()).float().cpu()
        ref = F.scaled_dot_product_attention(q.double(), k.double(), v.double())
        e = rel_err(out, ref)
        report("SDPA fp16", "PASS" if e < 2e-2 else "FAIL", f"rel_err={e:.2e}")
    except Exception as ex:
        report("SDPA fp16", "FAIL", f"{type(ex).__name__}: {ex}")

    # статус бэкендов
    try:
        for name in ("flash_sdp_enabled", "mem_efficient_sdp_enabled", "math_sdp_enabled", "cudnn_sdp_enabled"):
            fn = getattr(torch.backends.cuda, name, None)
            if fn:
                print(f"  {name}: {fn()}")
    except Exception:
        pass


def section_memory(torch):
    print(c("\n=== ПАМЯТЬ ===", C.B))
    try:
        free_b, total_b = torch.cuda.mem_get_info()
        print(f"  mem_get_info: free={free_b/2**20:.0f} MB, total={total_b/2**20:.0f} MB")
        props = torch.cuda.get_device_properties(0)
        print(f"  props.total_memory: {props.total_memory/2**20:.0f} MB")
        if abs(total_b - props.total_memory) > 256 * 2**20:
            report("total vs mem_get_info", "WARN", "расхождение >256 МБ — ZLUDA/HIP по-разному считают VRAM")
        else:
            report("total vs mem_get_info", "PASS")
    except Exception as ex:
        report("mem_get_info", "FAIL", f"{type(ex).__name__}: {ex}")

    try:
        # аллокация «широкого» буфера, чтобы проверить, что большие блоки не отваливаются
        x = torch.empty(256, 1024, 1024, dtype=torch.uint8, device="cuda")  # 256 МБ
        x.fill_(7)
        s = int(x[::4096, 0, 0].sum())
        del x
        torch.cuda.empty_cache()
        report("аллокация 256 МБ", "PASS", f"контрольная сумма={s}")
    except Exception as ex:
        report("аллокация 256 МБ", "FAIL", f"{type(ex).__name__}: {ex}")


def section_timing(torch):
    print(c("\n=== СКОРОСТЬ (ориентир, не экзамен) ===", C.B))
    try:
        a = torch.randn(2048, 2048, device="cuda", dtype=torch.float16)
        b = torch.randn(2048, 2048, device="cuda", dtype=torch.float16)
        # прогрев (тут ZLUDA компилирует ядра — может быть долго!)
        for _ in range(3):
            torch.mm(a, b)
        torch.cuda.synchronize()
        t0 = time.time()
        n = 10
        for _ in range(n):
            torch.mm(a, b)
        torch.cuda.synchronize()
        dt = (time.time() - t0) / n
        tflops = 2 * 2048**3 / dt / 1e12
        print(f"  fp16 2048³ mm: {dt*1000:.1f} мс/итерация ≈ {tflops:.2f} TFLOP/s")
        report("timing fp16 mm", "PASS", f"{tflops:.2f} TFLOP/s")
    except Exception as ex:
        report("timing fp16 mm", "FAIL", f"{type(ex).__name__}: {ex}")


SECTIONS = {
    "env": section_env,
    "basic": section_basic,
    "gemm": section_gemm,
    "conv": section_conv,
    "attention": section_attention,
    "memory": section_memory,
    "timing": section_timing,
}


def main() -> int:
    ap = argparse.ArgumentParser(description="Проверка операций torch 2.7 на ZLUDA/gfx803")
    ap.add_argument("--sections", default="env,basic,gemm,conv,attention,memory,timing",
                    help="через запятую, из: " + ",".join(SECTIONS))
    ap.add_argument("--fail-fast", action="store_true", help="остановиться на первом FAIL")
    args = ap.parse_args()

    print(c("PyTorch / ZLUDA / gfx803 verification", C.B))
    import torch

    ok = True
    for name in [s.strip() for s in args.sections.split(",") if s.strip()]:
        fn = SECTIONS.get(name)
        if not fn:
            print(f"  неизвестная секция: {name}")
            continue
        try:
            res = fn(torch)
            if name == "env" and res is False:
                ok = False
                # без CUDA остальные секции бессмысленны
                if args.fail_fast:
                    break
                rest = [s.strip() for s in args.sections.split(",") if s.strip()]
                rest = rest[rest.index(name) + 1:]
                for skip in rest:
                    report(skip, "SKIP", "нет CUDA-устройства")
                break
        except Exception as ex:
            report(name, "FAIL", f"исключение в секции: {type(ex).__name__}: {ex}")
            traceback.print_exc()
            ok = False

    print(c("\n=== СВОДКА ===", C.B))
    fails = [r for r in RESULTS if r[1] == "FAIL"]
    for section, status, note in RESULTS:
        color = {"PASS": C.G, "FAIL": C.R, "SKIP": C.Y, "WARN": C.Y}.get(status, C.B)
        print(f"  {c(f'{status:<4}', color)} {section}" + (f"  — {note}" if note else ""))

    print()
    if fails:
        print(c(f"Итог: {len(fails)} FAIL. Куда смотреть:", C.R))
        print("  • FAIL на addmm bias / nn.Linear  -> docs/06-torch27.md#1 (cublasLt)")
        print("  • FAIL на mm/bmm                  -> docs/05-gfx803-rocblas.md (rocBLAS gfx803)")
        print("  • FAIL на conv/conv_transpose     -> cuDNN/MIOpen: держать torch.backends.cudnn.enabled = False")
        print("  • FAIL на attention               -> docs/06-torch27.md#2 (SDPA-бэкенды)")
        return 1

    print(c("Итог: всё критичное прошло.", C.G))
    return 0


if __name__ == "__main__":
    sys.exit(main())
