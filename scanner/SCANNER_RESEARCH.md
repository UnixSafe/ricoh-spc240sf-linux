# Scanning options for the Ricoh Aficio SP C240SF on Linux

The MFP is 2011-era so it predates AirScan (eSCL) and WSD-Scan as standards.
Its scanning surface (flatbed + 35-page ADF) is exposed over USB and over the
network via:

* **Network TWAIN** — Ricoh's proprietary protocol (the official Windows
  driver, `RICOH Network TWAIN Driver`). The transport is HTTP+SOAP-ish to
  the device on TCP/80 + a side channel for the raw scan stream.
* **DSM / Distributed Scan** — a SOAP scan-to-folder/Email/FTP flow
  configured in the printer's *Web Image Monitor* (WIM). Push-from-printer
  rather than pull-from-host, so it isn't a SANE driver.
* **USB / USB-TWAIN** — same protocol as Network TWAIN but tunnelled
  over a USB scanner class endpoint. The Windows `Type 4` driver presents
  it as a WIA/TWAIN source. Linux has no shipping backend for this.

It does **not** support:
* AirScan / eSCL (came to Ricoh around 2014).
* WSD-Scan (Microsoft Web Services for Devices Scan).
* IPP Scan.

This means there is no out-of-the-box SANE backend that will pick up the
SP C240SF. Three realistic paths forward:

## A. Push-from-printer to a Samba/FTP share (no code)

Easiest and works today. Set up a Samba share on the Zorin OS host and tell
the printer to scan-to-folder via WIM. End user scans from the device's
front panel. Documented in `scanner/SETUP_SCAN_TO_SMB.md`.

## B. Minimal SANE backend that drives the printer via HTTP/SOAP

The Windows TWAIN driver talks to the printer at:

```
POST /scancontrol  HTTP/1.1
Host: <printer-ip>
Content-Type: application/octet-stream
... binary SOAP-like blob (Ricoh proprietary) ...
```

The shape of those blobs is not publicly documented. To implement a real
Linux SANE backend we need:

1. A USB or network packet capture of a successful Windows scan.
2. Decoded request/response pairs for: get capabilities, set scan params,
   start scan, fetch pages.
3. The image data is delivered as JFIF (JPEG) for color and as raw 8-bit
   PGM or G4-TIFF for mono.

`sane_ricoh_spc240sf.c` (in this repo) contains the skeleton — device
discovery, SANE option machinery, and stubs that need to be filled in
once we have captures. It already implements `sane_init`, `sane_exit`,
`sane_get_devices`, and option handling so that `scanimage --help -d
ricoh-spc240sf` works.

## C. Reuse sane-airscan as a probe

`sane-airscan` will fail to find the device (no eSCL endpoint) but it is
the right tool to confirm that fact and to keep an eye on Ricoh firmware
updates that may eventually add eSCL support. Install it from the Zorin OS
repos:

```
sudo apt install sane-airscan
airscan-discover
```

## D. (Future) escl-emulator proxy

If the device exposes any scan-to-Email JPEG flow over HTTP, we could
build a daemon that polls the printer, captures the pushed JPEGs, and
re-exposes them as an eSCL endpoint that `sane-airscan` consumes. Lots
of moving parts but no kernel/USB work.

---

## Ports observed on a typical SP C240SF (from Ricoh service docs)

| Port | Protocol | Notes |
|------|----------|-------|
| 80   | HTTP     | WIM, /scancontrol scanner SOAP |
| 443  | HTTPS    | WIM (when enabled) |
| 161  | SNMP     | status, configuration |
| 515  | LPR      | printing |
| 631  | IPP      | printing (no scan) |
| 9100 | JetDirect/RAW | printing |
| 26470| Ricoh    | scan stream side channel (firmware-dependent) |
| 26471| Ricoh    | scan control |

The exact scan ports vary by firmware; some units use 5358, 7443, or
49152+ dynamic allocations. Confirm with `nmap <printer-ip>`.
