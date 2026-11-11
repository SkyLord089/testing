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
import tempfile
import re
import sys
import pathlib
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


class SurveyTests(unittest.TestCase):
    """Опрос не должен ни писать, ни падать, ни молчать."""

    @unittest.skipIf(os.name == "nt", "проверяем поведение вне Windows")
    def test_survey_raises_patch_error_off_windows(self):
        with self.assertRaises(pinpatch.PatchError):
            pinpatch.survey()

    def test_survey_is_called_after_torch_is_imported(self):
        """check_patch вызывал install() до import torch и падал впустую."""
        source = pathlib.Path("zluda_rx580/diagnose.py").read_text(encoding="utf-8")
        body = source[source.index("def check_patch("):]
        body = body[: body.index("\ndef ")]
        self.assertLess(
            body.index("torch = _torch()"),
            body.index("pinpatch.survey()"),
            "torch должен быть импортирован до опроса модулей",
        )
        self.assertIn("pinpatch.install()", body)

    def test_report_lists_scanned_modules_even_without_slots(self):
        report = pinpatch.PatchReport()
        report.scanned = ["torch_cuda.dll", "c10_cuda.dll"]
        report.skipped = ["torch_cuda.dll", "c10_cuda.dll"]
        text = report.format()
        self.assertIn("scanned 2 module(s)", text)
        self.assertIn("torch_cuda.dll", text)
        self.assertIn("IAT slots for cudaHostAlloc/cudaFreeHost: 0", text)


class UndoTests(unittest.TestCase):
    def test_dry_run_slots_are_not_zeroed(self):
        """undo() после dry-run раньше писал 0 в чужой IAT."""
        report = pinpatch.PatchReport(
            slots=[pinpatch.PatchedSlot("m.dll", 0x1000, "cudart64_110.dll", "cudaHostAlloc", 0x2000, False, 0)]
        )
        patch = pinpatch.HostAllocPatch(report, page=None)
        written = []
        original = pinpatch.write_pointer
        pinpatch.write_pointer = lambda address, value: written.append((address, value))
        try:
            patch.undo()
        finally:
            pinpatch.write_pointer = original
        self.assertEqual(written, [], "слот с неизвестным прежним значением перезаписывать нельзя")

    def test_real_slots_are_restored(self):
        report = pinpatch.PatchReport(
            slots=[
                pinpatch.PatchedSlot("m.dll", 0x1000, "cudart64_110.dll", "cudaHostAlloc", 0x2000, False, 0xBEEF)
            ]
        )
        patch = pinpatch.HostAllocPatch(report, page=None)
        written = []
        original = pinpatch.write_pointer
        pinpatch.write_pointer = lambda address, value: written.append((address, value))
        try:
            patch.undo()
        finally:
            pinpatch.write_pointer = original
        self.assertEqual(written, [(0x2000, 0xBEEF)])


class ImportSummaryTests(unittest.TestCase):
    """Сканер обязан находить слоты, когда они есть.

    На живой машине опрос дал 0 слотов в 38 модулях. Чтобы считать это
    результатом, а не сбоем разбора, нужен положительный контроль: образ,
    который точно импортирует cudaHostAlloc из cudart64_110.dll.
    """

    def setUp(self):
        from tests.test_pe import build_synthetic_pe

        self.image = build_synthetic_pe()
        self.path = pathlib.Path(tempfile.gettempdir()) / "zlrx_synthetic_module.dll"
        self.path.write_bytes(self.image)

    def tearDown(self):
        try:
            self.path.unlink()
        except OSError:
            pass

    def test_detects_iat_slots_in_a_module_that_has_them(self):
        summary = pinpatch.import_summary(str(self.path))
        self.assertTrue(summary.parsed, summary.error)
        self.assertIn("cudart64_110.dll", summary.import_dlls)
        # Фикстура собирается с delay_load=True, поэтому cudaHostAlloc
        # встречается дважды: обычный импорт и отложенный.
        self.assertEqual(
            sorted(summary.iat_matches),
            [
                "cudart64_110.dll!cudaFreeHost",
                "cudart64_110.dll!cudaHostAlloc",
                "cudart64_110.dll!cudaHostAlloc (delay)",
            ],
        )
        self.assertEqual(summary.delay_dlls, ["cudart64_110.dll"])
        self.assertIn("есть слоты IAT", summary.verdict())

    def test_find_target_slots_agrees_with_the_summary(self):
        slots = pinpatch.find_target_slots(str(self.path), 0x10000)
        self.assertEqual(
            sorted(name for _d, name, _a, _dl in slots),
            ["cudaFreeHost", "cudaHostAlloc", "cudaHostAlloc"],
        )
        # отложенный импорт помечен отдельно - его слот заполняет загрузчик позже
        self.assertEqual([delay for _d, _n, _a, delay in slots].count(True), 1)

    def test_raw_hits_without_iat_mean_dynamic_resolution(self):
        summary = pinpatch.ImportSummary("m.dll", parsed=True, import_dlls=["kernel32.dll"])
        summary.raw_name_hits = ["cudaHostAlloc"]
        self.assertIn("GetProcAddress", summary.verdict())

    def test_cudart_itself_is_not_reported_as_dynamic(self):
        summary = pinpatch.ImportSummary("cudart64_110.dll", parsed=True, import_dlls=["kernel32.dll"])
        summary.raw_name_hits = ["cudaHostAlloc"]
        self.assertIn("экспортирует", summary.verdict())

    def test_missing_file_is_reported_not_crashing(self):
        summary = pinpatch.import_summary(str(self.path) + ".absent")
        self.assertFalse(summary.parsed)
        self.assertIn("не разобран", summary.verdict())
