#!/usr/bin/env bash
# One-shot installer for the Ricoh Aficio SP C240SF printer driver.
#
# On Zorin OS / Ubuntu / Debian:
#   sudo ./install.sh                       # build + install + auto-detect
#   sudo ./install.sh socket://IP:9100      # build + install + register queue
#
# Without arguments and with the printer plugged in via USB, the bundled
# udev rule + helper will create the CUPS queue automatically.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Please run as root: sudo $0 $*" >&2
    exit 1
fi

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
URI="${1:-}"
QUEUE="${2:-RicohSPC240SF}"

step() { echo; echo "==> $*"; }

step "Installing build & runtime dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential pkg-config \
    cups cups-filters \
    libcups2-dev libcupsimage2-dev \
    libjbig-dev libjbig0

step "Building rastertoddst (multi-threaded C filter)"
make -C "${SRC_DIR}" clean
make -C "${SRC_DIR}"

step "Installing filter, PPD and udev rule"
make -C "${SRC_DIR}" install

if [[ -n "${URI}" ]]; then
    step "Registering CUPS queue '${QUEUE}' → '${URI}'"
    PPD="/usr/share/cups/model/Ricoh-Aficio_SP_C240SF-rastertoddst.ppd"
    if lpstat -v 2>/dev/null | grep -q "device for ${QUEUE}:"; then
        lpadmin -x "${QUEUE}" || true
    fi
    lpadmin -p "${QUEUE}" -E -v "${URI}" -P "${PPD}" \
        -o printer-is-shared=false \
        -o printer-error-policy=retry-current-job
    echo "Default options:"
    lpoptions -p "${QUEUE}" || true
else
    step "No URI given; relying on udev auto-register"
    echo "If the printer is on USB and powered on, plug it again or run:"
    echo "    sudo udevadm trigger --subsystem-match=usb --action=add"
    echo
    echo "Or register manually:"
    echo "    sudo lpadmin -p ${QUEUE} -E -v <uri> \\"
    echo "         -P /usr/share/cups/model/Ricoh-Aficio_SP_C240SF-rastertoddst.ppd"
    echo
    echo "Discover URIs with:  lpinfo -v | grep -i ricoh"
fi

step "Done."
echo
echo "Test print:"
echo "    lp -d ${QUEUE} /usr/share/cups/data/testprint"
echo
echo "Speed knob (per-job): RASTERTODDST_THREADS=0 disables parallel JBIG."
