#!/usr/bin/env python3
"""Synthesize a tiny CUPS raster v3 stream for filter smoke-testing.

Produces a 320×240 page in 8-bit CMYK chunky (32 bpp). The raster contains
a few horizontal stripes per channel so the dithered output is visually
distinct.
"""

from __future__ import annotations

import struct
import sys


def make_header(w: int, h: int, color: bool = True) -> bytes:
    hdr = bytearray(1796)
    # MediaClass / MediaColor / MediaType / OutputType (4 × 64 bytes) — leave blank.
    def u32(off: int, v: int) -> None:
        struct.pack_into("<I", hdr, off, v)

    def f32(off: int, v: float) -> None:
        struct.pack_into("<f", hdr, off, v)

    u32(276, 600)  # HWResolution[0]
    u32(280, 600)  # HWResolution[1]
    u32(340, 1)  # NumCopies
    u32(372, w)  # cupsWidth
    u32(376, h)  # cupsHeight
    u32(384, 8)  # cupsBitsPerColor
    u32(388, 32 if color else 8)  # cupsBitsPerPixel
    u32(392, w * (4 if color else 1))  # cupsBytesPerLine
    u32(396, 0)  # cupsColorOrder (chunky)
    u32(400, 6 if color else 3)  # cupsColorSpace (6=CMYK, 3=K)
    u32(420, 4 if color else 1)  # cupsNumColors
    f32(424, 1.0)  # cupsBorderlessScalingFactor
    f32(428, 595.0)  # cupsPageSize w (A4 in points)
    f32(432, 842.0)  # cupsPageSize h
    return bytes(hdr)


def make_raster(w: int, h: int, color: bool) -> bytes:
    if color:
        out = bytearray(w * h * 4)
        for y in range(h):
            band = y // (h // 4) if h >= 4 else 0
            for x in range(w):
                i = (y * w + x) * 4
                # C in top, M next, Y next, K bottom (heavy stripes)
                c = 200 if band == 0 else 0
                m = 200 if band == 1 else 0
                yp = 200 if band == 2 else 0
                k = 200 if band == 3 else 0
                # add a diagonal gradient for variety
                t = (x + y) & 0xFF
                if band == 0:
                    c = t
                out[i + 0] = c
                out[i + 1] = m
                out[i + 2] = yp
                out[i + 3] = k
        return bytes(out)
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            out[y * w + x] = (x + y) & 0xFF
    return bytes(out)


def main() -> int:
    color = "--mono" not in sys.argv
    w, h = 320, 240
    pages = 1
    if "--pages" in sys.argv:
        pages = int(sys.argv[sys.argv.index("--pages") + 1])

    sys.stdout.buffer.write(b"3SaR")  # CUPS raster v3, little-endian
    for _ in range(pages):
        sys.stdout.buffer.write(make_header(w, h, color))
        sys.stdout.buffer.write(make_raster(w, h, color))
    return 0


if __name__ == "__main__":
    sys.exit(main())
