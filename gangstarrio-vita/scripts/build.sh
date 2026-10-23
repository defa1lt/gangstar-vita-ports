#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace_dir=$(dirname "$project_dir")
if [ ! -d "$workspace_dir/.vitasdk-softfp" ]; then
    workspace_dir=$(dirname "$workspace_dir")
fi

export VITASDK="${VITASDK:-$workspace_dir/.vitasdk-softfp}"
default_host_lib="$workspace_dir/.vitasdk-host-x86_64/lib"
if [ -z "${VITA_HOST_X86_64_LIB:-}" ] && [ -d "$default_host_lib" ]; then
    export VITA_HOST_X86_64_LIB="$default_host_lib"
fi
export PATH="$VITASDK/bin:/opt/homebrew/bin:/usr/bin:/bin"

if [ ! -f "$project_dir/build/build.ninja" ]; then
    "$project_dir/scripts/configure.sh" "${1:-Debug}"
fi

exec cmake --build "$project_dir/build"
