import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from tools.package_release import package


class PackageReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "SecondaryMotion"
        self.source.mkdir()
        self.input = self.source / "sample.dll"
        self.input.write_bytes(b"release binary" * 100)
        self.destination = self.root / "release.zip"

    def test_contents_paths_and_crc(self):
        (self.source / "data").mkdir()
        (self.source / "data/角色.json").write_text('{"Si":true}', encoding="utf-8")
        package(self.source, self.destination)
        with zipfile.ZipFile(self.destination) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(set(archive.namelist()), {
                "SecondaryMotion/sample.dll", "SecondaryMotion/data/角色.json"
            })
            self.assertEqual(archive.read("SecondaryMotion/sample.dll"), self.input.read_bytes())

    def test_transient_lock_retries(self):
        original_write = zipfile.ZipFile.write
        calls = 0

        def briefly_busy(archive, *args, **kwargs):
            nonlocal calls
            calls += 1
            if calls < 3:
                raise PermissionError("temporary lock")
            return original_write(archive, *args, **kwargs)

        with patch.object(zipfile.ZipFile, "write", briefly_busy):
            package(self.source, self.destination, retry_delay=0)
        self.assertEqual(calls, 3)
        with zipfile.ZipFile(self.destination) as archive:
            self.assertIsNone(archive.testzip())

    def test_permanent_lock_preserves_previous_archive(self):
        self.destination.write_bytes(b"previous release")
        with patch.object(zipfile.ZipFile, "write", side_effect=PermissionError("busy")):
            with self.assertRaises(PermissionError):
                package(self.source, self.destination, attempts=2, retry_delay=0)
        self.assertEqual(self.destination.read_bytes(), b"previous release")
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    def test_bad_crc_is_not_published(self):
        self.destination.write_bytes(b"previous release")
        with patch.object(zipfile.ZipFile, "testzip", return_value="sample.dll"):
            with self.assertRaisesRegex(RuntimeError, "integrity"):
                package(self.source, self.destination)
        self.assertEqual(self.destination.read_bytes(), b"previous release")

    def test_changed_input_is_not_published(self):
        original_write = zipfile.ZipFile.write

        def changed(archive, path, *args, **kwargs):
            original_write(archive, path, *args, **kwargs)
            Path(path).write_bytes(b"changed source")

        with patch.object(zipfile.ZipFile, "write", changed):
            with self.assertRaisesRegex(RuntimeError, "source changed"):
                package(self.source, self.destination)
        self.assertFalse(self.destination.exists())

    def test_missing_and_empty_sources_fail(self):
        with self.assertRaises(ValueError):
            package(self.root / "missing", self.destination)
        empty = self.root / "empty"
        empty.mkdir()
        with self.assertRaises(ValueError):
            package(empty, self.destination)

    def test_archive_cannot_include_itself(self):
        with self.assertRaises(ValueError):
            package(self.source, self.source / "release.zip")

    def test_cli_error_has_nonzero_exit(self):
        helper = Path(__file__).resolve().parents[1] / "tools/package_release.py"
        result = subprocess.run([sys.executable, str(helper), str(self.root / "missing"),
                                 str(self.destination)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("[ERROR] Package not created", result.stderr)
        self.assertFalse(self.destination.exists())

    @unittest.skipUnless(os.name == "nt", "Windows batch integration")
    def test_assembler_languages_and_failure_exit(self):
        repository = Path(__file__).resolve().parents[1]
        for language, helper_missing in [("EN", False), ("ZH", False), ("EN", True)]:
            with self.subTest(language=language, helper_missing=helper_missing):
                fixture = self.root / (language + str(helper_missing))
                fixture.mkdir()
                for filename in ("assemble_one.bat", "zh_names.py", "使用说明.txt"):
                    shutil.copyfile(repository / filename, fixture / filename)
                inputs = [
                    "dist/staging_app/SecondaryMotion.Manager.exe",
                    "bin/sbm.dll", "bin/d3dcompiler_47.dll", "bin/vulkan-1.dll",
                    "SecondaryMotion/data/characters.default.json",
                    "SecondaryMotion/presets/Default.json",
                    "SecondaryMotion/presets/Default.v3.0.0-bad.template.json",
                    "SecondaryMotion/presets/User.json",
                    "SecondaryMotion/runtime/config.json", "USER_GUIDE_EN.txt", "README.md",
                ]
                for name in inputs:
                    path = fixture / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text('{}', encoding="utf-8")
                if not helper_missing:
                    (fixture / "tools").mkdir()
                    shutil.copyfile(repository / "tools/package_release.py", fixture / "tools/package_release.py")
                result = subprocess.run(["cmd", "/d", "/c", str(fixture / "assemble_one.bat"),
                                         language, "v-test"], cwd=fixture, capture_output=True,
                                        text=True, errors="replace")
                archive_path = fixture / f"dist/ShakingBreastManager-v-test-{language}-win-x64.zip"
                if helper_missing:
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertFalse(archive_path.exists())
                    self.assertIn("[ERROR] zip EN failed", result.stdout)
                else:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    with zipfile.ZipFile(archive_path) as archive:
                        self.assertIsNone(archive.testzip())
                        expected_language = "en-US" if language == "EN" else "zh-CN"
                        self.assertEqual(archive.read("SecondaryMotion/default_lang.txt").decode().strip(), expected_language)
                        self.assertIn("SecondaryMotion/plugin/sbm.dll", archive.namelist())
                        if language == "ZH":
                            self.assertIn("SecondaryMotion/使用说明.txt", archive.namelist())

    @unittest.skipUnless(os.name == "nt", "Windows file sharing")
    def test_windows_shared_reader_and_exclusive_lock(self):
        create = ctypes.windll.kernel32.CreateFileW
        create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                           wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        create.restype = wintypes.HANDLE
        close = ctypes.windll.kernel32.CloseHandle
        close.argtypes = [wintypes.HANDLE]
        close.restype = wintypes.BOOL
        # A compatible reader must not prevent packaging.
        for share in (1, 0):
            handle = create(str(self.input), 0x80000000, share, None, 3, 0x80, None)
            self.assertNotEqual(handle, wintypes.HANDLE(-1).value)
            try:
                if share == 1:
                    package(self.source, self.destination)
                    previous = self.destination.read_bytes()
                else:
                    with self.assertRaises(PermissionError):
                        package(self.source, self.destination, attempts=2, retry_delay=0)
                    self.assertEqual(self.destination.read_bytes(), previous)
            finally:
                close(handle)


if __name__ == "__main__":
    unittest.main()
