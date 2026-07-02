# Scan to Samba share on Zorin OS

Practical workflow that works today, without any driver: configure the
SP C240SF to push scans to a Samba share on the Zorin OS host, then
trigger scans from the printer's front panel.

## On Zorin OS

```bash
sudo apt install samba
sudo mkdir -p /srv/scans
sudo chown nobody:nogroup /srv/scans
sudo chmod 0775 /srv/scans

# Add a guest-writable share. Append to /etc/samba/smb.conf:
sudo tee -a /etc/samba/smb.conf <<'EOF'

[scans]
   path = /srv/scans
   browseable = yes
   read only = no
   guest ok = yes
   create mask = 0664
   directory mask = 0775
   force user = nobody
EOF

sudo systemctl restart smbd
```

Verify the share is reachable:

```bash
smbclient -L //localhost -N
```

## On the printer

1. Open *Web Image Monitor*: `http://<printer-ip>/`.
2. Login as Administrator (default: `admin` with no password).
3. **Device Management → Address Book → Add User**.
4. Set **Folder** authentication to `Specify Other Authentication
   Information` if your share requires it, otherwise leave guest.
5. **Folder → SMB**:
   * Path: `\\<linux-host-ip>\scans`
   * Account: leave blank for guest, or your user.
6. Save.

## Scanning

1. At the printer, press **Scanner**.
2. Select **Folder** as destination.
3. Pick the Address Book entry created above.
4. Choose color/resolution/single or duplex from the panel.
5. Press **Start**.

Files appear in `/srv/scans/` as `RICOH-<timestamp>.pdf` (or `.tif`/`.jpg`
depending on the selected format on the device).

## Auto-OCR (optional)

```bash
sudo apt install tesseract-ocr ocrmypdf inotify-tools

# Watcher that OCRs every new PDF:
( cd /srv/scans
  while inotifywait -q -e close_write --format '%f' .; do
      for f in *.pdf; do
          out="ocr-$f"
          [ -f "$out" ] && continue
          ocrmypdf --skip-text "$f" "$out" 2>/dev/null
      done
  done
) &
```
