#!/usr/bin/env python3
"""Verify both ARMv7 libraries against the supplied Vegas reference profile."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zipfile

from game_profile import APK_SHA256, LIBRARIES


def verify_library(data, name):
    expected = LIBRARIES[name]
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        raise ValueError(f"{name}: expected little-endian ELF32")
    elf_type, machine = struct.unpack_from("<HH", data, 16)
    eabi = struct.unpack_from("<I", data, 36)[0] >> 24
    if (elf_type, machine, eabi) != (3, 40, 5):
        raise ValueError(f"{name}: expected ARM EABI5 shared object")
    actual = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    if actual != expected:
        raise ValueError(f"{name}: does not match the reference profile: {actual}")
    return {"path": name, **actual, "elf": "ELF32 ARM EABI5"}


def verify_apk(path):
    with path.open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != APK_SHA256:
            raise ValueError(f"{path}: APK does not match the Google Play reference SHA-256")
    with zipfile.ZipFile(path) as archive:
        return [verify_library(archive.read(name), name) for name in LIBRARIES]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = verify_apk(args.apk)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
