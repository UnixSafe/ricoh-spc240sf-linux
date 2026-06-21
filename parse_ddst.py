#!/usr/bin/env python3
"""Dump the structure of a DDST file produced by rastertoddst.

Verifies the magic numbers, header lengths and reports each band so we can
sanity-check the output before sending it to the printer.
"""

from __future__ import annotations

import struct
import sys


def be32(b: bytes, off: int) -> int:
    return struct.unpack(">I", b[off : off + 4])[0]


def be16(b: bytes, off: int) -> int:
    return struct.unpack(">H", b[off : off + 2])[0]


def dump(path: str) -> int:
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    n = len(data)
    page = 0
    band = 0
    while off < n:
        magic = data[off : off + 4]
        if magic == b"GJET":
            length = be32(data, off + 4)
            host = data[off + 8 : off + 0x48].rstrip(b"\0").decode("latin-1", "replace")
            title = data[off + 0x48 : off + 0x88].rstrip(b"\0").decode("latin-1", "replace")
            user = data[off + 0x98 : off + 0xA8].rstrip(b"\0").decode("latin-1", "replace")
            print(f"@{off:08x} GJET len={length} host={host!r} title={title!r} user={user!r}")
            off += length
        elif magic == b"GDIJ":
            length = be32(data, off + 4)
            version = data[off + 9]
            copies = be16(data, off + 0xA)
            duplex_b = data[off + 0x10]   # 0x00 simplex, 0x02 duplex
            collate_b = data[off + 0x11]  # 0x00 off, 0x08 on
            const = be16(data, off + 0x12)
            pid = be32(data, off + 0x14)
            color = data[off + 0x18]
            bpp_flag = be16(data, off + 0x20)
            host = data[off + 0x38 : off + 0x78].rstrip(b"\0").decode("latin-1", "replace")
            print(
                f"@{off:08x} GDIJ len={length} version=0x{version:02x} copies={copies} "
                f"duplex=0x{duplex_b:02x} collate=0x{collate_b:02x} const=0x{const:04x} "
                f"pid={pid} color={color} bpp_flag=0x{bpp_flag:04x} host={host!r}"
            )
            off += length
        elif magic == b"GDIP":
            length = be32(data, off + 4)
            media = data[off + 0x08]
            tray = data[off + 0x09]
            paper_type = data[off + 0x0A]
            w = be16(data, off + 0x0C)
            h = be16(data, off + 0x0E)
            channels = data[off + 0x20]
            duplex_status = data[off + 0x21]
            side = be16(data, off + 0x22)
            bands = be16(data, off + 0x36)
            pw = be32(data, off + 0x38)
            ph = be32(data, off + 0x3C)
            page += 1
            band = 0
            print(
                f"@{off:08x} GDIP page={page} len={length} {w}x{h} ch={channels} "
                f"media=0x{media:02x} tray=0x{tray:02x} type=0x{paper_type:02x} "
                f"duplex=0x{duplex_status:02x} side={side} bands={bands} pagesize={pw}x{ph}"
            )
            off += length
        elif magic == b"GDIB":
            band += 1
            flags = be32(data, off + 0x04)
            kS = be32(data, off + 0x08)
            yS = be32(data, off + 0x0C)
            mS = be32(data, off + 0x10)
            cS = be32(data, off + 0x14)
            w = be16(data, off + 0x18)
            h = be16(data, off + 0x1A)
            eop = data[off + 0x1E]
            print(
                f"@{off:08x} GDIB band={band} flags=0x{flags:08x} "
                f"K={kS} Y={yS} M={mS} C={cS} {w}x{h} eop={eop}"
            )
            off += 0x20 + kS + yS + mS + cS
        elif magic == b"JIDG":
            print(f"@{off:08x} JIDG end-of-job")
            off += 4
        else:
            print(f"@{off:08x} UNKNOWN magic={magic!r}; aborting")
            return 1
    print(f"-- end at {off:08x} (file size {n})")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: parse_ddst.py <file.ddst>", file=sys.stderr)
        sys.exit(2)
    sys.exit(dump(sys.argv[1]))
