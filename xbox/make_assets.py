"""Write the Xbox hookshot-branded tile pictures a UWP package needs (Assets/*.png) into a folder.

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


def load_rgba(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")

    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    at = 8
    while at < len(data):
        size = struct.unpack_from(">I", data, at)[0]
        kind = data[at + 4:at + 8]
        chunk = data[at + 8:at + 8 + size]
        at += size + 12
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            compressed.extend(chunk)
        elif kind == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6) or interlace:
        raise ValueError("the Xbox hookshot logo must be a non-interlaced 8-bit RGB or RGBA PNG")

    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(compressed)
    stride = width * channels
    pixels = bytearray(width * height * 4)
    previous = bytearray(stride)
    source_at = 0
    for y in range(height):
        filter_type = raw[source_at]
        source_at += 1
        row = bytearray(raw[source_at:source_at + stride])
        source_at += stride
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            upper_left = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                row[i] = (row[i] + left) & 255
            elif filter_type == 2:
                row[i] = (row[i] + up) & 255
            elif filter_type == 3:
                row[i] = (row[i] + ((left + up) >> 1)) & 255
            elif filter_type == 4:
                p = left + up - upper_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upper_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else upper_left
                row[i] = (row[i] + predictor) & 255
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter {filter_type}")
        for x in range(width):
            src = x * channels
            dst = (y * width + x) * 4
            pixels[dst:dst + 3] = row[src:src + 3]
            pixels[dst + 3] = row[src + 3] if channels == 4 else 255
        previous = row
    return width, height, pixels


def resize_rgba(source, width, height):
    src_width, src_height, pixels = source
    output = bytearray(width * height * 4)
    for y in range(height):
        sy = max(0.0, min(src_height - 1.0, (y + 0.5) * src_height / height - 0.5))
        y0, y1 = int(sy), min(int(sy) + 1, src_height - 1)
        fy = sy - y0
        for x in range(width):
            sx = max(0.0, min(src_width - 1.0, (x + 0.5) * src_width / width - 0.5))
            x0, x1 = int(sx), min(int(sx) + 1, src_width - 1)
            fx = sx - x0
            dst = (y * width + x) * 4
            for c in range(4):
                a = pixels[(y0 * src_width + x0) * 4 + c]
                b = pixels[(y0 * src_width + x1) * 4 + c]
                d = pixels[(y1 * src_width + x0) * 4 + c]
                e = pixels[(y1 * src_width + x1) * 4 + c]
                top = a + (b - a) * fx
                bottom = d + (e - d) * fx
                output[dst + c] = int(top + (bottom - top) * fy + 0.5)
    return output


def branded_pixels(width, height, logo):
    background = (15, 20, 18)
    out = bytearray(width * height * 4)
    for i in range(0, len(out), 4):
        out[i:i + 4] = bytes((*background, 255))
    size = min(240, height, width)
    logo = resize_rgba(logo, size, size)
    logo_x, logo_y = (width - size) // 2, (height - size) // 2
    for y in range(size):
        for x in range(size):
            src = (y * size + x) * 4
            dst = ((logo_y + y) * width + logo_x + x) * 4
            alpha = logo[src + 3] / 255.0
            for c in range(3):
                out[dst + c] = int(logo[src + c] * alpha + background[c] * (1.0 - alpha) + 0.5)
    return out


def png(width, height, logo):
    pixels = branded_pixels(width, height, logo)
    rows = [b"\0" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height)]

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    head = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = os.path.join(sys.argv[1], "Assets")
    os.makedirs(out, exist_ok=True)
    logo = load_rgba(os.path.join(os.path.dirname(__file__), "..", "assets", "branding", "xbox-hookshot.png"))
    for name, (w, h) in SIZES.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(png(w, h, logo))
    return 0


if __name__ == "__main__":
    sys.exit(main())
