#!/usr/bin/env python3
# tiramisu/tools/gen-assets.py — generate the 4 manifest logos (solid dark
# canvas + teal glyph) as real PNGs; no third-party binaries committed.
import struct, sys, zlib, os

def png(path, w, h, rgba):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b""
    row = b"\x00" + bytes(rgba) * w
    raw = row * h
    hdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def glyph_png(path, w, h):
    # dark canvas #0E1116 + teal "L" glyph #20808D, drawn pixelwise
    canvas = (0x0E, 0x11, 0x16, 0xFF)
    teal = (0x20, 0x80, 0x8D, 0xFF)
    px = bytearray(w * h * 4)
    u = max(w, h) / 100.0
    for y in range(h):
        for x in range(w):
            fx, fy = x / u, y / u
            col = teal if ((30 <= fx <= 44 and 22 <= fy <= 80) or
                           (30 <= fx <= 74 and 68 <= fy <= 80)) else canvas
            i = (y * w + x) * 4
            px[i:i+4] = bytes(col)
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    hdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) +
                chunk(b"IDAT", zlib.compress(bytes(px), 9)) + chunk(b"IEND", b""))

out = sys.argv[1] if len(sys.argv) > 1 else "out/layout/Assets"
os.makedirs(out, exist_ok=True)
glyph_png(os.path.join(out, "Square44x44Logo.png"), 44, 44)
glyph_png(os.path.join(out, "Square150x150Logo.png"), 150, 150)
glyph_png(os.path.join(out, "StoreLogo.png"), 50, 50)
glyph_png(os.path.join(out, "SplashScreen.png"), 1920, 1080)
print("assets written to", out)
