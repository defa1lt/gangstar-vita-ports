#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 /path/to/miami/source /path/to/staging-root [russian-donor-directory]" >&2
  exit 2
fi

source_root=$1
staging_root=$2
game_so="$source_root/decompiled_miami/lib/armeabi/libGangster2.so"
cache_root="$source_root/Gangstar2"
expected_sha=ad52892fa12bcf00574e1d0f51df7642802ca648c2bdc2f79372f9aa478c5b21

if [[ ! -f "$game_so" || ! -d "$cache_root" ]]; then
  echo "source must contain decompiled_miami/lib/armeabi/libGangster2.so and Gangstar2/" >&2
  exit 1
fi

required_resources=(
  PhonePerformanceProfiles.gmap
  DefaultEffects.bdae
  Res.array
)
for resource in "${required_resources[@]}"; do
  if [[ ! -f "$cache_root/$resource" ]]; then
    echo "incomplete Gangstar2 cache: missing $resource" >&2
    exit 1
  fi
done

actual_sha=$(shasum -a 256 "$game_so" | awk '{print $1}')
if [[ "$actual_sha" != "$expected_sha" ]]; then
  echo "unsupported libGangster2.so: $actual_sha" >&2
  exit 1
fi

mkdir -p "$staging_root/Gangstar2"
rsync -a --delete "$cache_root/" "$staging_root/Gangstar2/"
cp -p "$game_so" "$staging_root/libGangster2.so"
if [[ $# -eq 3 ]]; then
  python3 "$(dirname "$0")/../tools/prepare_russian.py" \
    "$cache_root" "$3" "$staging_root/Gangstar2"
fi

echo "Prepared Vita layout at $staging_root"
echo "Install its contents under ux0:data/gangstarmiami/"
