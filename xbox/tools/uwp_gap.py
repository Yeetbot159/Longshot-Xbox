"""Which of the Windows functions a built Longshot imports (xbox/probe/imports.inc) a UWP app may link: the names in the
Windows SDK's WindowsApp.lib, the import library of the app container. What is missing is what the UWP host layer
(host/uwp) must replace or look up at run time; the probe's imports section says which of those the console has anyway.

    python -I xbox/tools/uwp_gap.py [--lib <WindowsApp.lib>] [--markdown] [--pe <some.dll> ...]

--pe: the imports of those files instead (a DLL meant for the app package: Mesa's vulkan_dzn.dll, say), read from
their import tables.
"""
import argparse
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
IMPORTS = os.path.join(HERE, "..", "probe", "imports.inc")


def find_lib():
    roots = [os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "Lib")]
    hits = []
    for r in roots:
        hits += glob.glob(os.path.join(r, "*", "um", "x64", "WindowsApp.lib"))
    return sorted(hits)[-1] if hits else None


def lib_names(path):
    """The function names of an import library's short import members (an ar archive; each member's data starts with
    Sig1 0, Sig2 0xFFFF, then the name and the DLL as C strings)."""
    data = open(path, "rb").read()
    if not data.startswith(b"!<arch>\n"):
        raise SystemExit(f"{path}: not an archive")
    names = set()
    at = 8
    while at + 60 <= len(data):
        size = int(data[at + 48:at + 58].decode().strip() or 0)
        body = data[at + 60:at + 60 + size]
        if len(body) >= 20 and struct.unpack_from("<HH", body, 0) == (0, 0xFFFF):
            name = body[20:].split(b"\0", 1)[0].decode("ascii", "replace")
            names.add(name)
        at += 60 + size + (size & 1)
    return names


def pe_imports(path):
    """(dll, function) for each by-name import of a 64-bit PE file, from its import directory"""
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: not a PE file")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise SystemExit(f"{path}: not 64-bit")
    secs = [struct.unpack_from("<IIII", data, opt + struct.unpack_from("<H", data, pe + 20)[0] + 40 * i + 8)
            for i in range(nsec)]   # (virtual size, address, raw size, raw offset)

    def off(rva):
        for vsize, va, rsize, raw in secs:
            if va <= rva < va + max(vsize, rsize):
                return raw + rva - va
        raise ValueError(rva)

    def cstr(rva):
        o = off(rva)
        return data[o:data.index(b"\0", o)].decode("ascii", "replace")

    imp_rva = struct.unpack_from("<I", data, opt + 112 + 8)[0]   # (data directory 1: imports)
    out = []
    at = off(imp_rva) if imp_rva else None
    while at is not None:
        ilt, _, _, name, iat = struct.unpack_from("<IIIII", data, at)
        if not name:
            break
        dll = cstr(name).lower()
        t = off(ilt or iat)
        while True:
            entry = struct.unpack_from("<Q", data, t)[0]
            if not entry:
                break
            if not entry >> 63:   # (by name: a hint, then the name)
                out.append((dll, cstr((entry & 0x7FFFFFFF) + 2)))
            t += 8
        at += 20
    return out


def imports():
    out = []
    for line in open(IMPORTS, encoding="utf-8"):
        m = re.match(r'\s*\{"([^"]+)",\s*"([^"]+)"\}', line)
        if m:
            out.append((m.group(1).lower(), m.group(2)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--lib", help="WindowsApp.lib (default: the newest Windows SDK's)")
    ap.add_argument("--markdown", action="store_true", help="the missing functions as a Markdown table, by DLL")
    ap.add_argument("--pe", nargs="+", help="check these files' imports instead of imports.inc")
    a = ap.parse_args()
    lib = a.lib or find_lib()
    if not lib or not os.path.exists(lib):
        print("WindowsApp.lib not found: pass --lib", file=sys.stderr)
        return 2
    have = lib_names(lib)
    imp = sorted({i for p in a.pe for i in pe_imports(p)}) if a.pe else imports()
    # (a DLL beside it in the package is the app's own: its functions are not the system's to allow)
    own = {os.path.basename(p).lower() for p in a.pe or ()}
    missing = [(dll, fn) for dll, fn in imp if fn not in have and dll not in own]
    by_dll = {}
    for dll, fn in missing:
        by_dll.setdefault(dll, []).append(fn)
    if a.markdown:
        print("| DLL | Functions a UWP app may not link |")
        print("| --- | --- |")
        for dll in sorted(by_dll):
            print(f"| {dll} | {', '.join(sorted(by_dll[dll]))} |")
    else:
        for dll in sorted(by_dll):
            print(f"{dll} ({len(by_dll[dll])}): {' '.join(sorted(by_dll[dll]))}")
    print(f"{len(imp) - len(missing)} of {len(imp)} imports are in {os.path.basename(lib)}; {len(missing)} are not", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
