# Comparison: our SP C240SF driver vs. existing open-source Ricoh drivers

## Reference drivers studied

| Repo | Printer | Language | Protocol |
|------|---------|----------|----------|
| zhangtemplar/ricoh-sp100 | SP 100 (mono) | Bash | PJL + JBIG |
| funinkina/Ricoh-SP200-Linux | SP 200 (mono) | C (libcups) | PJL + JBIG |
| droidzone/ricohsp210 | SP 210/310 (mono) | DEB binary only | (not inspected) |
| **this project** | **SP C240SF (color)** | **C (libcups)** | **DDST binary (GDIJ/GDIP/GDIB)** |

## Protocol family observation

All inspected mono Ricoh GDI printers use a PJL-wrapped JBIG protocol:

```
%-12345X@PJL\r\n
@PJL SET COMPRESS=JBIG\r\n
... key=value lines ...
@PJL SET IMAGELEN=<bytes>\r\n
<JBIG payload>
@PJL SET PAGESTATUS=END\r\n
@PJL EOJ\r\n%-12345X
```

The color SP C240SF uses a binary "DDST" protocol with framed headers:

```
GJET (168 bytes)
GDIJ (120 bytes)
  GDIP (64 bytes)
    GDIB (32 bytes) + K JBIG + Y JBIG + M JBIG + C JBIG
    ...
JIDG (4 bytes)
```

The two are not interchangeable. Mac/Windows shipped DDST handlers for the
C240SF (see `RicohAficioSPC240SFFilter` and `gfegdrv.dll`); the SP100/200
PJL bash scripts cannot drive this printer.

## JBIG parameter cross-check

The mono PJL drivers and the C240SF DDST driver all use jbigkit, but with
slightly different BIH options:

| Driver | order | options | L0 stripe |
|--------|-------|---------|-----------|
| ricoh-sp100 / pstoricohddst-gdi | 0x03 | 0x48 (`-p 72`) | default |
| funinkina/Ricoh-SP200-Linux | 0x03 | 0x48 | 128 |
| Mac libDJZModule (SP C240SF) | 0x03 | **0x40** | **256** |

Our `rastertoddst.py` uses `pbmtojbg -s 256 -p 64 -o 3` (= order 0x03,
options 0x40, L0 256), matching the SP C240SF binary disassembly. If the
real device rejects the stream, the first thing to try is `-p 72` to flip
TPBON, since the SP200 maintainer documents that 0x40 by itself wedges
their (different) hardware.

## What the comparison validates

* "Two-step" structure — per-page headers ahead of JBIG payload — is the
  correct shape across the whole Ricoh GDI family.
* `jbigkit` with `order=0x03, options ∈ {0x40,0x48}` is the right encoder
  configuration; we don't need a custom JBIG implementation.
* Mono printers strip everything down to a single K plane. The C240SF
  is the first model in this family that exposes per-plane (K, Y, M, C)
  bands in its host protocol — which is why the binary GDIB header
  reserves four separate size fields.

## What the comparison cannot validate

* The exact byte layout of the GDIJ/GDIP/GDIB headers — no other
  open-source driver implements them, and the only ground truth is the
  macOS binary (and the Windows DLLs once disassembled). Real-hardware
  testing remains required.
