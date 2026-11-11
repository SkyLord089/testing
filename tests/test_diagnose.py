"""Тесты диагностики zluda_rx580.diagnose.

Появились после того, как на живой машине rocBLAS не смог найти
``TensileLibrary.dat`` для gfx803 и аварийно завершил процесс. Поиск
библиотеки — единственная часть этой проверки, которую можно исполнить без
Windows и без HIP SDK, поэтому проверяется именно она.
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from zluda_rx580 import diagnose  # noqa: E402


class TensileSearchTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.lib = self.root / "bin" / "rocblas" / "library"
        self.lib.mkdir(parents=True)
        self._saved = os.environ.get("ROCM_PATH")
        os.environ["ROCM_PATH"] = str(self.root)

    def tearDown(self):
        if self._saved is None:
            os.environ.pop("ROCM_PATH", None)
        else:
            os.environ["ROCM_PATH"] = self._saved

    def _touch(self, name: str) -> None:
        (self.lib / name).write_bytes(b"x")

    def test_finds_library_for_the_requested_arch(self):
        self._touch("TensileLibrary_gfx803.dat")
        hits = diagnose._find_tensile_libraries("gfx803")
        self.assertEqual(len(hits), 1)
        self.assertTrue(hits[0].endswith("TensileLibrary_gfx803.dat"))

    def test_other_architectures_are_filtered_out(self):
        self._touch("TensileLibrary_gfx803.dat")
        self._touch("TensileLibrary_gfx1030.dat")
        hits = diagnose._find_tensile_libraries("gfx803")
        self.assertEqual([os.path.basename(h) for h in hits], ["TensileLibrary_gfx803.dat"])

    def test_generic_library_is_also_reported(self):
        # rocBLAS пробует TensileLibrary.dat последним, поэтому он важен
        self._touch("TensileLibrary.dat")
        hits = diagnose._find_tensile_libraries("gfx803")
        self.assertEqual([os.path.basename(h) for h in hits], ["TensileLibrary.dat"])

    def test_missing_library_gives_empty_result(self):
        self.assertEqual(diagnose._find_tensile_libraries("gfx803"), [])

    def test_search_does_not_walk_forever(self):
        deep = self.root / "a" / "b" / "c" / "d" / "e"
        deep.mkdir(parents=True)
        (deep / "TensileLibrary_gfx803.dat").write_bytes(b"x")
        self.assertEqual(diagnose._find_tensile_libraries("gfx803"), [])

    def test_candidate_order_matches_rocblas(self):
        """Порядок каталогов обязан совпадать с tensile_host.cpp:622-696."""
        self.assertEqual(
            diagnose.TENSILE_CANDIDATES,
            ("../../Tensile/library", "library", "rocblas/library"),
        )
        self.assertEqual(
            diagnose.TENSILE_FILENAMES,
            ("TensileLibrary_lazy_%s.dat", "TensileLibrary_%s.dat", "TensileLibrary.dat"),
        )


if __name__ == "__main__":
    unittest.main()
