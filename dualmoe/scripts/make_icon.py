#!/usr/bin/env python3
"""Builds dualmoe/assets/dualmoe.ico (multi-size Windows icon) + a small PNG.

The .ico is embedded into DualMoE.exe through CMake (RC / generated resource),
so the native window title bar and the taskbar show the DualMoE logo instead of
the generic Win32 icon.
"""
import pathlib
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover - build machines have Pillow
    print("Pillow is required: pip install pillow", file=sys.stderr)
    raise SystemExit(1)

SIZES = [16, 24, 32, 48, 64, 128, 256]


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    src = root / "assets" / "icon_source.png"
    if len(sys.argv) > 1:
        src = pathlib.Path(sys.argv[1])
    out = root / "assets" / "dualmoe.ico"
    out.parent.mkdir(parents=True, exist_ok=True)

    img = Image.open(src).convert("RGBA")
    side = min(img.size)
    left = (img.width - side) // 2
    top = (img.height - side) // 2
    img = img.crop((left, top, left + side, top + side))

    frames = [img.resize((s, s), Image.LANCZOS) for s in SIZES]
    frames[-1].save(out, format="ICO", sizes=[(s, s) for s in SIZES])
    img.resize((256, 256), Image.LANCZOS).save(root / "assets" / "dualmoe-256.png")
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
