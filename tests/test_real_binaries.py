"""Проверка парсера на настоящих Windows-бинарниках.

По умолчанию пропускается: тестам не нужен интернет. Чтобы запустить, укажите
переменную ``ZLRX580_REAL_BIN_DIR`` — каталог, в котором лежат распакованные
``.dll``/``.pyd`` (например, из wheel ``nvidia-cuda-runtime-cu11`` или
``cupy-cuda11x``)::

    pip download --no-deps --platform win_amd64 --only-binary=:all: \
        --python-version 3.11 -d /tmp/w nvidia-cuda-runtime-cu11
    python -m zipfile -e /tmp/w/*.whl /tmp/w/x
    ZLRX580_REAL_BIN_DIR=/tmp/w/x python -m unittest tests.test_real_binaries -v

Проверяется не «работает ли вообще», а конкретное: наш парсер находит те же
импортируемые символы и те же RVA слотов IAT, что и ``pefile``.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from zluda_rx580 import pe as pe_mod  # noqa: E402

REAL_BIN_DIR = os.environ.get("ZLRX580_REAL_BIN_DIR")

try:  # pragma: no cover
    import pefile as _pefile
except ImportError:  # pragma: no cover
    _pefile = None


def _binaries():
    if not REAL_BIN_DIR:
        return []
    found = []
    for root, _dirs, files in os.walk(REAL_BIN_DIR):
        for name in files:
            if name.lower().endswith((".dll", ".pyd")):
                found.append(os.path.join(root, name))
    return sorted(found)


@unittest.skipUnless(REAL_BIN_DIR, "set ZLRX580_REAL_BIN_DIR to a directory of real PE files")
@unittest.skipUnless(_pefile, "pefile is not installed")
class RealBinaryTests(unittest.TestCase):
    def test_matches_pefile_import_tables(self):
        binaries = _binaries()
        self.assertTrue(binaries, "no .dll/.pyd found under %s" % REAL_BIN_DIR)
        checked = 0
        for path in binaries:
            with open(path, "rb") as handle:
                blob = handle.read()
            try:
                theirs = _pefile.PE(data=blob)
            except Exception:
                continue
            if not hasattr(theirs, "DIRECTORY_ENTRY_IMPORT"):
                continue
            ours = pe_mod.PEImage(blob, path)
            ours_map = {
                d.dll: sorted((s.name, s.iat_rva) for s in d.slots)
                for d in ours.import_descriptors()
            }
            theirs_map = {
                entry.dll.decode("latin-1"): sorted(
                    (imp.name.decode("latin-1") if imp.name else None, imp.address)
                    for imp in entry.imports
                )
                for entry in theirs.DIRECTORY_ENTRY_IMPORT
            }
            # pefile в address кладёт RVA слота IAT — сравниваем именно это.
            self.assertEqual(
                sorted(ours_map), sorted(theirs_map), "descriptor set differs for %s" % path
            )
            for dll in ours_map:
                ours_slots = {name for name, _rva in ours_map[dll]}
                theirs_slots = {name for name, _rva in theirs_map[dll]}
                self.assertEqual(ours_slots, theirs_slots, "%s: %s" % (path, dll))
            checked += 1
        self.assertGreater(checked, 0, "no importable PE found in %s" % REAL_BIN_DIR)

    def test_finds_cudart_slots_when_present(self):
        from zluda_rx580.pinpatch import find_target_slots

        hits = 0
        for path in _binaries():
            slots = find_target_slots(path, 0x180000000)
            for dll, function, address, _delay in slots:
                hits += 1
                self.assertTrue(dll.lower().startswith("cudart"))
                self.assertIn(function, ("cudaHostAlloc", "cudaFreeHost"))
                self.assertGreater(address, 0x180000000)
        print("\nreal binaries with cudaHostAlloc/cudaFreeHost imports: %d" % hits)


if __name__ == "__main__":
    unittest.main()
