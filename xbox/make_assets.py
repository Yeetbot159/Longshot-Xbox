"""Write the plain tile pictures a UWP package needs (Assets/*.png) into a folder: a dark square with a green bar.

    python -I xbox/make_assets.py <package folder>
"""
import os
import struct
import sys
import zlib

SIZES = {
    "StoreLogo.png": (50, 50),
    "Square44x44Logo.png": (44, 44),
    "Square150x150Logo.png": (150, 150),
    "SplashScreen.png": (620, 300),
}


def png(width, height):
    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            bar = height * 2 // 5 <= y < height * 3 // 5 and width // 6 <= x < width * 5 // 6
            row += bytes((0x3c, 0xb3, 0x71) if bar else (0x14, 0x18, 0x1a))
        rows.append(bytes(row))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    head = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = os.path.join(sys.argv[1], "Assets")
    os.makedirs(out, exist_ok=True)
    for name, (w, h) in SIZES.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(png(w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
