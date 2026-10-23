#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 "$project_dir/tools/vita_usb_watch.py" "$@"
