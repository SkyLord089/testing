#!/usr/bin/env python3
"""Package an already-tested executable with its documentation."""
import argparse
import hashlib
import re
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("platform", choices=("windows", "linux"))
    parser.add_argument("tag", help="Version tag, e.g. v0.1.0")
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"v\d+\.\d+\.\d+", args.tag):
        parser.error("tag must be a stable version like v0.1.0")
    if not args.binary.is_file():
        parser.error(f"binary not found: {args.binary}")

    args.output.mkdir(parents=True, exist_ok=True)
    binary_name = "strafe-lab.exe" if args.platform == "windows" else "strafe-lab"
    files = [(args.binary, binary_name), (ROOT / "README.md", "README.md"), (ROOT / "LICENSE", "LICENSE")]
    stem = f"Strafe-Lab-{args.tag}-{args.platform}-x86_64"
    if args.platform == "windows":
        archive = args.output / f"{stem}.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
            for source, name in files:
                bundle.write(source, name)
    else:
        archive = args.output / f"{stem}.tar.gz"
        with tarfile.open(archive, "w:gz") as bundle:
            for source, name in files:
                bundle.add(source, arcname=name)

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    checksum = archive.with_name(archive.name + ".sha256")
    checksum.write_text(f"{digest}  {archive.name}\n", encoding="ascii")
    print(f"Created {archive} and {checksum}")


if __name__ == "__main__":
    main()
