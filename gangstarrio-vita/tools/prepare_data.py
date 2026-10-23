#!/usr/bin/env python3
"""Prepare Vita data from a user-owned Gangstar Rio APK and OBB files."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import sys
from dataclasses import asdict
from pathlib import Path

from game_profile import MAIN_OBB_NAME, PATCH_OBB_NAME, SO_NAME
from verify_game_files import VerificationError, verify_path


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def copy_file(source: Path, destination: Path) -> dict[str, object]:
    if not source.is_file():
        raise VerificationError(f"file not found: {source}")
    shutil.copyfile(source, destination)
    return {
        "source_name": source.name,
        "output": destination.name,
        "size": destination.stat().st_size,
        "sha256": hash_file(destination),
    }


def ensure_empty_output(path: Path) -> None:
    if path.exists():
        if not path.is_dir():
            raise VerificationError(f"output path is not a directory: {path}")
        if any(path.iterdir()):
            raise VerificationError(f"output directory is not empty: {path}")
    path.mkdir(parents=True, exist_ok=True)


def inspect_gameloft_obb(path: Path) -> dict[str, int]:
    file_size = path.stat().st_size
    with path.open("rb") as source:
        header = source.read(16)
    if len(header) != 16:
        raise VerificationError(f"OBB is too small: {path}")

    _unknown, data_offset, names_offset, entry_count = struct.unpack(
        ">IIII", header
    )
    expected_names_offset = 16 + entry_count * 16
    if names_offset != expected_names_offset:
        raise VerificationError(
            f"unsupported Gameloft OBB header in {path}: names offset "
            f"0x{names_offset:x}, expected 0x{expected_names_offset:x}"
        )
    if data_offset < names_offset or data_offset > file_size:
        raise VerificationError(
            f"invalid Gameloft OBB data offset 0x{data_offset:x}: {path}"
        )
    return {"entries": entry_count, "data_offset": data_offset}


def validate_obb(path: Path, expected_name: str) -> dict[str, int]:
    if not path.is_file():
        raise VerificationError(f"file not found: {path}")
    if path.name != expected_name:
        raise VerificationError(
            f"expected {expected_name}, got {path.name}; keep the original OBB name"
        )
    return inspect_gameloft_obb(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--main-obb", type=Path, required=True)
    parser.add_argument("--patch-obb", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    try:
        report, native_library = verify_path(args.apk, is_apk=True)
        main_obb = validate_obb(args.main_obb, MAIN_OBB_NAME)
        patch_obb = validate_obb(args.patch_obb, PATCH_OBB_NAME)
        ensure_empty_output(args.out)

        native_path = args.out / SO_NAME
        native_path.write_bytes(native_library)

        obb_path = args.out / "obb"
        obb_path.mkdir()
        files = [
            {
                "source_name": args.apk.name,
                "output": SO_NAME,
                "size": len(native_library),
                "sha256": report.sha256,
            },
            copy_file(args.main_obb, obb_path / args.main_obb.name),
            copy_file(args.patch_obb, obb_path / args.patch_obb.name),
        ]

        manifest = {
            "format": 1,
            "package": "com.gameloft.android.ANMP.GloftG4HM",
            "game_binary": asdict(report),
            "obb": {"main": main_obb, "patch": patch_obb},
            "files": files,
            "vita_destination": "ux0:data/gangstarrio/",
        }
        (args.out / "manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
    except (OSError, VerificationError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(f"prepared: {args.out.resolve()}")
    print("copy its contents to ux0:data/gangstarrio/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
