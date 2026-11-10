"""Тесты среды выполнения: переменные окружения и хук импорта torch.

Работает на любой ОС: настоящий ``torch`` не нужен — вместо него во временном
каталоге создаётся подставной пакет с теми атрибутами, к которым обращается
:mod:`zluda_rx580.runtime`. Проверяется именно наш код, а не заглушка:
``setup_env`` действительно пишет переменные, ``activate.install_import_hook``
действительно срабатывает после ``import torch``.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from zluda_rx580 import runtime  # noqa: E402
from zluda_rx580 import activate  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FAKE_TORCH = textwrap.dedent(
    """
    __version__ = "2.7.1+cu118"

    class _Cuda:
        @staticmethod
        def is_available():
            return False

    class _Cudnn:
        enabled = True

    class _CudaBackend:
        @staticmethod
        def enable_flash_sdp(value):
            _FLAGS["flash"] = value

        @staticmethod
        def enable_mem_efficient_sdp(value):
            _FLAGS["mem_efficient"] = value

        @staticmethod
        def enable_math_sdp(value):
            _FLAGS["math"] = value

    class _Backend:
        cudnn = _Cudnn()
        cuda = _CudaBackend()

    class _Jit:
        @staticmethod
        def script(function, *_, **__):
            raise AssertionError("jit.script must not run the real implementation")

    class _DynamoConfig:
        suppress_errors = False

    class _Dynamo:
        config = _DynamoConfig()

    _FLAGS = {}
    backends = _Backend()
    cuda = _Cuda()
    jit = _Jit()
    _dynamo = _Dynamo()

    def stft(input, window=None, *args, **kwargs):
        return input

    def istft(input, window=None, *args, **kwargs):
        return input

    def topk(input, *args, **kwargs):
        return input

    class _Tensor:
        def item(self):
            return 0

        def __bool__(self):
            return True

        def __float__(self):
            return 0.0

        def __int__(self):
            return 0

        def __index__(self):
            return 0

    Tensor = _Tensor

    def count_nonzero(input, dim=None):
        return 0
    """
)


class EnvSetupTests(unittest.TestCase):
    def setUp(self):
        self._saved = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._saved)

    def test_disabled_switch_skips_everything(self):
        os.environ["ZLUDA_RX580_DISABLE"] = "1"
        os.environ.pop("DISABLE_ADDMM_CUDA_LT", None)
        self.assertEqual(runtime.setup_env(), {})
        self.assertNotIn("DISABLE_ADDMM_CUDA_LT", os.environ)
        self.assertTrue(runtime.is_disabled())

    def test_sets_disable_addmm_cuda_lt(self):
        os.environ.pop("ZLUDA_RX580_DISABLE", None)
        os.environ.pop("DISABLE_ADDMM_CUDA_LT", None)
        changes = runtime.setup_env()
        self.assertEqual(changes.get("DISABLE_ADDMM_CUDA_LT"), "1")
        self.assertEqual(os.environ["DISABLE_ADDMM_CUDA_LT"], "1")

    def test_does_not_override_user_value(self):
        os.environ.pop("ZLUDA_RX580_DISABLE", None)
        os.environ["DISABLE_ADDMM_CUDA_LT"] = "0"
        runtime.setup_env()
        self.assertEqual(os.environ["DISABLE_ADDMM_CUDA_LT"], "0")

    def test_host_register_option_is_opt_in(self):
        os.environ.pop("ZLUDA_RX580_DISABLE", None)
        os.environ.pop("PYTORCH_CUDA_ALLOC_CONF", None)
        os.environ.pop("ZLUDA_RX580_HOST_REGISTER", None)
        runtime.setup_env()
        self.assertNotIn("PYTORCH_CUDA_ALLOC_CONF", os.environ)

        os.environ["ZLUDA_RX580_HOST_REGISTER"] = "1"
        runtime.setup_env()
        self.assertEqual(
            os.environ["PYTORCH_CUDA_ALLOC_CONF"], "pinned_use_cuda_host_register:True"
        )

    def test_host_register_appends_to_existing_config(self):
        os.environ.pop("ZLUDA_RX580_DISABLE", None)
        os.environ["PYTORCH_CUDA_ALLOC_CONF"] = "expandable_segments:True"
        os.environ["ZLUDA_RX580_HOST_REGISTER"] = "1"
        runtime.setup_env()
        self.assertEqual(
            os.environ["PYTORCH_CUDA_ALLOC_CONF"],
            "expandable_segments:True,pinned_use_cuda_host_register:True",
        )

    def test_hide_rocm_is_opt_in(self):
        os.environ.pop("ZLUDA_RX580_DISABLE", None)
        os.environ["ROCM_HOME"] = r"C:\Program Files\AMD\ROCm\5.7"
        os.environ["PATH"] = r"C:\Program Files\AMD\ROCm\5.7\bin;C:\Windows"
        runtime.setup_env()
        self.assertIn("ROCM_HOME", os.environ)

        os.environ["ZLUDA_RX580_HIDE_ROCM"] = "1"
        runtime.setup_env()
        self.assertNotIn("ROCM_HOME", os.environ)
        self.assertNotIn("rocm", os.environ["PATH"].lower())


class TorchTweaksTests(unittest.TestCase):
    def setUp(self):
        self.module = _import_fake_torch()

    def test_tweaks_are_applied(self):
        done = runtime.apply_torch_tweaks(self.module)
        self.assertFalse(self.module.backends.cudnn.enabled)
        self.assertEqual(self.module._FLAGS["flash"], False)
        self.assertEqual(self.module._FLAGS["mem_efficient"], False)
        self.assertEqual(self.module._FLAGS["math"], True)
        self.assertTrue(self.module._dynamo.config.suppress_errors)
        self.assertIn("jit.script stubbed", done)

    def test_jit_script_stub_does_not_call_original(self):
        runtime.apply_torch_tweaks(self.module)
        function = self.module.jit.script(lambda x: x)
        self.assertTrue(callable(function))

    def test_idempotent(self):
        first = runtime.apply_torch_tweaks(self.module)
        second = runtime.apply_torch_tweaks(self.module)
        self.assertTrue(first)
        self.assertFalse([item for item in second if item.endswith("-> cpu")])


class ImportHookTests(unittest.TestCase):
    """Хук срабатывает после ``import torch`` в отдельном интерпретаторе."""

    def test_hook_runs_after_import(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "torch.py"), "w", encoding="utf-8") as handle:
                handle.write(FAKE_TORCH)
            script = textwrap.dedent(
                """
                import sys
                sys.path.insert(0, %r)
                sys.path.insert(0, %r)
                import os
                os.environ["ZLUDA_RX580_FORCE"] = "1"
                os.environ["ZLUDA_RX580_NO_PIN_PATCH"] = "1"
                import zluda_rx580.activate  # noqa: F401  (ставит хук)
                import torch
                print("CONFIGURED", getattr(torch, "_zluda_rx580_configured", False))
                print("CUDNN", torch.backends.cudnn.enabled)
                print("ADDMM", os.environ.get("DISABLE_ADDMM_CUDA_LT"))
                """
            ) % (tmp, REPO_ROOT)
            completed = subprocess.run(
                [sys.executable, "-c", script],
                capture_output=True,
                text=True,
                timeout=120,
            )
            output = completed.stdout
            self.assertIn("CONFIGURED True", output, completed.stderr)
            self.assertIn("CUDNN False", output)
            self.assertIn("ADDMM 1", output)

    def test_missing_tensor_does_not_abort_other_stages(self):
        """Регрессия: сбой в одной стадии не должен отменять cudnn/count_nonzero."""
        import types

        module = types.ModuleType("partial_torch")
        exec(compile(FAKE_TORCH, "<fake>", "exec"), module.__dict__)
        del module.Tensor  # как будто torch ещё не до конца инициализирован
        done = runtime.apply_pinned_shims(module)
        self.assertEqual(done, ["count_nonzero -> (x != 0).sum()"])
        tweaks = runtime.apply_torch_tweaks(module)
        self.assertIn("cudnn disabled", tweaks)
        self.assertFalse(module.backends.cudnn.enabled)

    def test_hook_is_inactive_when_disabled(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "torch.py"), "w", encoding="utf-8") as handle:
                handle.write(FAKE_TORCH)
            script = textwrap.dedent(
                """
                import sys, os
                sys.path.insert(0, %r)
                sys.path.insert(0, %r)
                os.environ["ZLUDA_RX580_DISABLE"] = "1"
                os.environ["ZLUDA_RX580_FORCE"] = "1"
                import zluda_rx580.activate  # noqa: F401
                import torch
                print("CONFIGURED", getattr(torch, "_zluda_rx580_configured", False))
                """
            ) % (tmp, REPO_ROOT)
            completed = subprocess.run(
                [sys.executable, "-c", script],
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertIn("CONFIGURED False", completed.stdout, completed.stderr)


def _import_fake_torch():
    """Собирает подставной модуль torch прямо в памяти (без файловой системы)."""
    import types

    module = types.ModuleType("fake_torch_for_tests")
    exec(compile(FAKE_TORCH, "<fake_torch>", "exec"), module.__dict__)
    return module


if __name__ == "__main__":
    unittest.main()
