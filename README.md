# ricoh-spc240sf-linux

Unofficial Linux driver for the Ricoh Aficio SP C240SF color laser MFP.

Provides a native C CUPS filter (`rastertoddst`), a PPD with paper sizes,
trays, duplex and color modes, a Debian package, and a udev rule that
auto-registers the printer in CUPS when plugged via USB.

This is an independent community driver. It is not endorsed by or
affiliated with Ricoh.

## Why

The vendor stopped shipping Linux drivers for this printer family. This
project keeps the hardware usable on modern distributions (Zorin OS,
Ubuntu, Debian and derivatives).

## Performance

Native C, multi-threaded: the four CMYK image planes of each band are
encoded in parallel.

Measured on a 4-core x86_64 host with a synthetic A4 600 dpi CMYK page
(4960 × 7016 pixels, ~133 MB raw raster):

| Build                    | Wall time | CPU usage |
|--------------------------|-----------|-----------|
| Single-threaded          | 0.79 s    | 65 %      |
| Multi-threaded (default) | **0.17 s** | **290 %** |

Set `RASTERTODDST_THREADS=0` in the job environment to force serial
encoding.

## One-shot install

```
git clone https://github.com/UnixSafe/ricoh-spc240sf-linux
cd ricoh-spc240sf-linux

# USB (Zorin OS / Ubuntu): plug the printer in, power it on, then:
sudo ./install-usb.sh                      # auto-detects the USB printer

# Or the generic installer:
sudo ./install.sh                          # USB via udev auto-register
sudo ./install.sh socket://IP:9100         # network: explicit URI
```

`install-usb.sh` is the easiest path: it installs the dependencies, builds
and deploys the filter + PPD, **auto-detects the USB-connected printer**,
creates the CUPS queue, and offers to print a test page — no URI to type.

`install.sh` is the generic variant: it does the same build/deploy and
either takes an explicit URI or relies on the bundled udev rule to
auto-register the queue on USB plug-in.

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

```
sudo apt install devscripts debhelper
make deb
sudo apt install ../ricoh-spc240sf-driver_1.0.0-1_amd64.deb
```

## Repository layout

| Path | Role |
|------|------|
| `rastertoddst.c` | CUPS filter source. |
| `Ricoh-Aficio_SP_C240SF-rastertoddst.ppd` | PPD: paper, trays, duplex, color. |
| `Makefile` | `make`, `make install`, `make deb`. |
| `install-usb.sh` | USB installer — auto-detects the printer. |
| `install.sh` | Generic installer (USB udev / network URI). |
| `debian/` | Debian source package, postinst, prerm, udev rule. |
| `parse_ddst.py` | Output validator. |
| `make_test_raster.py` | Synthesize a CUPS raster page for testing. |

## Smoke test without the printer

```
python3 make_test_raster.py > /tmp/page.raster
./rastertoddst 1 user "title" 1 "PageSize=A4 ColorModel=RGB" \
    /tmp/page.raster > /tmp/out.bin
python3 parse_ddst.py /tmp/out.bin
```

## Status

* Multi-threaded C filter — done.
* PPD with A4 / Letter / Legal / A5 / B5 / B6 / A6 / Executive /
  envelopes / postcard, duplex, two trays — done.
* One-shot installer and Debian package — done.
* USB auto-registration via udev — done.
* Halftoning uses a Bayer 8×8 ordered dither. Print quality is correct
  but not as fine as the vendor driver. Pull requests with better
  halftone screens are welcome.
* Real-hardware testing reports welcome via the issue tracker.

## Troubleshooting

See [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) for installation checks,
CUPS diagnostics, and USB troubleshooting.

## License

GPL-2.0-or-later. See `LICENSE`.
