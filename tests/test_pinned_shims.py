"""Тесты Python-заплаток против настоящего PyTorch.

Запускаются только если в окружении есть ``torch`` (``pip install torch``).
GPU не нужен: ветка для CUDA проверяется временной подменой ``Tensor.is_cuda`` —
так исполняется **тот же самый код**, который сработает на видеокарте
(``self.detach().to("cpu", copy=True)`` -> исходная реализация), только данные
лежат в обычной памяти.

Все изменения в типе ``torch.Tensor`` откатываются в ``tearDown``.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

try:  # pragma: no cover
    import torch  # type: ignore
except ImportError:  # pragma: no cover
    torch = None

from zluda_rx580 import runtime  # noqa: E402

PATCHED_NAMES = ("item", "__bool__", "__float__", "__int__", "__index__")


@unittest.skipUnless(torch, "torch is not installed")
class PinnedShimTests(unittest.TestCase):
    def setUp(self):
        self.saved = {name: getattr(torch.Tensor, name) for name in PATCHED_NAMES}
        self.saved_count = torch.count_nonzero
        self.saved_is_cuda = torch.Tensor.is_cuda
        self.saved_flag = getattr(torch, "_zluda_rx580_pin_shims", None)
        os.environ.pop("ZLUDA_RX580_NO_PIN_SHIM", None)

    def tearDown(self):
        for name, value in self.saved.items():
            setattr(torch.Tensor, name, value)
        torch.count_nonzero = self.saved_count
        torch.Tensor.is_cuda = self.saved_is_cuda
        if self.saved_flag is None:
            if hasattr(torch, "_zluda_rx580_pin_shims"):
                delattr(torch, "_zluda_rx580_pin_shims")
        else:
            torch._zluda_rx580_pin_shims = self.saved_flag

    def _pretend_cuda(self):
        torch.Tensor.is_cuda = property(lambda self: True)

    # -- установка --------------------------------------------------------

    def test_shims_are_installed(self):
        applied = runtime.apply_pinned_shims(torch)
        self.assertEqual(
            sorted(applied),
            sorted(
                ["Tensor.%s -> cpu" % name for name in PATCHED_NAMES]
                + ["count_nonzero -> (x != 0).sum()"]
            ),
        )
        for name in PATCHED_NAMES:
            self.assertTrue(
                getattr(getattr(torch.Tensor, name), "_zluda_rx580_pin_shim", False),
                "Tensor.%s was not replaced" % name,
            )

    def test_switch_disables_shims(self):
        os.environ["ZLUDA_RX580_NO_PIN_SHIM"] = "1"
        try:
            self.assertEqual(runtime.apply_pinned_shims(torch), [])
        finally:
            os.environ.pop("ZLUDA_RX580_NO_PIN_SHIM", None)

    def test_idempotent(self):
        self.assertTrue(runtime.apply_pinned_shims(torch))
        self.assertEqual(runtime.apply_pinned_shims(torch), [])

    # -- значения в «CUDA-ветке» -----------------------------------------

    def test_scalar_operations_stay_correct(self):
        runtime.apply_pinned_shims(torch)
        self._pretend_cuda()

        scalar = torch.arange(1, 1025, dtype=torch.float32).sum()
        expected = 524800.0
        self.assertEqual(scalar.item(), expected)
        self.assertTrue(bool(scalar))
        self.assertEqual(int(scalar), 524800)
        self.assertEqual(float(scalar), expected)

        zero = torch.zeros((), dtype=torch.float32)
        self.assertFalse(bool(zero))
        self.assertEqual(zero.item(), 0.0)

        negative = torch.tensor(-7.5)
        self.assertEqual(int(negative), -7)
        self.assertTrue(bool(negative))

    def test_index_protocol_still_works(self):
        runtime.apply_pinned_shims(torch)
        self._pretend_cuda()
        index = torch.tensor(5)
        data = list(range(10))
        self.assertEqual(data[index.__index__()], 5)

    def test_count_nonzero_matches_original(self):
        truth = torch.arange(0, 64, dtype=torch.int64) > 7
        expected = int(self.saved_count(truth).item())
        self.assertEqual(expected, 56)

        runtime.apply_pinned_shims(torch)
        self._pretend_cuda()
        self.assertEqual(int(torch.count_nonzero(truth).item()), expected)

        two_dim = torch.arange(0, 24, dtype=torch.int64).reshape(4, 6) > 3
        self.assertTrue(
            torch.equal(
                torch.count_nonzero(two_dim, dim=1),
                self.saved_count(two_dim, dim=1),
            )
        )

    def test_multi_element_bool_still_raises(self):
        runtime.apply_pinned_shims(torch)
        self._pretend_cuda()
        with self.assertRaises(RuntimeError):
            bool(torch.zeros(3))

    # -- регрессия на CPU -------------------------------------------------

    def test_cpu_path_is_untouched(self):
        runtime.apply_pinned_shims(torch)
        scalar = torch.arange(1, 17, dtype=torch.float32).sum()
        self.assertEqual(scalar.item(), 136.0)
        self.assertEqual(int(torch.count_nonzero(torch.arange(0, 10) > 4)), 5)
        self.assertTrue(bool(torch.tensor(1)))


if __name__ == "__main__":
    unittest.main()
