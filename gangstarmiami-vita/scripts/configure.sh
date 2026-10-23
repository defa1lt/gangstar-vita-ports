#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
workspace_dir="$(dirname "$project_dir")"
if [[ ! -d "$workspace_dir/.vitasdk-softfp" ]]; then
  workspace_dir="$(dirname "$workspace_dir")"
fi
export VITASDK="${VITASDK:-$workspace_dir/.vitasdk-softfp}"
export PATH="$VITASDK/bin:/opt/homebrew/bin:/usr/bin:/bin"
host_lib="$(dirname "$VITASDK")/.vitasdk-host-x86_64/lib"
if [[ -d "$host_lib" ]]; then
  export DYLD_LIBRARY_PATH="$host_lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
fi

if [[ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]]; then
  echo "softfp VitaSDK is missing at $VITASDK" >&2
  exit 1
fi

exec cmake -S "$project_dir" -B "$project_dir/build" \
  -DCMAKE_BUILD_TYPE="${1:-Debug}" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
