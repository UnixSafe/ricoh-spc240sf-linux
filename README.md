# Ricoh Aficio SP C240SF — Linux printer driver

Native C CUPS filter (`rastertoddst`) + PPD + udev auto-registration for
the Ricoh Aficio SP C240SF color laser MFP on Zorin OS / Ubuntu / Debian.

Reverse-engineered from the macOS RicohAficioSPC240SFFilter binary; the
SP C240SF speaks a proprietary "DDST" binary protocol with `GJET`,
`GDIJ`, `GDIP`, `GDIB` headers wrapping per-plane (CMYK) JBIG payloads.
The wire format is documented in [`DDST_FORMAT.md`](DDST_FORMAT.md).

## Performance

Measured on a 4-core x86_64 with a synthetic A4 600 dpi CMYK page
(4960 × 7016 pixels, ~133 MB raster):

| Build       | Wall time | CPU usage |
|-------------|-----------|-----------|
| Python      | ~30 s     | 100 %     |
| C, single-threaded | 0.79 s    | 65 %      |
| C, multi-threaded  | **0.17 s** | **290 %** |

The filter encodes the four CMYK JBIG planes of each band in parallel.
Set `RASTERTODDST_THREADS=0` in the job environment to force serial
encoding (useful for diagnosing crashes).

## One-shot install

```
git clone <this repo>
cd driver_ricoh_linux
sudo ./install.sh                          # auto-detects USB device
sudo ./install.sh socket://IP:9100         # explicit network URI
```

`install.sh` apt-installs the build deps, compiles `rastertoddst`,
deploys the filter / PPD / udev rule and (optionally) creates the CUPS
queue. With the printer plugged via USB, the udev rule auto-registers
the queue on first plug — no `lpadmin` call needed.

## Manual install

```
sudo apt install build-essential pkg-config \
                 libcups2-dev libcupsimage2-dev libjbig-dev
make
sudo make install
sudo lpadmin -p RicohSPC240SF -E \
     -v socket://192.168.1.42:9100 \
     -P /usr/share/cups/model/Ricoh-Aficio_SP_C240SF-rastertoddst.ppd
```

## Build a .deb

On the target host (Zorin OS / Ubuntu):

```
sudo apt install devscripts debhelper
make deb
sudo apt install ../ricoh-spc240sf-driver_1.0.0-1_amd64.deb
```

The package handles install / uninstall cleanly, drops the udev rule,
reloads CUPS, and removes orphaned queues on `apt purge`.

## What's in the box

| File | Role |
|------|------|
| `rastertoddst.c` | The CUPS filter source (multi-threaded C). |
| `Ricoh-Aficio_SP_C240SF-rastertoddst.ppd` | PPD with paper, trays, duplex, color modes. |
| `Makefile` | `make`, `make install`, `make deb`. |
| `install.sh` | One-shot installer (apt + make + lpadmin). |
| `debian/` | Source package, postinst, prerm, udev rule, auto-register helper. |
| `parse_ddst.py` | DDST stream validator (decodes headers). |
| `make_test_raster.py` | Synthesize a CUPS raster page for testing. |
| `DDST_FORMAT.md` | Wire-format reverse-engineering notes. |
| `COMPARISON.md` | Cross-check vs. open-source mono Ricoh drivers. |
| `ANALYSE_COMPATIBILITE.md` | Evidence-backed compatibility diagnosis and target checks. |

## Testing without the printer

```
python3 make_test_raster.py > /tmp/page.raster
./rastertoddst 1 user "title" 1 "PageSize=A4 ColorModel=RGB" /tmp/page.raster > /tmp/out.ddst
python3 parse_ddst.py /tmp/out.ddst
```

Expected: GJET → GDIJ → GDIP → GDIB(K,Y,M,C) → JIDG with consistent
plane sizes and a final JIDG terminator.

## Status

* ✅ Wire format cross-checked against the Mac driver's static header layouts.
* ✅ Multi-threaded C implementation.
* ✅ One-shot Zorin OS install + .deb package.
* ✅ Udev auto-registration on USB plug.
* ⚠️  Halftoning uses a Bayer 8×8 matrix. The Windows driver ships
  proprietary halftone screens (`gfegs{c,m,y,k}{1,2,4}.bin`) that
  produce nicer output. Print quality is correct but not optimal.
* ⚠️  Real-hardware testing is required to confirm bit-exact
  compatibility with the printer's firmware.

See [`ANALYSE_COMPATIBILITE.md`](ANALYSE_COMPATIBILITE.md) for the
confirmed corrections, remaining risks, and CUPS diagnostics.

## Performance knobs

| Knob | Effect |
|------|--------|
| `RASTERTODDST_THREADS=0` | Disable parallel JBIG encoding (debug). |
| `make CFLAGS=-O3` | Slight gain on the halftone inner loop. |
| `make CFLAGS="-O3 -march=native"` | Best on the target machine. |
