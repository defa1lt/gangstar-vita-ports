#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

if [[ -f .vita-dev.env ]]; then
    # shellcheck disable=SC1091
    source .vita-dev.env
fi

VITA_TITLEID="${VITA_TITLEID:-GSRV00001}"
VITA_FTP_PORT="${VITA_FTP_PORT:-1337}"
VITA_CMD_PORT="${VITA_CMD_PORT:-1338}"
VITA_LOG_PORT="${VITA_LOG_PORT:-3333}"
VITA_USB_MOUNT="${VITA_USB_MOUNT:-/Volumes/Untitled}"
VITA_APP_DIR="${VITA_APP_DIR:-ux0:/app/$VITA_TITLEID}"
VITA_LOG_REMOTE="${VITA_LOG_REMOTE:-ux0:/data/gangstarrio/loader.log}"
VITA_BUILD_COMMAND="${VITA_BUILD_COMMAND:-./scripts/build.sh}"
VITA_EBOOT_PATH="${VITA_EBOOT_PATH:-build/eboot.bin}"
VITA_ELF_PATH="${VITA_ELF_PATH:-build/gangstarrio_vita}"
VITA_VPK_PATH="${VITA_VPK_PATH:-build/gangstarrio-vita.vpk}"
VITA_DEBUG_DIR="${VITA_DEBUG_DIR:-debug}"

sdk_workspace="$(dirname "$project_dir")"
if [[ ! -d "$sdk_workspace/.vitasdk-softfp" ]]; then
    sdk_workspace="$(dirname "$sdk_workspace")"
fi
if [[ -z "${VITA_SDK_PATH:-}" && -x "$sdk_workspace/.vitasdk-softfp/bin/arm-vita-eabi-addr2line" ]]; then
    VITA_SDK_PATH="$sdk_workspace/.vitasdk-softfp"
fi
if [[ -n "${VITA_SDK_PATH:-}" ]]; then
    export VITASDK="$VITA_SDK_PATH"
    export PATH="$VITASDK/bin:$PATH"
fi

die() { printf '[%s] [vita-dev] error: %s\n' "$(date '+%H:%M:%S')" "$*" >&2; exit 1; }
note() { printf '[%s] [vita-dev] %s\n' "$(date '+%H:%M:%S')" "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

remote_to_usb() {
    local path="$1"
    path="${path#ux0:/}"
    printf '%s/%s' "${VITA_USB_MOUNT%/}" "$path"
}

network_ready() {
    [[ -n "${VITA_IP:-}" ]] && have nc && nc -z -w 2 "$VITA_IP" "$VITA_CMD_PORT" >/dev/null 2>&1
}

usb_ready() {
    [[ -d "$VITA_USB_MOUNT" ]]
}

transport() {
    if network_ready; then
        printf network
    elif usb_ready; then
        printf usb
    else
        printf none
    fi
}

companion() {
    local command="$*"
    [[ -n "${VITA_IP:-}" ]] || die 'VITA_IP is empty; copy .vita-dev.env.example to .vita-dev.env'
    have nc || die 'nc is required for Vita Companion commands'
    printf '%s\n' "$command" | nc -w 4 "$VITA_IP" "$VITA_CMD_PORT"
}

require_artifact() {
    [[ -f "$1" ]] || die "missing artifact: $1 (run build first)"
}

build() {
    note "building: $VITA_BUILD_COMMAND"
    bash -lc "$VITA_BUILD_COMMAND"
    require_artifact "$VITA_ELF_PATH"
    require_artifact "$VITA_EBOOT_PATH"
}

archive_build() {
    require_artifact "$VITA_ELF_PATH"
    require_artifact "$VITA_EBOOT_PATH"
    local stamp out revision
    stamp="$(date '+%Y%m%d-%H%M%S')"
    out="$VITA_DEBUG_DIR/builds/$stamp"
    mkdir -p "$out"
    cp "$VITA_ELF_PATH" "$out/"
    cp "$VITA_EBOOT_PATH" "$out/"
    [[ -f "$VITA_VPK_PATH" ]] && cp "$VITA_VPK_PATH" "$out/" || true
    revision="$(git rev-parse --short HEAD 2>/dev/null || printf unknown)"
    printf 'timestamp=%s\nrevision=%s\ntitleid=%s\n' "$stamp" "$revision" "$VITA_TITLEID" > "$out/build.txt"
    if have shasum; then
        shasum -a 256 "$out/$(basename "$VITA_ELF_PATH")" "$out/$(basename "$VITA_EBOOT_PATH")" > "$out/SHA256SUMS"
    fi

    local builds=() old
    while IFS= read -r old; do builds+=("$old"); done < <(find "$VITA_DEBUG_DIR/builds" -mindepth 1 -maxdepth 1 -type d -print | sort -r)
    if (( ${#builds[@]} > 10 )); then
        for old in "${builds[@]:10}"; do
            [[ "$old" == "$VITA_DEBUG_DIR/builds/"* ]] || die "refusing to prune unexpected path: $old"
            rm -rf -- "$old"
        done
    fi
    note "debug symbols archived: $out"
}

status() {
    local mode
    mode="$(transport)"
    printf 'project:   %s\n' "$project_dir"
    printf 'title id:  %s\n' "$VITA_TITLEID"
    printf 'transport: %s\n' "$mode"
    printf 'VitaSDK:   %s\n' "${VITASDK:-unset}"
    printf 'elf:       %s %s\n' "$VITA_ELF_PATH" "$([[ -f "$VITA_ELF_PATH" ]] && printf OK || printf MISSING)"
    printf 'eboot:     %s %s\n' "$VITA_EBOOT_PATH" "$([[ -f "$VITA_EBOOT_PATH" ]] && printf OK || printf MISSING)"
    if [[ "$mode" == network ]]; then
        note "Vita Companion at $VITA_IP:$VITA_CMD_PORT"
        companion version || true
    elif [[ "$mode" == usb ]]; then
        note "USB mounted at $VITA_USB_MOUNT; remote launch and live logs are unavailable"
    else
        note 'Vita is not reachable. Set VITA_IP or mount VitaShell USB.'
    fi
}

kill_app() {
    network_ready || die 'kill requires Vita Companion on the network'
    note "stopping $VITA_TITLEID"
    companion kill "$VITA_TITLEID"
}

upload() {
    require_artifact "$VITA_EBOOT_PATH"
    local mode destination
    mode="$(transport)"
    if [[ "$mode" == network ]]; then
        have curl || die 'curl is required for FTP upload'
        note "uploading eboot through FTP to $VITA_APP_DIR"
        curl --fail --silent --show-error --ftp-method nocwd --ftp-create-dirs -T "$VITA_EBOOT_PATH" \
            "ftp://$VITA_IP:$VITA_FTP_PORT//${VITA_APP_DIR}/eboot.bin"
    elif [[ "$mode" == usb ]]; then
        destination="$(remote_to_usb "$VITA_APP_DIR")/eboot.bin"
        [[ -d "$(dirname "$destination")" ]] || die "installed app not found: $(dirname "$destination")"
        note "copying eboot to $destination"
        cp "$VITA_EBOOT_PATH" "$destination"
        sync
    else
        die 'Vita is not reachable by Companion or USB'
    fi
}

run_app() {
    network_ready || die 'run requires Vita Companion on the network'
    note "launching $VITA_TITLEID"
    companion launch "$VITA_TITLEID"
}

deploy() {
    build
    archive_build
    if network_ready; then
        kill_app || true
        upload
        run_app
    elif usb_ready; then
        upload
        note 'USB deploy complete; disconnect USB in VitaShell and launch the game manually'
    else
        die 'build succeeded, but Vita is not reachable'
    fi
}

install_vpk() {
    require_artifact "$VITA_VPK_PATH"
    if network_ready; then
        have curl || die 'curl is required for FTP upload'
        note 'uploading VPK to ux0:data (install it once from VitaShell)'
        curl --fail --silent --show-error --ftp-method nocwd --ftp-create-dirs -T "$VITA_VPK_PATH" \
            "ftp://$VITA_IP:$VITA_FTP_PORT//ux0:/data/$(basename "$VITA_VPK_PATH")"
    elif usb_ready; then
        mkdir -p "$VITA_USB_MOUNT/data"
        cp "$VITA_VPK_PATH" "$VITA_USB_MOUNT/data/"
        sync
        note 'VPK copied to ux0:data; install it from VitaShell'
    else
        die 'Vita is not reachable'
    fi
}

pull_logs() {
    local stamp out source
    stamp="$(date '+%Y%m%d-%H%M%S')"
    out="$VITA_DEBUG_DIR/runs/$stamp"
    mkdir -p "$out"
    if usb_ready; then
        source="$(remote_to_usb "$VITA_LOG_REMOTE")"
        [[ -f "$source" ]] || die "log not found: $source"
        cp "$source" "$out/loader.log"
    elif network_ready; then
        have curl || die 'curl is required for FTP download'
        curl --fail --silent --show-error --ftp-method nocwd \
            "ftp://$VITA_IP:$VITA_FTP_PORT//${VITA_LOG_REMOTE}" -o "$out/loader.log"
    else
        die 'Vita is not reachable'
    fi
    cp "$VITA_ELF_PATH" "$out/" 2>/dev/null || true
    note "saved log snapshot: $out/loader.log"
}

live_logs() {
    have nc || die 'nc is required for PrincessLog'
    note "listening for PrincessLog on TCP $VITA_LOG_PORT (Ctrl-C to stop)"
    mkdir -p "$VITA_DEBUG_DIR/live"
    local file
    file="$VITA_DEBUG_DIR/live/$(date '+%Y%m%d-%H%M%S').log"
    nc -lk "$VITA_LOG_PORT" | tee "$file"
}

crash() {
    local dump="${1:-}" elf="${2:-$VITA_ELF_PATH}" report
    [[ -n "$dump" && -f "$dump" ]] || die 'usage: vita-dev.sh crash FILE.psp2dmp [UNSTRIPPED_ELF]'
    [[ -f "$elf" ]] || die "unstripped ELF not found: $elf"
    [[ -n "${VITA_CRASHDUMP:-}" && -x "$VITA_CRASHDUMP" ]] || die 'set VITA_CRASHDUMP in .vita-dev.env'
    mkdir -p "$VITA_DEBUG_DIR/crashes"
    report="$VITA_DEBUG_DIR/crashes/$(date '+%Y%m%d-%H%M%S').txt"
    "$VITA_CRASHDUMP" "$dump" "$elf" | tee "$report"
    note "crash report: $report"
}

latest_crash() {
    usb_ready || die 'crash-latest currently requires a mounted VitaShell USB volume'
    [[ -n "${VITA_CRASHDUMP:-}" && -x "$VITA_CRASHDUMP" ]] || die 'set VITA_CRASHDUMP in .vita-dev.env'
    local files=() dump header
    shopt -s nullglob
    files=("$VITA_USB_MOUNT"/data/*.psp2dmp)
    shopt -u nullglob
    (( ${#files[@]} > 0 )) || die 'no .psp2dmp files found in ux0:data'
    while IFS= read -r dump; do
        header="$("$VITA_CRASHDUMP" "$dump" /dev/null 2>/dev/null | sed -n '1p')"
        if [[ "$header" == *\"$VITA_TITLEID\"* ]]; then
            note "latest $VITA_TITLEID dump: $(basename "$dump")"
            crash "$dump"
            return
        fi
    done < <(ls -1t "${files[@]}")
    die "no crash dump for $VITA_TITLEID found"
}

usage() {
    cat <<'EOF'
Usage: tools/vita-dev.sh COMMAND

  status       show artifacts and Vita connection
  build        build and validate ELF/eboot
  archive      archive unstripped ELF and eboot (keeps 10)
  kill         stop this title through Vita Companion
  upload       replace installed eboot (FTP or VitaShell USB)
  run          launch title through Vita Companion
  deploy       build, archive, kill, upload, launch
  install-vpk  upload the VPK for one-time VitaShell installation
  pull         save the current on-device loader.log with timestamp
  logs         listen for PrincessLog output
  crash FILE [ELF]  symbolicate a Vita crash dump
  crash-latest  find and symbolicate this title's newest USB dump
  reboot       reboot through Vita Companion
  destroy      emergency: stop all applications through Vita Companion
EOF
}

command_name="${1:-help}"
shift || true
case "$command_name" in
    status|doctor) status ;;
    build) build ;;
    archive) archive_build ;;
    kill) kill_app ;;
    upload) upload ;;
    run) run_app ;;
    deploy) deploy ;;
    install-vpk) install_vpk ;;
    pull|collect) pull_logs ;;
    logs) live_logs ;;
    crash) crash "$@" ;;
    crash-latest) latest_crash ;;
    reboot) network_ready || die 'reboot requires Vita Companion'; companion reboot ;;
    destroy) network_ready || die 'destroy requires Vita Companion'; companion destroy ;;
    help|-h|--help) usage ;;
    *) usage; die "unknown command: $command_name" ;;
esac
