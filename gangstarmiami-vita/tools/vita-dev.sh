#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
[[ ! -f .vita-dev.env ]] || source .vita-dev.env

titleid="${VITA_TITLEID:-PSVGMV002}"
vita_ip="${VITA_IP:-192.168.1.7}"
ftp_port="${VITA_FTP_PORT:-1337}"
cmd_port="${VITA_CMD_PORT:-1338}"
usb_mount="${VITA_USB_MOUNT:-/Volumes/Untitled}"
elf_path=build/gangstar_miami_vita
eboot_path=build/eboot.bin
vpk_path=build/Gangstar_Miami_Vita.vpk
remote_log=ux0:/data/gangstarmiami/loader.log
staging_data=staging/gangstarmiami
remote_data=ux0:/data/gangstarmiami

note() { printf '[%s] [miami-dev] %s\n' "$(date '+%H:%M:%S')" "$*"; }
die() { note "error: $*" >&2; exit 1; }

network_ready() {
  command -v nc >/dev/null 2>&1 && nc -z -w 2 "$vita_ip" "$cmd_port" >/dev/null 2>&1
}

companion() {
  printf '%s\n' "$*" | nc -w 4 "$vita_ip" "$cmd_port"
}

build_port() {
  ./scripts/build.sh
  [[ -f "$elf_path" && -f "$eboot_path" && -f "$vpk_path" ]] ||
    die 'build did not produce the expected artifacts'
}

archive_build() {
  local stamp out revision
  stamp="$(date '+%Y%m%d-%H%M%S')"
  out="debug/builds/$stamp"
  revision="$(git rev-parse --short HEAD)"
  mkdir -p "$out"
  cp "$elf_path" "$eboot_path" "$vpk_path" "$out/"
  printf 'timestamp=%s\nrevision=%s\ntitleid=%s\n' \
    "$stamp" "$revision" "$titleid" > "$out/build.txt"
  (cd "$out" && shasum -a 256 "$(basename "$elf_path")" \
    "$(basename "$eboot_path")" "$(basename "$vpk_path")" > SHA256SUMS)
  note "archived build: $out"
}

status() {
  printf 'project:   %s\n' "$project_dir"
  printf 'title id:  %s\n' "$titleid"
  printf 'network:   %s:%s %s\n' "$vita_ip" "$cmd_port" \
    "$(network_ready && echo ready || echo unavailable)"
  printf 'USB:       %s %s\n' "$usb_mount" \
    "$([[ -d "$usb_mount" ]] && echo mounted || echo unavailable)"
  printf 'VPK:       %s\n' "$([[ -f "$vpk_path" ]] && echo ready || echo missing)"
}

install_vpk() {
  build_port
  archive_build
  if network_ready; then
    curl --fail --silent --show-error --ftp-method nocwd --ftp-create-dirs \
      -T "$vpk_path" "ftp://$vita_ip:$ftp_port//ux0:/data/$(basename "$vpk_path")"
    note "uploaded installer to ux0:data/$(basename "$vpk_path")"
  elif [[ -d "$usb_mount/data" ]]; then
    cp "$vpk_path" "$usb_mount/data/"
    note "copied installer to $usb_mount/data/"
  else
    die 'Vita Companion and VitaShell USB are unavailable'
  fi
}

sync_data() {
  [[ -d "$staging_data/Gangstar2" ]] ||
    die "prepared data is missing; run ./scripts/prepare_data.sh ../../../gameloft/miami $staging_data"
  python3 tools/sync_miami_ftp.py \
    --host "$vita_ip" --port "$ftp_port" \
    --source "$staging_data/Gangstar2" \
    --destination "$remote_data/Gangstar2"
}

deploy() {
  build_port
  archive_build
  network_ready || die 'Vita Companion is unavailable'
  companion destroy >/dev/null
  curl --fail --silent --show-error --ftp-method nocwd --ftp-create-dirs \
    -T "$eboot_path" "ftp://$vita_ip:$ftp_port//ux0:/app/$titleid/eboot.bin"
  companion launch "$titleid"
  note 'eboot uploaded and launched'
}

pull_log() {
  local stamp out
  stamp="$(date '+%Y%m%d-%H%M%S')"
  out="debug/runs/$stamp"
  mkdir -p "$out"
  if network_ready; then
    curl --fail --silent --show-error --ftp-method nocwd \
      "ftp://$vita_ip:$ftp_port//$remote_log" -o "$out/loader.log"
  elif [[ -f "$usb_mount/data/gangstarmiami/loader.log" ]]; then
    cp "$usb_mount/data/gangstarmiami/loader.log" "$out/loader.log"
  else
    die 'Miami loader.log is unavailable'
  fi
  printf 'timestamp=%s\nrevision=%s\ntitleid=%s\n' \
    "$stamp" "$(git rev-parse --short HEAD)" "$titleid" > "$out/run.txt"
  note "saved log: $out/loader.log"
}

case "${1:-}" in
  status) status ;;
  build) build_port ;;
  archive) archive_build ;;
  install) install_vpk ;;
  sync-data) sync_data ;;
  deploy) deploy ;;
  pull) pull_log ;;
  *) echo "usage: $0 {status|build|archive|install|sync-data|deploy|pull}" >&2; exit 2 ;;
esac
