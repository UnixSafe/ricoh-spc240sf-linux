#!/usr/bin/env bash
#
# USB installer for the Ricoh Aficio SP C240SF driver on Zorin OS / Ubuntu.
#
# Plug the printer in via USB, power it on, then run:
#
#     sudo ./install-usb.sh
#
# The script installs the dependencies, builds and deploys the CUPS filter +
# PPD, auto-detects the USB-connected printer, creates the print queue, and
# offers to print a CUPS test page. No printer URI needs to be typed.
#
# Optional second argument overrides the queue name (default: RicohSPC240SF):
#     sudo ./install-usb.sh RicohSPC240SF

set -euo pipefail

QUEUE="${1:-RicohSPC240SF}"
PPD_NAME="Ricoh-Aficio_SP_C240SF-rastertoddst.ppd"
PPD_DST="/usr/share/cups/model/${PPD_NAME}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

step() { echo; echo "==> $*"; }
warn() { echo "  ! $*" >&2; }

if [[ $EUID -ne 0 ]]; then
    echo "Please run as root:  sudo $0 $*" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
step "1/6  Installing dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential pkg-config \
    cups cups-filters \
    libcups2-dev libcupsimage2-dev \
    libjbig-dev libjbig0 \
    usbutils

systemctl enable --now cups >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
step "2/6  Building the filter"
make -C "${SRC_DIR}" clean
make -C "${SRC_DIR}"

step "3/6  Installing filter, PPD and udev rule"
make -C "${SRC_DIR}" install

# ---------------------------------------------------------------------------
step "4/6  Checking the USB connection"
# Ricoh USB vendor id is 05ca. Confirm the hardware is actually enumerated
# before we go hunting for a CUPS device URI.
if lsusb 2>/dev/null | grep -qiE '05ca:|ricoh'; then
    echo "  Ricoh USB device detected:"
    lsusb | grep -iE '05ca:|ricoh' | sed 's/^/    /'
else
    warn "No Ricoh USB device found by lsusb."
    warn "Check that the printer is powered on and the USB cable is connected,"
    warn "then re-run this script. (Continuing anyway in case lsusb is filtered.)"
fi

# ---------------------------------------------------------------------------
step "5/6  Detecting the printer's CUPS device URI"
# CUPS enumeration can lag a few seconds after plug-in; poll for it.
URI=""
for attempt in 1 2 3 4 5 6 7 8; do
    URI="$(lpinfo -v 2>/dev/null \
        | awk '/^direct +usb:/ && (/C240/ || /C240SF/ || /Ricoh/ || /RICOH/) {print $2; exit}' \
        || true)"
    [[ -n "${URI}" ]] && break
    # Fall back to ANY usb:// device if exactly one is present.
    if [[ -z "${URI}" ]]; then
        mapfile -t USB_URIS < <(lpinfo -v 2>/dev/null | awk '/^direct +usb:/ {print $2}')
        if [[ ${#USB_URIS[@]} -eq 1 ]]; then
            URI="${USB_URIS[0]}"
            break
        fi
    fi
    sleep 2
done

if [[ -z "${URI}" ]]; then
    warn "Could not auto-detect the USB printer URI."
    echo
    echo "  Available USB devices CUPS can see:"
    lpinfo -v 2>/dev/null | grep -E '^direct +usb:' | sed 's/^/    /' || echo "    (none)"
    echo
    echo "  Fix and finish manually:"
    echo "    1. Make sure the printer is on and connected."
    echo "    2. Add yourself to the 'lp' group if needed:  sudo usermod -aG lp \$USER"
    echo "    3. Register the queue with the URI shown above:"
    echo "         sudo lpadmin -p ${QUEUE} -E -v <usb-uri> -P ${PPD_DST} \\"
    echo "              -o printer-is-shared=false"
    exit 1
fi
echo "  Found: ${URI}"

# ---------------------------------------------------------------------------
step "6/6  Registering the print queue '${QUEUE}'"
# Idempotent: drop any pre-existing queue of the same name first.
if lpstat -v 2>/dev/null | grep -q "device for ${QUEUE}:"; then
    echo "  Removing existing queue '${QUEUE}'..."
    lpadmin -x "${QUEUE}" || true
fi

lpadmin -p "${QUEUE}" -E \
    -v "${URI}" \
    -P "${PPD_DST}" \
    -o printer-is-shared=false \
    -o printer-error-policy=retry-current-job

# Make sure it accepts jobs and is enabled.
cupsenable "${QUEUE}" 2>/dev/null || true
cupsaccept "${QUEUE}" 2>/dev/null || true
# Set as system default if no default is configured yet.
lpstat -d 2>/dev/null | grep -q "system default destination" || lpadmin -d "${QUEUE}" || true

echo
echo "  Queue status:"
lpstat -p "${QUEUE}" 2>/dev/null | sed 's/^/    /' || true

# ---------------------------------------------------------------------------
echo
echo "============================================================"
echo "  Installation complete."
echo "============================================================"
echo
echo "  Print a CUPS test page now with:"
echo "      lp -d ${QUEUE} /usr/share/cups/data/testprint"
echo
echo "  Tip: start with a simple black-and-white page first."
echo "  Speed knob (per job): RASTERTODDST_THREADS=0 disables parallel JBIG."
echo

# Offer an immediate test page if running interactively.
if [[ -t 0 && -r /usr/share/cups/data/testprint ]]; then
    read -r -p "  Print a test page now? [y/N] " ans
    case "${ans}" in
        [yY]|[yY][eE][sS])
            lp -d "${QUEUE}" /usr/share/cups/data/testprint && \
                echo "  Test page submitted. Check the printer." ;;
        *) echo "  Skipped. You can print it anytime with the command above." ;;
    esac
fi
