"""The Xbox build of the game (LONGSHOT_XBOX=1, nx2/build.py): a module for the app (xbox/app) that names no desktop DLL.

Read from build.py's source, as the build needs the game: its link takes the app container's import library and no
desktop one; host/uwp stands in for host/display.c; the game's main is renamed for host/uwp/uwp_entry.c; the module's
compile flags emulate thread-locals; no renderer is built into it; and the Zen 2 default leaves RX_MARCH's own lines
alone (tests/test_cpu_target.py reads them)."""
import ast
import os
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return f.read()


BUILD = read("nx2", "build.py")
DESKTOP = ("user32", "winmm", "shell32", "gdi32", "dbghelp", "advapi32", "kernel32", "winhttp")


def assigned(name):
    """The literal value of a top-level `name = [...]` in build.py"""
    for node in ast.parse(BUILD).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return ast.literal_eval(node.value) if not isinstance(node.value, ast.BinOp) else \
                ast.literal_eval(node.value.left) + ast.literal_eval(node.value.right)
    raise AssertionError("%s is not assigned in build.py" % name)


class XboxBuild(unittest.TestCase):
    def test_probe_keeps_master_voice_until_source_voice_is_destroyed(self):
        source = read("xbox", "probe", "probe.cpp")
        audio = source[source.index("void probe_audio_input()"):source.index("// ---- 5. threads")]
        self.assertLess(audio.index("CreateMasteringVoice"), audio.index("CreateSourceVoice"))
        self.assertLess(audio.index("voice->DestroyVoice()"), audio.index("mv->DestroyVoice()"))

    def test_xbox_packages_do_not_request_unsupported_broad_filesystem_access(self):
        for manifest in ("xbox", "app", "AppxManifest.xml"), ("xbox", "probe", "AppxManifest.xml"):
            text = read(*manifest)
            self.assertNotIn("broadFileSystemAccess", text)
            self.assertIn('Name="removableStorage"', text)

    def test_game_package_requests_utf8_for_game_and_renderer_paths(self):
        manifest = ET.fromstring(read("xbox", "app", "AppxManifest.xml"))
        ns = {"f": "http://schemas.microsoft.com/appx/manifest/foundation/windows10",
              "uap7": "http://schemas.microsoft.com/appx/manifest/uap/windows10/7",
              "uap8": "http://schemas.microsoft.com/appx/manifest/uap/windows10/8"}
        app = manifest.find("f:Applications/f:Application", ns)
        self.assertIsNotNone(app)
        self.assertEqual(app.findtext("uap7:Properties/uap8:ActiveCodePage", namespaces=ns), "UTF-8")
        self.assertTrue({"uap7", "uap8"} <= set(manifest.get("IgnorableNamespaces", "").split()))

    def test_link_names_no_desktop_library(self):
        libs = assigned("UWP_LIBS")
        self.assertIn("-lwindowsapp", libs)
        self.assertIn("-nodefaultlibs", libs)   # (else the driver adds user32, shell32 and the rest by itself)
        for lib in DESKTOP:
            self.assertNotIn("-l" + lib, libs)
        self.assertNotIn("-static", " ".join(libs))   # (the C++ runtime's DLLs are the app's: libc++abi has native TLS)

    def test_host_uwp_replaces_the_desktop_window(self):
        self.assertIn('and not (UWP and f == "display.c")', BUILD)
        for f in ("display_uwp.c", "win32_uwp.c", "uwp_entry.c"):
            self.assertIn('"%s"' % f, BUILD)
            self.assertTrue(os.path.isfile(os.path.join(ROOT, "host", "uwp", f)), f)

    def test_host_asks_for_no_desktop_library(self):
        """A `#pragma comment(lib, ...)` in a host file puts a desktop import library in the module's link, beside host/uwp's
        own definitions of its functions (duplicate symbols): each sits under #ifndef LONGSHOT_UWP"""
        host = os.path.join(ROOT, "host")
        for f in sorted(os.listdir(host)):
            if not f.endswith((".c", ".cpp", ".h")):
                continue
            depth = []
            for line in read("host", f).splitlines():
                s = line.strip()
                if s.startswith("#if"):
                    depth.append(s.startswith("#ifndef LONGSHOT_UWP") or s.startswith("#if !defined(LONGSHOT_UWP)"))
                elif s.startswith(("#else", "#elif")) and depth:
                    depth[-1] = False
                elif s.startswith("#endif") and depth:
                    depth.pop()
                elif re.match(r"#\s*pragma\s+comment\s*\(\s*lib", s):
                    self.assertTrue(any(depth), "%s: %s outside #ifndef LONGSHOT_UWP" % (f, s))

    def test_every_replaced_function_is_found_by_name(self):
        """The app resolves the renderer's and Dozen's desktop imports through longshot_uwp_win32 (xbox/app/modules.cpp):
        a function win32_uwp.c defines and its table leaves out would be found in the game module and nowhere else"""
        src = read("host", "uwp", "win32_uwp.c")
        defined = set(re.findall(r"^\w[\w ]*\bWINAPI (\w+)\(", src, re.M))
        table = src[src.index("#define UWP_WIN32(X)"):src.index("static const struct")]
        listed = set(re.findall(r"X\((\w+)\)", table))
        self.assertGreater(len(defined), 80)
        self.assertEqual(defined - listed, set())
        self.assertEqual(listed - defined, set())

    def test_main_is_renamed_for_the_app(self):
        self.assertIn('"-Dmain=hos_uwp_game_main"', BUILD)
        entry = read("host", "uwp", "uwp_entry.c")
        self.assertIn("int hos_uwp_game_main(int argc, char **argv);", entry)

    def test_compile_flags(self):
        start = BUILD.index("def target():")
        body = BUILD[start:BUILD.index("\ndef ", start + 1)]
        self.assertRegex(body, r'\["-femulated-tls", "-DLONGSHOT_UWP"\] if UWP')

    def test_no_renderer_in_the_module(self):
        self.assertRegex(BUILD, r"if UWP\b[^\n]*:\s*(?:#[^\n]*)?\n\s+build_vk = False")
        self.assertIn("not UWP and os.path.isfile(os.path.join(WGPU", BUILD)

    def test_zen2_through_rx_march(self):
        self.assertIn('os.environ.setdefault("RX_MARCH", "znver2")', BUILD)
        self.assertIn('MARCH = os.environ.get("RX_MARCH") or host_march()', BUILD)
        self.assertLess(BUILD.index('setdefault("RX_MARCH"'), BUILD.index('MARCH = os.environ.get("RX_MARCH")'))

    def test_switch_follows_the_on_off_rule(self):
        line = next(l for l in BUILD.splitlines() if l.startswith("UWP = "))
        for off in ('""', '"0"', '"false"', '"off"', '"no"'):
            self.assertIn(off, line)
        self.assertTrue(re.search(r"\.strip\(\)\.lower\(\)", line))


if __name__ == "__main__":
    unittest.main()
