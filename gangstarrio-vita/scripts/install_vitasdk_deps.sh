#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace_dir=$(dirname "$project_dir")
if [ ! -d "$workspace_dir/.vitasdk-softfp" ]; then
    workspace_dir=$(dirname "$workspace_dir")
fi
export VITASDK="${VITASDK:-$workspace_dir/.vitasdk-softfp}"

if [ ! -x "$VITASDK/bin/vdpm" ]; then
    echo "softfp vdpm is missing at $VITASDK/bin/vdpm" >&2
    exit 1
fi

exec "$VITASDK/bin/vdpm" \
    zlib opensles libsndfile lame mpg123 opus libvorbis libogg flac \
    libmathneon vitaGL vitaShaRK SceShaccCgExt kubridge taihen
