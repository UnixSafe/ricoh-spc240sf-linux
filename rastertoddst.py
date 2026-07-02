#!/usr/bin/env python3
"""
rastertoddst — CUPS filter for the Ricoh Aficio SP C240SF (DDST/GDI).

Reads ``application/vnd.cups-raster`` from stdin, halftones each page to
1-bit per channel, compresses each plane with JBIG (via the bundled
``pbmtojbg`` from jbigkit), and writes the proprietary DDST stream to
stdout.

CUPS invokes filters as::

    rastertoddst job-id user title copies options [filename]

This filter only consumes argv for the GJET header user/title fields; the
actual raster comes from stdin.

The DDST format is documented in ``DDST_FORMAT.md`` next to this file,
based on reverse engineering of the macOS RicohAficioSPC240SFFilter and
its libDJZModule.dylib.
"""

from __future__ import annotations

import os
import socket
import struct
import subprocess
import sys
import tempfile
from typing import BinaryIO

# Sentinel magic numbers — stored as 4 ASCII bytes in stream order.
MAGIC_GJET = b"GJET"
MAGIC_GDIJ = b"GDIJ"
MAGIC_GDIP = b"GDIP"
MAGIC_GDIB = b"GDIB"
MAGIC_END = b"JIDG"

GJET_LEN = 0xA8  # 168
GDIJ_LEN = 0x78  # 120
GDIP_LEN = 0x40  # 64
GDIB_LEN = 0x20  # 32

# Default band height in raster lines. The Mac filter uses 256.
BAND_HEIGHT = 256

# Media size code table (PageSize string → DDST byte at GDIP+0x08).
MEDIA_CODES = {
    "Letter": 0x01,
    "Legal": 0x05,
    "Executive": 0x07,
    "A3": 0x08,
    "A4": 0x09,
    "A5": 0x0B,
    "A6": 0x46,
    "B4": 0x0C,
    "B5": 0x0D, # JIS B5
    "JISB5": 0x0D,
    "B6": 0x58, # JIS B6
    "FS": 0x10,
    "Folio": 0x0F,
    "Foolscap": 0x0E,
    "Env10": 0x14,
    "Monarch": 0x25,
    "EnvMonarch": 0x25,
    "DL": 0x1B, # EnvDL
    "EnvDL": 0x1B,
    "C5": 0x1C, # EnvC5
    "EnvC5": 0x1C,
    "C6": 0x1F, # EnvC6
    "EnvC6": 0x1F,
    "Ledger": 0x11,
    "HalfLetter": 0x06,
    "Kai8": 0x5C,
    "Kai16": 0x5D,
    "Postcard": 0x2B,
    "ReplyPaid": 0x45,
    "Custom": 0xFF,
}


def tray_code_for(name: str) -> int:
    name = name.lower()
    if name in ("automatic", "auto"):
        return 0x00
    if name in ("manual", "bypass"):
        return 0x01
    if name in ("tray1", "tray 1", "1"):
        return 0x02
    if name in ("tray2", "tray 2", "2"):
        return 0x03
    if name in ("tray3", "tray 3", "3"):
        return 0x04
    if name in ("tray4", "tray 4", "4"):
        return 0x05
    return 0x00


def paper_type_code_for(name: str) -> int:
    name = name.lower()
    if name == "plainrecycled":
        return 0x00
    if name in ("plain", "plain paper", "normal"):
        return 0x01
    if name in ("recycled", "recycled paper"):
        return 0x02
    if name in ("color", "colour"):
        return 0x03
    if name == "letterhead":
        return 0x04
    if name == "preprinted":
        return 0x05
    if name == "prepunched":
        return 0x06
    if name in ("labels", "label"):
        return 0x07
    if name == "bond":
        return 0x08
    if name in ("cardstock", "card"):
        return 0x09
    if name in ("thick", "thick paper"):
        return 0x0C
    if name == "thick160":
        return 0x0D
    if name == "envelope":
        return 0x0E
    if name in ("thin", "thin paper"):
        return 0x0F
    if name == "plain90":
        return 0x10
    if name == "thinner":
        return 0x12
    return 0x01


# Canonical point dimensions → media size code, used to pick the right code
# for each page's actual geometry (mixed-size jobs). Matches either
# orientation within a small tolerance.
_MEDIA_POINTS = [
    (595, 842, 0x09),   # A4
    (612, 792, 0x01),   # Letter
    (612, 1008, 0x05),  # Legal
    (420, 595, 0x0B),   # A5
    (516, 729, 0x0D),   # B5 (JIS)
    (363, 516, 0x58),   # B6 (JIS)
    (298, 420, 0x46),   # A6
    (522, 756, 0x07),   # Executive
    (297, 684, 0x14),   # Env #10
    (279, 540, 0x25),   # Env Monarch
    (312, 624, 0x1B),   # Env DL
    (459, 649, 0x1C),   # Env C5
    (323, 459, 0x1F),   # Env C6
    (284, 419, 0x2B),   # Postcard
]


def media_code_from_points(w: int, h: int, tol: int = 3) -> int:
    """Return the media code matching (w, h) in points, or 0 if unknown."""
    for sw, sh, code in _MEDIA_POINTS:
        portrait = abs(w - sw) <= tol and abs(h - sh) <= tol
        landscape = abs(w - sh) <= tol and abs(h - sw) <= tol
        if portrait or landscape:
            return code
    return 0


def cups_media_position_to_tray(cupsMediaPosition: int) -> int:
    if cupsMediaPosition == 1:
        return 0x02  # Tray 1
    if cupsMediaPosition == 2:
        return 0x03  # Tray 2
    if cupsMediaPosition == 3:
        return 0x04  # Tray 3
    if cupsMediaPosition == 4:
        return 0x01  # Bypass / Manual
    if cupsMediaPosition == 5:
        return 0x05  # Tray 4 / Fallback
    return 0x00  # Auto


def cups_media_type_to_paper_type(cupsMediaType: int) -> int:
    if cupsMediaType == 0:
        return 0x01  # Plain
    if cupsMediaType == 1:
        return 0x0C  # Thick
    if cupsMediaType == 2:
        return 0x0F  # Thin
    if cupsMediaType == 3:
        return 0x02  # Recycled
    if cupsMediaType == 4:
        return 0x0E  # Envelope
    if cupsMediaType == 5:
        return 0x07  # Labels
    return 0x01  # Plain


# --- CUPS raster parser -----------------------------------------------------

CUPS_RASTER_MAGIC_V2_BE = b"RaS2"
CUPS_RASTER_MAGIC_V2_LE = b"2SaR"
CUPS_RASTER_MAGIC_V3_BE = b"RaS3"
CUPS_RASTER_MAGIC_V3_LE = b"3SaR"

# CUPS raster v2/v3 page header is 1796 bytes laid out as documented in
# cups-raster-header(5). We only need a handful of fields.
CUPS_PAGE_HEADER_V2_LEN = 1796


def _read_exact(stream: BinaryIO, n: int) -> bytes:
    """Read exactly *n* bytes from *stream* or raise EOFError."""
    out = b""
    while len(out) < n:
        chunk = stream.read(n - len(out))
        if not chunk:
            raise EOFError(f"expected {n} bytes, got {len(out)}")
        out += chunk
    return out


class CupsRasterReader:
    """Iterates pages from a CUPS raster v2/v3 stream."""

    def __init__(self, stream: BinaryIO):
        self.stream = stream
        magic = _read_exact(stream, 4)
        if magic in (CUPS_RASTER_MAGIC_V2_LE, CUPS_RASTER_MAGIC_V3_LE):
            self.endian = "<"
        elif magic in (CUPS_RASTER_MAGIC_V2_BE, CUPS_RASTER_MAGIC_V3_BE):
            self.endian = ">"
        else:
            raise ValueError(f"not a CUPS raster stream: magic={magic!r}")
        self.is_v3 = magic in (CUPS_RASTER_MAGIC_V3_BE, CUPS_RASTER_MAGIC_V3_LE)

    def pages(self):
        while True:
            try:
                hdr = _read_exact(self.stream, CUPS_PAGE_HEADER_V2_LEN)
            except EOFError:
                return
            page = self._parse_header(hdr)
            # Raster data follows. cupsBytesPerLine * cupsHeight bytes.
            raster = _read_exact(self.stream, page["cupsBytesPerLine"] * page["cupsHeight"])
            page["raster"] = raster
            yield page

    def _parse_header(self, buf: bytes) -> dict:
        e = self.endian
        # The v2 header begins with a 64-byte MediaClass string and then a
        # series of fixed offsets. We read the ones we need by absolute
        # offset; see cups-raster-header(5).
        u32 = lambda off: struct.unpack(e + "I", buf[off : off + 4])[0]
        f32 = lambda off: struct.unpack(e + "f", buf[off : off + 4])[0]
        cstr = lambda off, n: buf[off : off + n].split(b"\0", 1)[0].decode(
            "latin-1", "replace"
        )
        return {
            "MediaClass": cstr(0, 64),
            "MediaColor": cstr(64, 64),
            "MediaType": cstr(128, 64),
            "OutputType": cstr(192, 64),
            "AdvanceDistance": u32(256),
            "AdvanceMedia": u32(260),
            "Collate": u32(264),
            "CutMedia": u32(268),
            "Duplex": u32(272),
            "HWResolution": (u32(276), u32(280)),
            "ImagingBBox": (u32(284), u32(288), u32(292), u32(296)),
            "InsertSheet": u32(300),
            "Jog": u32(304),
            "LeadingEdge": u32(308),
            "Margins": (u32(312), u32(316)),
            "ManualFeed": u32(320),
            "MediaPosition": u32(324),
            "MediaWeight": u32(328),
            "MirrorPrint": u32(332),
            "NegativePrint": u32(336),
            "NumCopies": u32(340),
            "Orientation": u32(344),
            "OutputFaceUp": u32(348),
            "PageSize": (u32(352), u32(356)),
            "Separations": u32(360),
            "TraySwitch": u32(364),
            "Tumble": u32(368),
            "cupsWidth": u32(372),
            "cupsHeight": u32(376),
            "cupsMediaType": u32(380),
            "cupsBitsPerColor": u32(384),
            "cupsBitsPerPixel": u32(388),
            "cupsBytesPerLine": u32(392),
            "cupsColorOrder": u32(396),
            "cupsColorSpace": u32(400),
            "cupsCompression": u32(404),
            "cupsRowCount": u32(408),
            "cupsRowFeed": u32(412),
            "cupsRowStep": u32(416),
            "cupsNumColors": u32(420),
            "cupsBorderlessScalingFactor": f32(424),
            "cupsPageSize": (f32(428), f32(432)),
            "cupsImagingBBox": (f32(436), f32(440), f32(444), f32(448)),
        }


# --- Halftoning -------------------------------------------------------------

def _ordered_dither_1bit(row: bytes, width: int, y: int) -> bytes:
    """Bayer 8x8 ordered dither of an 8-bpp coverage row to packed 1-bit MSB.

    The resulting bytes have white = 0 bit, black = 1 bit (ink).
    """
    # Threshold matrix (Bayer 8x8 × 4 normalized to 0..255).
    bayer = (
        (0, 32, 8, 40, 2, 34, 10, 42),
        (48, 16, 56, 24, 50, 18, 58, 26),
        (12, 44, 4, 36, 14, 46, 6, 38),
        (60, 28, 52, 20, 62, 30, 54, 22),
        (3, 35, 11, 43, 1, 33, 9, 41),
        (51, 19, 59, 27, 49, 17, 57, 25),
        (15, 47, 7, 39, 13, 45, 5, 37),
        (63, 31, 55, 23, 61, 29, 53, 21),
    )
    threshold_row = bayer[y & 7]
    out = bytearray((width + 7) // 8)
    for x in range(width):
        t = threshold_row[x & 7] * 4 + 2
        # Treat > threshold as ink (black bit set)
        if row[x] > t:
            out[x >> 3] |= 0x80 >> (x & 7)
    return bytes(out)


def halftone_plane(plane: bytes, width: int, height: int) -> bytes:
    """Halftone a single 8-bpp coverage plane to packed 1-bit MSB-first.

    Returned data has rows aligned to whole bytes; ``len() == ceil(w/8) * h``.
    """
    row_bytes = (width + 7) // 8
    out = bytearray(row_bytes * height)
    for y in range(height):
        row = plane[y * width : (y + 1) * width]
        out[y * row_bytes : (y + 1) * row_bytes] = _ordered_dither_1bit(row, width, y)
    return bytes(out)


# --- JBIG via pbmtojbg ------------------------------------------------------

def jbig_encode(plane_bits: bytes, width: int, height: int) -> bytes:
    """Encode a packed 1-bit MSB-first plane as a JBIG payload.

    Uses ``pbmtojbg`` with the parameters the Mac filter's ``_jbigInitialize``
    selects: stripe L0=256, order 0x03, options 0x40.
    """
    if width <= 0 or height <= 0:
        return b""
    # Build a PBM (P4) — binary raw header is "P4\n<w> <h>\n" then packed bits
    # with MSB-first and row alignment to whole bytes.
    pbm_header = f"P4\n{width} {height}\n".encode("ascii")
    pbm = pbm_header + plane_bits

    # We use pbmtojbg in a subprocess. -s 256, -p 0x40 (DPLAST), -o 0x03
    # (SMID|ILEAVE), -q to disable any progressive/diff layers, -m 0 to match C filter.
    cmd = ["pbmtojbg", "-q", "-s", "256", "-p", "64", "-o", "3", "-m", "0"]
    import shutil
    if not shutil.which("pbmtojbg"):
        script_dir = os.path.dirname(os.path.abspath(__file__))
        local_pbm = os.path.join(script_dir, "jbigkit-2.1", "pbmtools", "pbmtojbg")
        if os.path.exists(local_pbm):
            cmd[0] = local_pbm

    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    out, err = proc.communicate(pbm)
    if proc.returncode != 0:
        sys.stderr.write(f"ERROR: pbmtojbg failed: {err.decode(errors='replace')}\n")
        raise RuntimeError("pbmtojbg failed")
    return out


# --- DDST writers -----------------------------------------------------------

def write_gjet(out: BinaryIO, user: str, title: str) -> None:
    buf = bytearray(GJET_LEN)
    buf[0:4] = MAGIC_GJET
    struct.pack_into(">I", buf, 4, GJET_LEN)
    host = socket.gethostname() or "localhost"
    buf[8:8 + 64] = host.encode("ascii", "replace")[:64].ljust(64, b"\0")
    buf[0x48:0x48 + 64] = title.encode("ascii", "replace")[:64].ljust(64, b"\0")
    # 0x88..0x97 reserved
    buf[0x98:0x98 + 16] = user.encode("ascii", "replace")[:16].ljust(16, b"\0")
    out.write(buf)


def write_gdij(out: BinaryIO, page: dict, ditherBitHeight: int, color: bool,
               ditherBPP: int, duplex: bool, collate: bool) -> None:
    buf = bytearray(GDIJ_LEN)
    buf[0:4] = MAGIC_GDIJ
    struct.pack_into(">I", buf, 4, GDIJ_LEN)
    buf[8] = 0x00
    buf[9] = 0x64  # format version
    struct.pack_into(">H", buf, 0x0A, ditherBitHeight & 0xFFFF)
    # 0x10 = duplex byte (0x00 simplex, 0x02 duplex);
    # 0x11 = collate byte (0x00 off, 0x08 on). Ground truth: macOS startPage
    # (disasm_trace.txt 425-446) — simplex stores 0x00 at 0x10 (the 0x08 in the
    # legacy "0x0800" is the collate bit, byte 0x11). Collate independent.
    buf[0x10] = 0x02 if duplex else 0x00
    buf[0x11] = 0x08 if collate else 0x00
    struct.pack_into(">H", buf, 0x12, 0x00A8)
    struct.pack_into(">I", buf, 0x14, os.getpid() & 0xFFFFFFFF)
    buf[0x18] = 0x01 if color else 0x00
    # 0x20/0x21: macOS sets AH=0x01, AL=ditherBPP-1, byte-swaps, stores LE
    # (disasm 478-484) -> file bytes [0x01, ditherBPP-1]. NOT a big-endian word.
    buf[0x20] = 0x01
    buf[0x21] = max(ditherBPP - 1, 0) & 0xFF
    # 0x22/0x23: constant 0x0200 stored little-endian (disasm 486 movw $0x200)
    # -> file bytes [0x00, 0x02].
    buf[0x22] = 0x00
    buf[0x23] = 0x02
    host = socket.gethostname() or "localhost"
    buf[0x38:0x38 + 64] = host.encode("ascii", "replace")[:64].ljust(64, b"\0")
    out.write(buf)


def write_gdip(out: BinaryIO, page: dict, page_width: int, page_height: int,
               color: bool, media_code: int, tray_code: int, paper_type: int,
               duplex: bool, page_index: int, band_count: int) -> None:
    buf = bytearray(GDIP_LEN)
    buf[0:4] = MAGIC_GDIP
    struct.pack_into(">I", buf, 4, GDIP_LEN)
    buf[0x08] = media_code & 0xFF
    buf[0x09] = tray_code & 0xFF
    buf[0x0A] = paper_type & 0xFF
    struct.pack_into(">H", buf, 0x0C, page_width & 0xFFFF)
    struct.pack_into(">H", buf, 0x0E, page_height & 0xFFFF)
    buf[0x20] = 0x04 if color else 0x01
    # Replicate the macOS filter exactly (startPage, disasm_trace.txt 656-688).
    # r15 is the 1-based CUPS page number; page_index is 0-based -> +1.
    #   simplex: face 0x01, side = cups_page.
    #   duplex : face = 0x05 (even) / 0x0D (odd); side pair-swapped, not monotonic.
    cups_page = page_index + 1
    if duplex:
        buf[0x21] = 0x05 if (cups_page % 2 == 0) else 0x0D
        side = (cups_page - 1) if (cups_page & 1) else (cups_page + 1)
        struct.pack_into(">H", buf, 0x22, side & 0xFFFF)
    else:
        buf[0x21] = 0x01
        struct.pack_into(">H", buf, 0x22, cups_page & 0xFFFF)
    struct.pack_into(">H", buf, 0x36, band_count & 0xFFFF)
    # 0x38: float-to-int of cupsPageSize[0] (printable area in points).
    pw, ph = page["cupsPageSize"]
    struct.pack_into(">I", buf, 0x38, int(pw) & 0xFFFFFFFF)
    struct.pack_into(">I", buf, 0x3C, int(ph) & 0xFFFFFFFF)
    out.write(buf)


def write_gdib(out: BinaryIO, planes: dict, band_width: int, band_height: int,
               first_band: bool, last_band: bool, last_of_page: bool) -> None:
    buf = bytearray(GDIB_LEN)
    buf[0:4] = MAGIC_GDIB
    flags = (1 if first_band else 0) | (2 if last_band else 0)
    struct.pack_into(">I", buf, 0x04, flags)
    struct.pack_into(">I", buf, 0x08, len(planes.get("K", b"")))
    struct.pack_into(">I", buf, 0x0C, len(planes.get("Y", b"")))
    struct.pack_into(">I", buf, 0x10, len(planes.get("M", b"")))
    struct.pack_into(">I", buf, 0x14, len(planes.get("C", b"")))
    struct.pack_into(">H", buf, 0x18, band_width & 0xFFFF)
    struct.pack_into(">H", buf, 0x1A, band_height & 0xFFFF)
    buf[0x1E] = 1 if last_of_page else 0
    out.write(buf)


def write_end(out: BinaryIO) -> None:
    out.write(MAGIC_END)


# --- Page splitting into planes --------------------------------------------

def split_planes(page: dict) -> dict[str, bytes]:
    """Return ``{plane: 8bpp_coverage_bytes}`` for K-only or CMYK pages.

    We accept the common CUPS color spaces that the PPD can request:
    ``W`` (1ch luminance), ``K`` (1ch ink), ``RGB`` (3ch) and
    ``CMYK`` (4ch chunky). RGB is converted to CMYK with a naive
    under-color-removal.
    """
    cs = page["cupsColorSpace"]
    w, h = page["cupsWidth"], page["cupsHeight"]
    raster = page["raster"]
    bpl = page["cupsBytesPerLine"]
    bpp = page["cupsBitsPerPixel"]
    # CUPS color space numeric values (cups/raster.h):
    # 0 W, 1 RGB, 2 RGBA, 3 K, 6 CMYK, 18 sGray (SW), 19 sRGB ...
    if cs == 3:  # K
        if bpp == 8:
            plane = _extract_chunky(raster, bpl, w, h, 1, 0)
        else:
            raise ValueError(f"unsupported K bpp: {bpp}")
        return {"K": plane}
    if cs == 0:  # W = white (lighter = higher value, like grayscale)
        if bpp == 8:
            white = _extract_chunky(raster, bpl, w, h, 1, 0)
            # Convert W to K coverage by inversion.
            plane = bytes((255 - v) for v in white)
        else:
            raise ValueError(f"unsupported W bpp: {bpp}")
        return {"K": plane}
    if cs == 6:  # CMYK chunky
        if bpp == 32:
            c = _extract_chunky(raster, bpl, w, h, 4, 0)
            m = _extract_chunky(raster, bpl, w, h, 4, 1)
            y = _extract_chunky(raster, bpl, w, h, 4, 2)
            k = _extract_chunky(raster, bpl, w, h, 4, 3)
            return {"C": c, "M": m, "Y": y, "K": k}
        raise ValueError(f"unsupported CMYK bpp: {bpp}")
    if cs in (1, 19):  # RGB / sRGB (19 is CUPS_CSPACE_SRGB; 18 is sGray)
        if bpp == 24:
            return _rgb_to_cmyk(raster, bpl, w, h)
        raise ValueError(f"unsupported RGB bpp: {bpp}")
    if cs == 2:  # RGBA chunky — drop alpha, then UCR (parity with the C filter)
        if bpp == 32:
            rgb = bytearray(w * h * 3)
            for yy in range(h):
                row = raster[yy * bpl : yy * bpl + w * 4]
                dr = rgb[yy * w * 3 : (yy + 1) * w * 3]
                for xx in range(w):
                    dr[xx * 3 + 0] = row[xx * 4 + 0]
                    dr[xx * 3 + 1] = row[xx * 4 + 1]
                    dr[xx * 3 + 2] = row[xx * 4 + 2]
                rgb[yy * w * 3 : (yy + 1) * w * 3] = dr
            return _rgb_to_cmyk(bytes(rgb), w * 3, w, h)
        raise ValueError(f"unsupported RGBA bpp: {bpp}")
    raise ValueError(f"unsupported cupsColorSpace: {cs}")


def _extract_chunky(raster: bytes, bpl: int, w: int, h: int,
                    stride: int, offset: int) -> bytes:
    out = bytearray(w * h)
    for y in range(h):
        row = raster[y * bpl : y * bpl + w * stride]
        out[y * w : (y + 1) * w] = bytes(row[offset::stride])
    return bytes(out)


def _rgb_to_cmyk(raster: bytes, bpl: int, w: int, h: int) -> dict[str, bytes]:
    c = bytearray(w * h)
    m = bytearray(w * h)
    yp = bytearray(w * h)
    k = bytearray(w * h)
    for y in range(h):
        row = raster[y * bpl : y * bpl + w * 3]
        for x in range(w):
            r = row[x * 3]
            g = row[x * 3 + 1]
            b = row[x * 3 + 2]
            c_v = 255 - r
            m_v = 255 - g
            y_v = 255 - b
            k_v = min(c_v, m_v, y_v)
            idx = y * w + x
            c[idx] = c_v - k_v
            m[idx] = m_v - k_v
            yp[idx] = y_v - k_v
            k[idx] = k_v
    return {"C": bytes(c), "M": bytes(m), "Y": bytes(yp), "K": bytes(k)}


# --- Main pipeline ----------------------------------------------------------

def _option(options: str, key: str, default: str = "") -> str:
    for token in options.split():
        if token == key:
            return "true"
        if "=" in token:
            k, _, v = token.partition("=")
            if k == key:
                return v
    return default


def process(stream: BinaryIO, out: BinaryIO, job_id: str, user: str,
            title: str, copies: str, options: str) -> None:
    reader = CupsRasterReader(stream)

    sent_job_headers = False
    pages_written = 0
    media_name = _option(options, "PageSize", "")
    if not media_name:
        media_name = _option(options, "media", "A4")
    # Case-insensitive lookup to match the C filter's strcasecmp.
    _media_ci = {k.lower(): v for k, v in MEDIA_CODES.items()}
    media_opt = _media_ci.get(media_name.lower(), MEDIA_CODES.get("A4", 0x09))

    tray_name = _option(options, "InputSlot", "")
    if not tray_name:
        tray_name = _option(options, "InputTray", "")

    type_name = _option(options, "MediaType", "")
    if not type_name:
        type_name = _option(options, "PaperType", "")

    # Case-insensitive to match the C filter's strcasecmp (byte-parity).
    duplex_s = _option(options, "Duplex", "None")
    duplex = duplex_s.lower() not in ("none", "", "false")
    collate = _option(options, "Collate", "").lower() == "true"
    forced_mono = _option(options, "ColorModel", "").lower() in ("gray", "mono", "black")

    for page in reader.pages():
        cs = page["cupsColorSpace"]
        page_is_color = (cs in (1, 2, 6, 19)) and not forced_mono
        planes_data = split_planes(page)
        if forced_mono and "K" not in planes_data:
            # Convert to K by averaging CMY.
            c = planes_data.get("C", b"")
            m = planes_data.get("M", b"")
            yp = planes_data.get("Y", b"")
            k = planes_data.get("K", b"")
            n = len(k) or len(c) or len(m) or len(yp)
            combined = bytearray(n)
            for i in range(n):
                vals = [b[i] for b in (c, m, yp, k) if b]
                combined[i] = min(255, sum(vals))
            planes_data = {"K": bytes(combined)}

        width = page["cupsWidth"]
        height = page["cupsHeight"]

        # Pad each plane's width up to a multiple of 32 pixels (4 bytes) so
        # the JBIG stripes line up with what the firmware expects.
        padded_width = (width + 31) & ~31
        if padded_width != width:
            planes_data = {
                ch: _pad_plane(p, width, height, padded_width)
                for ch, p in planes_data.items()
            }

        if not sent_job_headers:
            write_gjet(out, user, title)
            write_gdij(
                out, page,
                ditherBitHeight=height,
                color=page_is_color,
                ditherBPP=1,
                duplex=duplex,
                collate=collate,
            )
            sent_job_headers = True

        band_count = (height + BAND_HEIGHT - 1) // BAND_HEIGHT
        # Prefer the media code matching this page's actual geometry (mixed-size
        # jobs); fall back to the job-level PageSize option when unrecognised.
        pw, ph = page["cupsPageSize"]
        media_pg = media_code_from_points(int(pw), int(ph))
        media_code = media_pg if media_pg else media_opt
        tray_code = tray_code_for(tray_name) if tray_name else cups_media_position_to_tray(page.get("MediaPosition", 0))
        paper_type = paper_type_code_for(type_name) if type_name else cups_media_type_to_paper_type(page.get("cupsMediaType", 0))

        write_gdip(
            out, page,
            page_width=padded_width,
            page_height=height,
            color=page_is_color,
            media_code=media_code,
            tray_code=tray_code,
            paper_type=paper_type,
            duplex=duplex,
            page_index=pages_written,
            band_count=band_count,
        )

        for band_idx in range(band_count):
            y0 = band_idx * BAND_HEIGHT
            y1 = min(y0 + BAND_HEIGHT, height)
            bh = y1 - y0
            jbig_planes = {}
            for ch, plane in planes_data.items():
                row_bytes = padded_width  # 1 byte per pixel still
                band_8bpp = plane[y0 * row_bytes : y1 * row_bytes]
                halftoned = halftone_plane(band_8bpp, padded_width, bh)
                jbig_planes[ch] = jbig_encode(halftoned, padded_width, bh)

            first = band_idx == 0
            last = band_idx == band_count - 1
            write_gdib(
                out, jbig_planes, padded_width, bh,
                first_band=first, last_band=last,
                last_of_page=last,
            )
            # Order in stream: K, Y, M, C (color); just K for mono.
            for ch in ("K", "Y", "M", "C"):
                payload = jbig_planes.get(ch)
                if payload:
                    out.write(payload)

        pages_written += 1

    if sent_job_headers:
        write_end(out)


def _pad_plane(plane: bytes, w: int, h: int, padded_w: int) -> bytes:
    if padded_w == w:
        return plane
    out = bytearray(padded_w * h)
    for y in range(h):
        out[y * padded_w : y * padded_w + w] = plane[y * w : (y + 1) * w]
    return bytes(out)


def main(argv: list[str]) -> int:
    if len(argv) < 6:
        sys.stderr.write(
            "Usage: rastertoddst job-id user title copies options [filename]\n"
        )
        return 1
    job_id, user, title, copies, options = argv[1:6]
    if len(argv) >= 7:
        in_stream = open(argv[6], "rb")
    else:
        in_stream = sys.stdin.buffer
    try:
        process(in_stream, sys.stdout.buffer, job_id, user, title, copies, options)
    finally:
        if in_stream is not sys.stdin.buffer:
            in_stream.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
