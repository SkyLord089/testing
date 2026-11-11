"""Регрессионные тесты на разбор WinAPI в zluda_rx580.pinpatch.

Появились после реальной ошибки на машине пользователя:
``GetModuleFileNameExW`` брали из ``kernel32.dll``, хотя функция живёт в
``psapi.dll`` (в kernel32 есть только ``K32GetModuleFileNameExW``). Падение
выглядело как ``AttributeError: function 'GetModuleFileNameExW' not found``
и полностью отключало патч ``cudaHostAlloc``.

Тесты работают на любой ОС:

* принадлежность функции нужной DLL проверяется по исходнику модуля —
  это ровно то утверждение, которое тогда и нарушилось;
* на не-Windows все точки входа обязаны давать понятную ``PatchError``,
  а не ``AttributeError`` из недр ctypes.
"""

from __future__ import annotations

import os
import pathlib
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from zluda_rx580 import pinpatch  # noqa: E402

SOURCE = pathlib.Path(pinpatch.__file__).read_text(encoding="utf-8")


class PsapiOwnershipTests(unittest.TestCase):
    def test_module_filename_comes_from_psapi(self):
        self.assertIn("GetModuleFileNameExW", pinpatch.PSAPI_FUNCTIONS)
        self.assertIn("EnumProcessModulesEx", pinpatch.PSAPI_FUNCTIONS)

    def test_kernel32_never_provides_psapi_functions(self):
        for name in pinpatch.PSAPI_FUNCTIONS:
            self.assertNotIn(
                "kernel32.%s" % name,
                SOURCE,
                "%s is exported by psapi.dll, not kernel32.dll" % name,
            )

    def test_psapi_handle_is_used_at_the_call_site(self):
        self.assertIn("psapi.GetModuleFileNameExW(", SOURCE)
        self.assertIn("psapi.EnumProcessModulesEx(", SOURCE)

    def test_all_winapi_calls_use_a_known_dll_handle(self):
        handles = set(re.findall(r"\b(\w+)\.[A-Z]\w+\.", SOURCE))
        self.assertTrue(handles, "no WinAPI calls found — test is out of date")
        self.assertEqual(
            handles,
            {"kernel32", "psapi"},
            "unexpected WinAPI handle(s): %s" % sorted(handles - {"kernel32", "psapi"}),
        )


class WindowsGuardTests(unittest.TestCase):
    """На не-Windows ошибка должна быть понятной, а не AttributeError."""

    @unittest.skipIf(os.name == "nt", "guard only applies off Windows")
    def test_loaded_modules_raises_patch_error(self):
        with self.assertRaises(pinpatch.PatchError):
            pinpatch.loaded_modules()

    @unittest.skipIf(os.name == "nt", "guard only applies off Windows")
    def test_resolve_crt_allocators_raises_patch_error(self):
        with self.assertRaises(pinpatch.PatchError):
            pinpatch.resolve_crt_allocators()

    @unittest.skipIf(os.name == "nt", "guard only applies off Windows")
    def test_write_pointer_raises_patch_error(self):
        with self.assertRaises(pinpatch.PatchError):
            pinpatch.write_pointer(0x1000, 0x2000)

    @unittest.skipIf(os.name == "nt", "guard only applies off Windows")
    def test_install_raises_patch_error(self):
        with self.assertRaises(pinpatch.PatchError):
            pinpatch.install()

    @unittest.skipIf(os.name == "nt", "guard only applies off Windows")
    def test_thunk_page_raises_patch_error(self):
        with self.assertRaises(pinpatch.PatchError):
            pinpatch.ThunkPage(0x1000, 0x2000)


if __name__ == "__main__":
    unittest.main()
