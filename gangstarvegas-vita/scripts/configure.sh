#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace_dir=$(dirname "$project_dir")

# The port may live either directly in the shared Gangstar workspace or in its
# `gangstar/` source-data group. Locate the private SDK in both layouts instead
# of baking the repository depth into every build command.
if [ ! -d "$workspace_dir/.vitasdk-softfp" ]; then
    workspace_dir=$(dirname "$workspace_dir")
fi

export VITASDK="${VITASDK:-$workspace_dir/.vitasdk-softfp}"
default_host_lib="$workspace_dir/.vitasdk-host-x86_64/lib"
if [ -z "${VITA_HOST_X86_64_LIB:-}" ] && [ -d "$default_host_lib" ]; then
    export VITA_HOST_X86_64_LIB="$default_host_lib"
fi
export PATH="$VITASDK/bin:/opt/homebrew/bin:/usr/bin:/bin"

if [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
    echo "softfp VitaSDK is missing at $VITASDK" >&2
    exit 1
fi
build_type=${1:-Debug}

exec cmake -S "$project_dir" -B "$project_dir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_C_COMPILER="$project_dir/scripts/arm-vita-eabi-gcc" \
    -DCMAKE_CXX_COMPILER="$project_dir/scripts/arm-vita-eabi-g++" \
    -DCMAKE_ASM_COMPILER="$project_dir/scripts/arm-vita-eabi-gcc"
