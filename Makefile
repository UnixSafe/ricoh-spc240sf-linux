# Build rules for the Ricoh Aficio SP C240SF printer driver (Linux/Zorin).
#
# Default target builds only the printer filter — fast and minimal deps.
#
# Targets:
#   make                  build rastertoddst (default)
#   make install          install filter + PPD + udev (run as root)
#   make uninstall        remove the installed files
#   make deb              build a .deb package (Debian-based hosts only)
#   make clean
#   make sane             optional: build the unfinished scanner backend
#
# Runtime deps (Debian/Ubuntu/Zorin):
#   sudo apt install build-essential pkg-config \
#                    libcups2-dev libcupsimage2-dev libjbig-dev

.DEFAULT_GOAL := all

PREFIX           ?= /usr
CUPS_FILTER_DIR  ?= $(PREFIX)/lib/cups/filter
CUPS_HELPER_DIR  ?= $(PREFIX)/lib/cups
CUPS_PPD_DIR     ?= $(PREFIX)/share/cups/model
UDEV_RULES_DIR   ?= /lib/udev/rules.d

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -D_GNU_SOURCE
LDFLAGS ?=

CUPS_CFLAGS := $(shell cups-config --cflags 2>/dev/null)
CUPS_LIBS   := $(shell cups-config --image --libs 2>/dev/null)
ifeq ($(strip $(CUPS_LIBS)),)
CUPS_LIBS   := -lcupsimage -lcups
endif

# libjbig: prefer the system shared library (Debian: libjbig-dev). Fall
# back to the in-tree jbigkit source for local development or for
# building on a host without libjbig-dev installed.
JBIG_LOCAL := jbigkit-2.1/libjbig
ifneq ($(wildcard $(JBIG_LOCAL)/jbig.h),)
ifeq ($(strip $(shell echo '\#include <jbig.h>' \
    | $(CC) -E -x c - -o /dev/null 2>/dev/null && echo ok)),)
JBIG_CFLAGS := -I$(JBIG_LOCAL)
JBIG_LIBS   := $(JBIG_LOCAL)/libjbig.a
$(JBIG_LOCAL)/libjbig.a:
	$(MAKE) -C $(JBIG_LOCAL) libjbig.a
$(FILTER_BIN): $(JBIG_LOCAL)/libjbig.a
endif
endif
JBIG_LIBS   ?= -ljbig
PTHREAD_LIB := -pthread

FILTER_BIN  := rastertoddst
PPD_FILE    := Ricoh-Aficio_SP_C240SF-rastertoddst.ppd

.PHONY: all install uninstall deb clean sane

all: $(FILTER_BIN)

$(FILTER_BIN): rastertoddst.c
	$(CC) $(CFLAGS) $(CUPS_CFLAGS) $(JBIG_CFLAGS) $(PTHREAD_LIB) -o $@ $< \
	    $(CUPS_LIBS) $(JBIG_LIBS) $(LDFLAGS)

install: $(FILTER_BIN) $(PPD_FILE)
	install -d "$(DESTDIR)$(CUPS_FILTER_DIR)" \
	    "$(DESTDIR)$(CUPS_HELPER_DIR)" \
	    "$(DESTDIR)$(CUPS_PPD_DIR)" \
	    "$(DESTDIR)$(UDEV_RULES_DIR)"
	install -m 0755 $(FILTER_BIN) \
	    "$(DESTDIR)$(CUPS_FILTER_DIR)/$(FILTER_BIN)"
	install -m 0755 debian/spc240sf-autoregister \
	    "$(DESTDIR)$(CUPS_HELPER_DIR)/spc240sf-autoregister"
	install -m 0644 $(PPD_FILE) \
	    "$(DESTDIR)$(CUPS_PPD_DIR)/$(PPD_FILE)"
	install -m 0644 debian/99-ricoh-spc240sf.rules \
	    "$(DESTDIR)$(UDEV_RULES_DIR)/99-ricoh-spc240sf.rules"
	@if command -v udevadm >/dev/null && [ -z "$(DESTDIR)" ]; then \
	    udevadm control --reload-rules || true; \
	    udevadm trigger --subsystem-match=usb --action=add || true; \
	fi
	@if command -v systemctl >/dev/null && [ -z "$(DESTDIR)" ]; then \
	    systemctl reload-or-restart cups.service || true; \
	fi

uninstall:
	rm -f "$(CUPS_FILTER_DIR)/$(FILTER_BIN)"
	rm -f "$(CUPS_HELPER_DIR)/spc240sf-autoregister"
	rm -f "$(CUPS_PPD_DIR)/$(PPD_FILE)"
	rm -f "$(UDEV_RULES_DIR)/99-ricoh-spc240sf.rules"
	@command -v udevadm >/dev/null && udevadm control --reload-rules || true

deb:
	dpkg-buildpackage -us -uc -b
	@echo "Built .deb is in the parent directory."

# Optional: scanner backend (incomplete, see scanner/SCANNER_RESEARCH.md).
SANE_LIB := libsane-ricoh-spc240sf.so
sane: $(SANE_LIB)
$(SANE_LIB): sane_ricoh_spc240sf.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -lcurl $(LDFLAGS)

clean:
	rm -f $(FILTER_BIN) $(SANE_LIB) *.o
