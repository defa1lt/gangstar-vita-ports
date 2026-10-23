#!/usr/bin/env python3
"""Reproducible host audit for the supported Gangstar Miami Android binary."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

EXPECTED_SHA256 = "ad52892fa12bcf00574e1d0f51df7642802ca648c2bdc2f79372f9aa478c5b21"
EXPECTED_SIZE = 9_793_266
EXPECTED_IMPORTS = 312
EXPECTED_JNI_EXPORTS = 24


def find_readelf(explicit: Path | None) -> str:
    candidates: list[Path | str] = []
    if explicit:
        candidates.append(explicit)
    vitasdk = os.environ.get("VITASDK")
    if vitasdk:
        candidates.append(Path(vitasdk) / "bin" / "arm-vita-eabi-readelf")
    candidates.append(Path(__file__).resolve().parents[1] / ".vitasdk-softfp" /
                      "bin" / "arm-vita-eabi-readelf")
    candidates.append(Path(__file__).resolve().parents[2] / ".vitasdk-softfp" /
                      "bin" / "arm-vita-eabi-readelf")
    candidates.append(Path(__file__).resolve().parents[3] / ".vitasdk-softfp" /
                      "bin" / "arm-vita-eabi-readelf")
    system = shutil.which("readelf")
    if system:
        candidates.append(system)
    for candidate in candidates:
        path = Path(candidate)
        if path.is_file() and os.access(path, os.X_OK):
            return str(path)
    raise SystemExit("readelf not found; pass --readelf or export VITASDK")


def read_symbols(readelf: str, so_path: Path) -> tuple[set[str], set[str]]:
    result = subprocess.run(
        [readelf, "-Ws", str(so_path)], check=True, capture_output=True,
        text=True,
    )
    imports: set[str] = set()
    jni_exports: set[str] = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].rstrip(":").isdigit():
            continue
        name = fields[7].split("@", 1)[0]
        if not name:
            continue
        if fields[6] == "UND":
            imports.add(name)
        elif name.startswith("Java_"):
            jni_exports.add(name)
    return imports, jni_exports


def resolver_symbols(path: Path) -> set[str]:
    table_entry = re.compile(r'\{\s*"([^"]+)"\s*,')
    return set(table_entry.findall(path.read_text(encoding="utf-8")))


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(chunk)
    return sha.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--so", required=True, type=Path,
                        help="Path to libGangster2.so from APK v3.1.6")
    parser.add_argument("--resolver", type=Path,
                        default=Path(__file__).resolve().parents[1] /
                                "source" / "dynlib.c")
    parser.add_argument("--readelf", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    if not args.so.is_file():
        parser.error(f"game library does not exist: {args.so}")
    if not args.resolver.is_file():
        parser.error(f"resolver source does not exist: {args.resolver}")

    readelf = find_readelf(args.readelf)
    imports, exports = read_symbols(readelf, args.so)
    resolver = resolver_symbols(args.resolver)
    missing = sorted(imports - resolver)
    profile = {
        "path": str(args.so),
        "size": args.so.stat().st_size,
        "sha256": digest(args.so),
        "unique_imports": len(imports),
        "jni_exports": len(exports),
        "resolver_direct_matches": len(imports & resolver),
        "resolver_missing": missing,
    }
    profile_ok = (
        profile["size"] == EXPECTED_SIZE
        and profile["sha256"] == EXPECTED_SHA256
        and profile["unique_imports"] == EXPECTED_IMPORTS
        and profile["jni_exports"] == EXPECTED_JNI_EXPORTS
    )
    profile["profile_ok"] = profile_ok

    if args.json:
        print(json.dumps(profile, indent=2, sort_keys=True))
    else:
        print("Gangstar Miami binary audit")
        print(f"  size:             {profile['size']} bytes")
        print(f"  sha256:           {profile['sha256']}")
        print(f"  unique imports:   {profile['unique_imports']}")
        print(f"  JNI exports:      {profile['jni_exports']}")
        print(f"  resolver coverage:{profile['resolver_direct_matches']}/{len(imports)}")
        print(f"  pinned profile:   {'OK' if profile_ok else 'MISMATCH'}")
        if missing:
            print("  missing direct resolver entries:")
            for symbol in missing:
                print(f"    {symbol}")

    return 0 if profile_ok and not missing else 1


if __name__ == "__main__":
    sys.exit(main())
