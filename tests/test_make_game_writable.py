"""Game-free: a game folder the build cannot write to stops at once with a plain reason, not minutes later inside nup.py with a
bare "PermissionError: [WinError 5]" (a game folder under Program Files).

Run with: python -I tests/test_make_game_writable.py
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_writable_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class Writable(unittest.TestCase):
    def stopped(self, out, env=None, error=None):
        printed = io.StringIO()
        real_open = open

        def refusing_open(path, *args, **kwargs):
            if os.path.basename(path) == ".longshot_write_check":
                raise error or PermissionError(13, "Access is denied")
            return real_open(path, *args, **kwargs)

        with contextlib.redirect_stdout(printed), mock.patch("builtins.open", refusing_open), \
                mock.patch.dict(os.environ, env or {}), self.assertRaises(SystemExit) as stop:
            make_game.check_writable(out)
        self.assertEqual(stop.exception.code, 1)
        return printed.getvalue()

    def test_a_writable_folder_passes_and_leaves_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "authored game folder")
            make_game.check_writable(out)
            self.assertTrue(os.path.isdir(out))
            self.assertEqual(os.listdir(out), [])

    def test_a_refused_folder_stops_with_the_folder_named(self):
        with tempfile.TemporaryDirectory() as d:
            text = self.stopped(d)
        self.assertTrue(text.startswith("@@error The game folder "), text)
        self.assertIn(d, text)
        self.assertIn("Access is denied", text)
        self.assertIn("choose a game folder of your own", text)
        self.assertNotIn("Program Files", text)

    def test_program_files_is_named_as_the_reason(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "authored game folder")
            text = self.stopped(out, {"ProgramFiles(x86)": d})
        self.assertIn("under Program Files", text)

    def test_program_files_is_not_blamed_for_another_error(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "authored game folder")
            text = self.stopped(out, {"ProgramFiles(x86)": d}, error=OSError(28, "No space left on device"))
        self.assertIn("No space left on device", text)
        self.assertNotIn("Program Files", text)

    def test_a_blocked_write_with_another_error_stops_the_same_way(self):
        # (#2792: the probe refused in a folder just made with "[Errno 2] No such file or directory")
        with tempfile.TemporaryDirectory() as d:
            text = self.stopped(d, error=FileNotFoundError(2, "No such file or directory"))
        self.assertTrue(text.startswith("@@error The game folder "), text)
        self.assertIn(d, text)
        self.assertIn("No such file or directory", text)
        self.assertIn("choose a game folder of your own", text)


class XboxTemporaryFiles(unittest.TestCase):
    def test_workspace_temp_is_writable_and_inherited_by_isolated_children(self):
        with tempfile.TemporaryDirectory() as work, mock.patch.dict(os.environ, {"LONGSHOT_XBOX": "1"}), \
                mock.patch.object(tempfile, "tempdir", tempfile.tempdir):
            make_game.xbox_build_temp(work)
            expected = os.path.join(work, "temp")
            self.assertEqual(tempfile.gettempdir(), expected)
            self.assertEqual(os.environ["TMP"], expected)
            child = subprocess.run([sys.executable, "-I", "-c",
                                    "import os,tempfile; print(os.path.normcase(tempfile.gettempdir()))"],
                                   capture_output=True, text=True, check=True)
            self.assertEqual(child.stdout.strip(), os.path.normcase(expected))
            self.assertEqual(os.listdir(expected), [])

    def test_denied_temp_reports_the_work_folder(self):
        with tempfile.TemporaryDirectory() as work, \
                mock.patch.object(make_game.build_temp, "temporary_directory", side_effect=PermissionError("access denied")), \
                contextlib.redirect_stdout(io.StringIO()) as output, self.assertRaises(SystemExit):
            make_game.xbox_build_temp(work)
        self.assertIn("build temporary folder", output.getvalue())
        self.assertIn("access denied", output.getvalue())

    def test_denied_work_folder_reports_the_temp_folder(self):
        with mock.patch.object(make_game.os, "makedirs", side_effect=PermissionError("access denied")), \
                contextlib.redirect_stdout(io.StringIO()) as output, self.assertRaises(SystemExit):
            make_game.xbox_build_temp("unwritable-work")
        self.assertIn(os.path.join("unwritable-work", "temp"), output.getvalue())
        self.assertIn("access denied", output.getvalue())

    @unittest.skipUnless(os.name == "nt", "Windows app-container directory mode")
    def test_xbox_temp_inherits_parent_permissions_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as work, mock.patch.dict(os.environ, {"LONGSHOT_XBOX": "1"}):
            real_mkdir = os.mkdir
            modes = []

            def mkdir(path, mode=0o777):
                modes.append(mode)
                return real_mkdir(path, mode)

            with mock.patch.object(make_game.build_temp.os, "mkdir", side_effect=mkdir):
                with make_game.build_temp.temporary_directory(dir=work) as folder:
                    with open(os.path.join(folder, "input"), "w") as f:
                        f.write("ok")
            self.assertEqual(modes, [0o777])
            self.assertFalse(os.path.exists(folder))


if __name__ == "__main__":
    unittest.main()
