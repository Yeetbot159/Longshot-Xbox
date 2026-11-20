"""Exercise the staged Xbox builder without game files.

Run on Windows: python -I tests/test_xbox_tool_package.py <staged tools directory>
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


TOOLS = Path(sys.argv.pop(1)).resolve()
PYTHON = TOOLS / "python" / "python.exe"


class XboxToolPackageTests(unittest.TestCase):
    def test_host_gpu_includes_are_packaged(self):
        includes = set()
        for source in (TOOLS / "host").rglob("*"):
            if source.suffix not in (".c", ".cpp", ".h"):
                continue
            text = source.read_text(encoding="utf-8")
            includes.update(re.findall(r'#\s*include\s*["<][^">]*(gpu[/\\][^">]+)', text))
        includes.add("gpu/render/nvn_api.h")
        self.assertTrue(includes)
        for name in sorted(includes):
            with self.subTest(header=name):
                self.assertTrue((TOOLS / name.replace("\\", "/")).is_file(), name)
        code = "\n".join('#include "%s"' % name.replace("\\", "/") for name in sorted(includes))
        compiler = TOOLS / "llvm-mingw" / "bin" / "clang++.exe"
        result = subprocess.run([str(compiler), "--target=x86_64-w64-mingw32", "-fsyntax-only",
                                 "-x", "c++", "-I", str(TOOLS), "-"], input=code,
                                capture_output=True, text=True, cwd=TOOLS)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_helpers_load_without_rebuilding(self):
        script = """
import sys
from unittest import mock
sys.path.insert(0, sys.argv[1] + r'\\nx2')
sys.path.insert(0, sys.argv[1] + r'\\third_party\\a64-kit\\lift')
import a64emit, zbic
with mock.patch('subprocess.run', side_effect=AssertionError('unexpected helper rebuild')):
    texts, flags = a64emit.translate([(0, 0xd503201f)])
    assert len(texts) == len(flags) == 1
    assert zbic.lib().zbic_content_size(b'invalid', 7) == -1
"""
        env = os.environ.copy()
        env["PATH"] = str(Path(env["SYSTEMROOT"]) / "System32")
        result = subprocess.run([str(PYTHON), "-I", "-B", "-c", script, str(TOOLS)],
                                cwd=TOOLS, env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_game_free_uwp_build_plan(self):
        with tempfile.TemporaryDirectory(prefix="longshot-xbox-package-") as work:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(("RX_", "OOT_", "A64_", "LONGSHOT_"))}
            env.update(LONGSHOT_XBOX="1", LONGSHOT_MINGW=str(TOOLS / "llvm-mingw"), OOT_WORK=work)
            result = subprocess.run([str(PYTHON), "-I", "-B", str(TOOLS / "nx2" / "build.py"),
                                     "--standalone", "--dry-run"], cwd=TOOLS, env=env,
                                    capture_output=True, text=True, timeout=180)
            output = result.stdout + result.stderr
            self.assertEqual(result.returncode, 0, output)
            self.assertIn("0 missing", output)


if __name__ == "__main__":
    unittest.main()
