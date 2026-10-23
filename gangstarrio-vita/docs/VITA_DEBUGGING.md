# Vita debugging over Vita Companion FTP

Use this workflow when the game is running on the Vita and you need a
repeatable image of what it rendered, plus the matching diagnostics, without
switching to VitaShell USB. FTP transfers files; it does not provide a live
screen or video stream.

## One-time connection setup

1. Connect the Vita and Mac to the same reachable network and start/enable the
   FTP service in Vita Companion on the Vita.
2. If the project has no local `.vita-dev.env`, copy
   `.vita-dev.env.example` to `.vita-dev.env`. Set `VITA_IP` to the Vita's
   address; the example's FTP port is `1337`. Keep `.vita-dev.env` local; it is
   ignored by Git.
3. Run `./tools/vita-dev.sh status`. This confirms the Companion command
   endpoint used by the project, but is not by itself a test of FTP access.

## Capture and fetch a screenshot

1. While the game is showing the scene to inspect, press **PS + START** on the
   Vita to use its built-in screenshot capture. A memory card is required and
   some applications/scenes may disallow capture. Confirm the new image appears
   in Photos on the Vita if the capture notification is unclear.
2. Vita screenshots are normally stored under
   `ux0:picture/SCREENSHOT/`. List the directory before and after a capture;
   identify the new filename in the second listing:

   ```sh
   source .vita-dev.env
   curl --fail --silent --show-error --ftp-method nocwd --list-only \
     "ftp://${VITA_IP}:${VITA_FTP_PORT}//ux0:/picture/SCREENSHOT/"
   ```

3. Download only the screenshot for the current test into the ignored run
   directory. Replace `FILENAME_FROM_LIST` with the exact listed name:

   ```sh
   source .vita-dev.env
   capture_dir="debug/runs/$(date '+%Y%m%d-%H%M%S')/screenshots"
   mkdir -p "$capture_dir"
   (
     cd "$capture_dir"
     curl --fail --silent --show-error --ftp-method nocwd -O \
       "ftp://${VITA_IP}:${VITA_FTP_PORT}//ux0:/picture/SCREENSHOT/FILENAME_FROM_LIST"
   )
   ```

4. Open the downloaded image and compare it with the same named scenario from
   another build. Put the build hash, scene/location, action and capture time
   in the run record. Do not upload personal screenshots or the Vita's full
   picture directory to Git.

If FTP cannot list/download while Rio is in the foreground, leave the game
state unchanged if possible, retry after returning to Vita Companion, or fetch
the saved file after exiting the game. The screenshot remains on the Vita; do
not switch to USB unless the network transfer is unavailable and USB is
specifically needed for a crash dump.

## Fetch the matching log

`./tools/vita-dev.sh pull` saves the current `ux0:data/gangstarrio/loader.log`
under `debug/runs/` and copies the local unstripped ELF when available. It uses
USB first if the configured USB volume is mounted; otherwise it uses FTP when
Vita Companion is reachable. For a guaranteed FTP-only transfer, download the
exact file directly:

```sh
source .vita-dev.env
run_dir="debug/runs/$(date '+%Y%m%d-%H%M%S')"
mkdir -p "$run_dir"
curl --fail --silent --show-error --ftp-method nocwd \
  "ftp://${VITA_IP}:${VITA_FTP_PORT}//ux0:/data/gangstarrio/loader.log" \
  -o "$run_dir/loader.log"
```

To fetch a crash dump, list `ux0:/data/` over FTP, then use the screenshot
download pattern with the exact `psp2core-...psp2dmp` filename. Keep the log,
dump and screenshot under the same run directory. Fetch the log promptly: a
later app launch recreates it, so it may replace the previous run's contents.

The `./tools/vita-dev.sh logs` command is a separate TCP listener for
PrincessLog on port `3333`; it is not FTP and cannot show the screen. The Rio
source currently writes its primary timestamped trace to `loader.log`; verify
that the running Vita build actually emits PrincessLog before relying on that
listener for live diagnostics.

## What FTP can and cannot show

FTP is the preferred low-risk path for retrieving a native screenshot and the
existing diagnostic log. It can provide successive manually captured stills,
not continuous video or audio. For a truly live screen, a separate Vita screen
streaming solution is required; do not add a kernel plugin to the currently
unstable Rio setup as part of routine diagnosis. Assess streaming separately,
with a reversible test and a known-good build, because it changes the Vita's
runtime/plugin environment and some solutions compete for USB services.
