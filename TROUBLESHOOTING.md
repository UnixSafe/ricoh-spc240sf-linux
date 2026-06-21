# Troubleshooting

## Quick checks

Confirm that CUPS can see the queue and its device URI:

```sh
lpstat -v -p RicohSPC240SF
lpinfo -v | grep -i ricoh
```

For a USB-connected printer, run the installer after the printer is powered
on and connected:

```sh
sudo ./install-usb.sh
```

For a network-connected printer, install with its explicit RAW socket URI:

```sh
sudo ./install.sh socket://PRINTER_IP:9100
```

## Print a test page

```sh
lp -d RicohSPC240SF /usr/share/cups/data/testprint
```

If the queue is paused, re-enable it:

```sh
sudo cupsenable RicohSPC240SF
sudo cupsaccept RicohSPC240SF
```

## Inspect CUPS errors

Enable debug logging, submit one test job, then inspect the recent log:

```sh
sudo cupsctl --debug-logging
lp -d RicohSPC240SF /usr/share/cups/data/testprint
sudo tail -n 200 /var/log/cups/error_log
```

Useful indicators include a missing `rastertoddst` filter, an unsupported
raster colour space, or a device URI that CUPS cannot open.

## Validate the filter without a printer

```sh
make clean all
python3 make_test_raster.py > /tmp/spc240sf.raster
./rastertoddst 1 user test 1 'PageSize=A4 ColorModel=RGB' \
    /tmp/spc240sf.raster > /tmp/spc240sf.ddst
python3 parse_ddst.py /tmp/spc240sf.ddst
```

The output should contain `GJET`, `GDIJ`, `GDIP`, `GDIB`, and a final `JIDG`.
The `GDIJ` line reports the requested number of copies.

## USB detection delay

After a new USB connection, CUPS can take several seconds to publish the
device URI. The bundled helper waits up to 30 seconds. If no queue appears,
run `sudo ./install-usb.sh` to display the URIs detected by CUPS and register
the queue explicitly.
