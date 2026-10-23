# USB debugging workflow

The Mac sees VitaShell's USB storage as `/Volumes/Untitled`. Keep the USB cable
connected and configure VitaShell's USB device as the storage containing
`ux0:data/gangstarrio`.

Start the watcher on the Mac:

```sh
./scripts/watch-vita-usb.sh
```

After a test run, return to VitaShell and press `SELECT` to enable USB mode.
The watcher copies and SHA-256 verifies these files automatically:

```text
ux0:data/gangstarrio/loader.log -> runtime_logs/latest-loader.log
ux0:data/psp2core-*.psp2dmp    -> runtime_logs/cores/
```

VitaShell exposes the storage only while VitaShell is open in USB mode, so the
file log cannot remain mounted while the game is running. The watcher removes
the need to eject the SD card and captures the diagnostics as soon as USB mode
is enabled again.
