#!/usr/bin/env python3
"""Converts dualmoe/webui/index.html into a C++ raw-byte header so dualmoe-server is a single portable binary."""
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: embed_webui.py <input.html> <output.h>", file=sys.stderr)
        return 1
    src = pathlib.Path(sys.argv[1]).read_bytes()
    out = pathlib.Path(sys.argv[2])
    out.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "namespace dualmoe {",
        "alignas(8) static const uint8_t kEmbeddedIndexHtml[] = {",
    ]
    row = []
    for i, b in enumerate(src):
        row.append(f"0x{b:02x}")
        if len(row) == 16 or i + 1 == len(src):
            lines.append("    " + ", ".join(row) + ",")
            row = []
    lines.append("    0x00")
    lines.append("};")
    lines.append(f"static constexpr size_t kEmbeddedIndexHtmlSize = {len(src)}u;")
    lines.append("} // namespace dualmoe")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
