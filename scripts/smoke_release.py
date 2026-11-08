#!/usr/bin/env python3
"""Run the packaged server and check the embedded game without a browser."""
import argparse
import queue
import re
import subprocess
import threading
import urllib.request
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    binary = parser.parse_args().binary.resolve(strict=True)
    process = subprocess.Popen(
        [str(binary), "--no-open", "--port", "0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    try:
        output = queue.Queue()
        reader = threading.Thread(target=lambda: output.put(process.stdout.readline()), daemon=True)
        reader.start()
        try:
            line = output.get(timeout=15)
        except queue.Empty as error:
            raise RuntimeError("server did not announce its address within 15 seconds") from error
        match = re.search(r"http://127\.0\.0\.1:(\d+)/", line)
        if not match:
            raise RuntimeError(f"server did not announce a local address: {line!r}")
        base = match.group(0)
        with urllib.request.urlopen(base, timeout=5) as response:
            assert response.status == 200
            assert b"STRAFE" in response.read()
        with urllib.request.urlopen(base + "engine.wasm", timeout=5) as response:
            assert response.headers.get_content_type() == "application/wasm"
            assert response.read(4) == b"\0asm"
        print(f"Release binary serves the bundled game: {binary.name}")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        if process.stdout:
            process.stdout.close()


if __name__ == "__main__":
    main()
