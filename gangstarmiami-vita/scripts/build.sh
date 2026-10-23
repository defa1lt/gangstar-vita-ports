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
source_root="${MIAMI_SOURCE_ROOT:-$project_dir/../../../gameloft/miami}"
game_so="$source_root/decompiled_miami/lib/armeabi/libGangster2.so"

if [[ -f "$game_so" ]]; then
  "$project_dir/tools/audit_game.py" --so "$game_so"
else
  echo "warning: host game binary is absent; skipping import/profile audit" >&2
fi

if [[ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]]; then
  echo "softfp VitaSDK is missing at $VITASDK" >&2
  exit 1
fi

if [[ ! -f "$project_dir/build/CMakeCache.txt" ]] ||
   ! grep -Fq "CMAKE_HOME_DIRECTORY:INTERNAL=$project_dir" \
     "$project_dir/build/CMakeCache.txt"; then
  "$project_dir/scripts/configure.sh" "${1:-Debug}"
fi

exec cmake --build "$project_dir/build" -j"${BUILD_JOBS:-4}"
