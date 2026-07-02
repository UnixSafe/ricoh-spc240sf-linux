# Ricoh DDST (Distributed Document Streaming Technology) Format
## Reverse-engineered from RicohAficioSPC240SFFilter (macOS) – 2026-05-20

This document describes the binary spool format consumed by the Ricoh
Aficio SP C240SF over USB/network. The Mac/Windows hosts both produce
this stream; on Linux we have to generate it ourselves.

All multi-byte fields in DDST headers are **big-endian** unless stated.
Magic numbers appear as 4 ASCII characters when read in stream order.

## Stream layout

```
+-------------------+
| GJET (168 bytes)  | job start sentinel (login/host info)
+-------------------+
| GDIJ (120 bytes)  | job header (resolution, color, bpp, pid, hostname)
+-------------------+
| GDIP (64 bytes)   | page 1 header
+-------------------+
| GDIB band 1.1     |
| K plane (JBIG)    |
| Y plane (JBIG)    | -- only if color (channelCount == 4)
| M plane (JBIG)    |
| C plane (JBIG)    |
+-------------------+
| GDIB band 1.N ... |
+-------------------+
| GDIP page 2 ...   |
+-------------------+
| JIDG (4 bytes)    | end of job marker
+-------------------+
```

## GJET — Job Start (168 bytes, written once)

| Offset | Size | Field          |
|--------|------|----------------|
| 0x00   | 4    | "GJET"         |
| 0x04   | 4    | BE32 = 0xA8 (length) |
| 0x08   | 64   | hostname (NUL padded; "localhost" if gethostname() fails) |
| 0x48   | 64   | print job name (from CUPS argv[3]) |
| 0x88   | 16   | reserved (zero) |
| 0x98   | 16   | user code / login name |

## GDIJ — Job Header (120 bytes, written once)

| Offset | Size | Field |
|--------|------|-------|
| 0x00   | 4    | "GDIJ" |
| 0x04   | 4    | BE32 = 0x78 (length=120) |
| 0x08   | 1    | reserved (0x00) |
| 0x09   | 1    | format version = 0x64 (100) |
| 0x0A   | 2    | BE16 copy count (`cups_page_header2_t.NumCopies`) |
| 0x0C   | 4    | reserved (zero) |
| 0x10   | 1    | duplex byte: 0x00 simplex, 0x02 duplex |
| 0x11   | 1    | collate byte: 0x00 off, 0x08 on (independent of duplex) |
| 0x12   | 2    | BE16 = 0x00A8 (constant) |
| 0x14   | 4    | BE32 process PID |
| 0x18   | 1    | byte: 1 if color (channelCount==4), 0 if mono |
| 0x19   | 1    | reserved |
| 0x1A   | 6    | reserved |
| 0x20   | 1    | = 0x01 (constant) |
| 0x21   | 1    | = ditherBPP-1 (0 for 1-bit halftone) |
| 0x22   | 2    | little-endian 0x0200 → bytes 0x00 0x02 (NOT big-endian) |
| 0x24   | 20   | reserved (zero) |
| 0x38   | 64   | hostname (gethostname, NUL padded) |
| 0x78   | end  | |

## GDIP — Page Header (64 bytes, before each page)

| Offset | Size | Field |
|--------|------|-------|
| 0x00   | 4    | "GDIP" |
| 0x04   | 4    | BE32 = 0x40 (length=64) |
| 0x08   | 1    | media size code (0x09=A4, 0x01=Letter, 0x05=Legal, 0x0B=A5, 0x0D=B5, 0x58=B6, 0x46=A6, 0x07=Exec, 0x14=Env10, 0x25=Monarch, 0x1B=DL, 0x1C=C5, 0x1F=C6, 0x2B=Postcard, 0xFF=Custom) |
| 0x09   | 1    | input tray: 0x00 auto, 0x01 bypass, 0x02 tray1, 0x03 tray2, 0x04 tray3, 0x05 tray4 |
| 0x0A   | 1    | paper type: 0x01 plain, 0x02 recycled, 0x0C thick, 0x0F thin, 0x0E envelope, 0x07 labels, … |
| 0x0B   | 1    | reserved (zero) |
| 0x0C   | 2    | BE16 page width in pixels (ditherBitWidth) |
| 0x0E   | 2    | BE16 page height in lines (cupsHeight) |
| 0x10   | 16   | reserved (zero) |
| 0x20   | 1    | channel count: 0x01 mono, 0x04 CMYK |
| 0x21   | 1    | face marker: 0x01 simplex; duplex 0x0D on odd CUPS page (1st,3rd…), 0x05 on even |
| 0x22   | 2    | BE16 side index: simplex = CUPS page (1-based); duplex = (odd page) page-1, (even) page+1 — pair-swapped |
| 0x24   | 16   | reserved (zero) |
| 0x34   | 2    | reserved (zero) |
| 0x36   | 2    | BE16 band count for this page (ditherBandCount) |
| 0x38   | 4    | BE32 (float→int) page printable area width |
| 0x3C   | 4    | BE32 (float→int) page printable area height |

## GDIB — Band Header (32 bytes, before each band's JBIG payload)

| Offset | Size | Field |
|--------|------|-------|
| 0x00   | 4    | "GDIB" |
| 0x04   | 4    | BE32 flags: bit 0 = first band of plane, bit 1 = last band of plane |
| 0x08   | 4    | BE32 K plane JBIG payload size |
| 0x0C   | 4    | BE32 Y plane JBIG payload size (0 if mono) |
| 0x10   | 4    | BE32 M plane JBIG payload size (0 if mono) |
| 0x14   | 4    | BE32 C plane JBIG payload size (0 if mono) |
| 0x18   | 2    | BE16 band width in pixels |
| 0x1A   | 2    | BE16 band height in lines |
| 0x1C   | 2    | reserved (zero) |
| 0x1E   | 1    | end-of-page marker (1 on last band of a finished page) |
| 0x1F   | 1    | reserved (zero) |

## JBIG payload encoding (per plane, per band)

Produced by jbigkit's encoder with these BIH parameters (`_jbigInitialize`):

* DL = 0, D = 0 (no differential layers)
* P = 1 (single plane)
* XD = band width in bits, YD = band height in lines
* L0 = 256 (stripe height)
* MX = 0, MY = 0
* Order = 0x03 (SMID + ILEAVE)
* Options = 0x40 (`JBG_LRLTWO`). The macOS `djz_Compress` path passes
  mode `1` to its JBIG encoder, selecting this value rather than 0x48.

This matches `pbmtojbg -s 256 -p 64 -o 3 -q -m 0` produced from a strict PBM
(P4) raster of the plane padded so width is a multiple of 32 pixels (per
`djz_Compress`: `width_bytes = (width_pixels + 7) / 8` then aligned to 4).

## Halftoning

Windows driver uses 1/2/4-bit dither matrices (gfegs{c,m,y,k}{1,2,4}.bin).
For our first Linux port we generate **1-bit** halftones using a simple
Floyd-Steinberg or ordered dither (see `rastertoddst.py`).
