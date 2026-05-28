#!/usr/bin/env python3
# Placeholder icon generator. Stdlib only (no Pillow/ImageMagick required).
# Re-run any time you want fresh placeholders; replace with real art whenever.
#   python3 wallpaper/resources/_gen_icons.py

import os
import struct
import zlib


def png(width, height, pixels):
    """Build a minimal PNG. pixels = raw RGBA bytes, top-to-bottom rows."""
    row_bytes = width * 4
    # PNG scanline filter byte (0 = None) per row.
    raw = b"".join(
        b"\x00" + bytes(pixels[i * row_bytes:(i + 1) * row_bytes])
        for i in range(height)
    )
    compressed = zlib.compress(raw, 9)

    def chunk(tag, data):
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", compressed) + chunk(b"IEND", b"")


def ico(images):
    """images: [(size, png_bytes), ...]. Returns a Vista+ PNG-embedded ICO."""
    n = len(images)
    header = struct.pack("<HHH", 0, 1, n)  # reserved, type=1 (icon), count
    entries = bytearray()
    payload = bytearray()
    offset = 6 + 16 * n
    for size, data in images:
        # 0 in the width/height byte means 256 (the byte can only hold 0..255).
        w = 0 if size >= 256 else size
        h = 0 if size >= 256 else size
        entries += struct.pack(
            "<BBBBHHII",
            w, h,            # width, height
            0,               # color palette count (0 = no palette)
            0,               # reserved
            1,               # color planes
            32,              # bits per pixel
            len(data),       # image size
            offset,          # image data offset
        )
        payload += data
        offset += len(data)
    return header + bytes(entries) + bytes(payload)


def draw_droplet(size):
    """Teal-cyan disc with an upper-left highlight (suggests a fluid droplet)."""
    px = bytearray(size * size * 4)
    cx = cy = size / 2.0
    r = size * 0.46

    # Specular highlight in the upper-left quadrant.
    hx, hy = size * 0.37, size * 0.37
    hr = size * 0.14

    for y in range(size):
        for x in range(size):
            dx = x + 0.5 - cx
            dy = y + 0.5 - cy
            d = (dx * dx + dy * dy) ** 0.5
            i = (y * size + x) * 4

            if d > r + 1:
                continue  # already zero (transparent)

            # Inside the disc: lerp from cyan center to deep teal edge.
            t = max(0.0, min(1.0, d / r))
            cr = int(40 + (10 - 40) * t)
            cg = int(190 + (60 - 190) * t)
            cb = int(220 + (140 - 220) * t)

            # Mix in highlight (additive-ish toward white).
            hdx, hdy = x + 0.5 - hx, y + 0.5 - hy
            hd = (hdx * hdx + hdy * hdy) ** 0.5
            if hd < hr:
                blend = (1.0 - hd / hr) * 0.75
                cr = int(cr + (255 - cr) * blend)
                cg = int(cg + (255 - cg) * blend)
                cb = int(cb + (255 - cb) * blend)

            # 1-px edge anti-alias.
            if d < r - 1.0:
                alpha = 255
            elif d < r:
                alpha = int(255 * (r - d))
            else:
                alpha = int(255 * max(0.0, r + 1.0 - d))

            px[i + 0] = max(0, min(255, cr))
            px[i + 1] = max(0, min(255, cg))
            px[i + 2] = max(0, min(255, cb))
            px[i + 3] = max(0, min(255, alpha))
    return px


def main():
    sizes = [16, 32, 48, 256]
    images = [(s, png(s, s, draw_droplet(s))) for s in sizes]
    blob = ico(images)

    out_dir = os.path.dirname(os.path.abspath(__file__))
    # Same art for both ICOs in this placeholder. Swap in real assets later;
    # tray.ico should ideally be optimized for 16px display.
    for name in ("app.ico", "tray.ico"):
        path = os.path.join(out_dir, name)
        with open(path, "wb") as f:
            f.write(blob)
        print(f"Wrote {path} ({len(blob)} bytes)")


if __name__ == "__main__":
    main()
