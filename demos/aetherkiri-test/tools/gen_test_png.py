#!/usr/bin/env python3
"""Generates test images for the AetherKiri render self-test (data/test/).

Writes RGBA8 PNGs with only stdlib (struct+zlib), no PIL required:
  gradient.png   256x256  horizontal RGB gradient + alpha sweep
  alpha_circle.png 256x256 transparent bg + translucent red circle
  checker.png    128x128  black/white checkerboard (scaling/aliasing)
  stripes.png    320x240  saturated vertical stripes (JPEG-ish content)
"""

import math
import os
import struct
import sys
import zlib


def chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path: str, w: int, h: int, pixels: bytearray) -> None:
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8-bit RGBA
    raw = b"".join(
        b"\x00" + bytes(pixels[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    print(f"generated {path}: {w}x{h}")


def gen_gradient(path: str) -> None:
    w = h = 256
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            px[i] = x * 255 // (w - 1)          # R ramp
            px[i + 1] = 128                     # fixed G
            px[i + 2] = 255 - x * 255 // (w - 1)  # B ramp
            px[i + 3] = 255                     # opaque
    write_png(path, w, h, px)


def gen_alpha_circle(path: str) -> None:
    w = h = 256
    cx = cy = 128
    r = 110
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            d = math.hypot(x - cx, y - cy)
            i = (y * w + x) * 4
            if d < r:
                # translucent red, alpha falls off toward the rim
                alpha = int(255 * (1.0 - d / r) * 0.9 + 26)
                px[i] = 255
                px[i + 1] = 0
                px[i + 2] = 0
                px[i + 3] = alpha
            # else fully transparent
    write_png(path, w, h, px)


def gen_checker(path: str) -> None:
    w = h = 128
    cell = 16
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            v = 255 if ((x // cell + y // cell) % 2) == 0 else 0
            px[i] = px[i + 1] = px[i + 2] = v
            px[i + 3] = 255
    write_png(path, w, h, px)


def gen_stripes(path: str) -> None:
    w, h = 320, 240
    colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0),
              (0, 255, 255), (255, 0, 255), (255, 255, 255)]
    band = 16
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            r, g, b = colors[(x // band) % len(colors)]
            i = (y * w + x) * 4
            px[i], px[i + 1], px[i + 2] = r, g, b
            px[i + 3] = 255
    write_png(path, w, h, px)


def main() -> int:
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "data/test"
    os.makedirs(out_dir, exist_ok=True)
    gen_gradient(os.path.join(out_dir, "gradient.png"))
    gen_alpha_circle(os.path.join(out_dir, "alpha_circle.png"))
    gen_checker(os.path.join(out_dir, "checker.png"))
    gen_stripes(os.path.join(out_dir, "stripes.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
