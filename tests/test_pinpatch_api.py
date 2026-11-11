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


class ProcessModuleApiTests(unittest.TestCase):
    """Принадлежность функций DLL и обязательный дескриптор процесса."""

    def test_both_functions_are_declared_with_fallbacks(self):
        declared = dict(pinpatch.PROCESS_MODULE_FUNCTIONS)
        self.assertEqual(
            set(declared), {"EnumProcessModulesEx", "GetModuleFileNameExW"}
        )
        for name, candidates in declared.items():
            dlls = {dll for dll, _export in candidates}
            self.assertEqual(
                dlls,
                {"kernel32", "psapi"},
                "%s должен искаться и в kernel32 (K32-вариант), и в psapi" % name,
            )
            for _dll, export in candidates:
                self.assertTrue(
                    export.endswith(name) or export == "K32" + name,
                    "неожиданное имя экспорта %r для %s" % (export, name),
                )

    def test_resolver_dict_matches_the_tuple(self):
        self.assertEqual(
            pinpatch.PROCESS_MODULE_FUNCTIONS_DICT,
            dict(pinpatch.PROCESS_MODULE_FUNCTIONS),
        )

    def test_process_handle_is_passed_not_null(self):
        """GetLastError=6 на машине пользователя: NULL вместо дескриптора."""
        self.assertIn("GetCurrentProcess", SOURCE)
        self.assertIn("process = current_process()", SOURCE)
        self.assertIn("process, handles", SOURCE)
        self.assertIn("module_file_name(process, handle", SOURCE)
        for forbidden in ("None, handles", "GetModuleFileNameExW(None"):
            self.assertNotIn(forbidden, SOURCE, "NULL вместо дескриптора процесса")

    def test_only_kernel32_and_psapi_are_consulted(self):
        """Прямых обращений к атрибутам DLL больше нет - только резолвер."""
        dlls = {dll for _n, cands in pinpatch.PROCESS_MODULE_FUNCTIONS for dll, _e in cands}
        self.assertEqual(dlls, {"kernel32", "psapi"})
        handles = set(re.findall(r"\b(\w+)\.[A-Z]\w+\.", SOURCE))
        self.assertEqual(
            handles,
            {"kernel32"},
            "прямое обращение к функции DLL в обход резолвера: %s"
            % sorted(handles - {"kernel32"}),
        )


class ResolverTests(unittest.TestCase):
    @unittest.skipIf(os.name == "nt", "проверяем поведение вне Windows")
    def test_unknown_function_raises_patch_error(self):
        with self.assertRaises(pinpatch.PatchError):
            pinpatch._resolve_process_api("NoSuchFunction")

    @unittest.skipIf(os.name == "nt", "проверяем поведение вне Windows")
    def test_current_process_raises_patch_error(self):
        with self.assertRaises(pinpatch.PatchError):
            pinpatch.current_process()


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
