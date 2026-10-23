#!/usr/bin/env python3
"""Verify the exact ARMv7 Gangstar Rio binary supported by this port."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path

from game_profile import APK_SO_PATH, SO_SHA256, SO_SIZE


class VerificationError(ValueError):
    """Input does not match the supported game profile."""


@dataclass(frozen=True)
class ElfIdentity:
    elf_class: int
    byte_order: int
    elf_type: int
    machine: int
    eabi_version: int


@dataclass(frozen=True)
class VerificationReport:
    source: str
    apk_member: str | None
    size: int
    sha256: str
    elf: ElfIdentity
    supported: bool


def parse_elf_identity(data: bytes) -> ElfIdentity:
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise VerificationError("native library is not a complete ELF32 file")
    elf_class = data[4]
    byte_order = data[5]
    if elf_class != 1:
        raise VerificationError(f"expected ELF32, got ELF class {elf_class}")
    if byte_order != 1:
        raise VerificationError("expected a little-endian ELF")

    elf_type, machine, flags = struct.unpack_from("<HH16xI", data, 16)
    return ElfIdentity(
        elf_class=elf_class,
        byte_order=byte_order,
        elf_type=elf_type,
        machine=machine,
        eabi_version=(flags >> 24) & 0xFF,
    )


def verify_bytes(data: bytes, source: str, apk_member: str | None = None) -> VerificationReport:
    identity = parse_elf_identity(data)
    digest = hashlib.sha256(data).hexdigest()
    problems: list[str] = []

    if len(data) != SO_SIZE:
        problems.append(f"size {len(data)} != {SO_SIZE}")
    if digest != SO_SHA256:
        problems.append(f"SHA-256 {digest} != {SO_SHA256}")
    if identity.elf_type != 3:
        problems.append(f"ELF type {identity.elf_type} is not ET_DYN")
    if identity.machine != 40:
        problems.append(f"ELF machine {identity.machine} is not ARM")
    if identity.eabi_version != 5:
        problems.append(f"EABI version {identity.eabi_version} is not 5")

    report = VerificationReport(
        source=source,
        apk_member=apk_member,
        size=len(data),
        sha256=digest,
        elf=identity,
        supported=not problems,
    )
    if problems:
        raise VerificationError("unsupported game binary: " + "; ".join(problems))
    return report


def read_so(path: Path) -> tuple[bytes, str | None]:
    if not path.is_file():
        raise VerificationError(f"file not found: {path}")
    return path.read_bytes(), None


def read_apk(path: Path) -> tuple[bytes, str]:
    if not path.is_file() or not zipfile.is_zipfile(path):
        raise VerificationError(f"not a readable APK: {path}")
    with zipfile.ZipFile(path) as archive:
        try:
            return archive.read(APK_SO_PATH), APK_SO_PATH
        except KeyError as error:
            raise VerificationError(
                f"APK has no {APK_SO_PATH}; an ARMv7 build is required"
            ) from error


def verify_path(path: Path, is_apk: bool) -> tuple[VerificationReport, bytes]:
    data, member = read_apk(path) if is_apk else read_so(path)
    return verify_bytes(data, str(path), member), data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--apk", type=Path, help="APK containing the ARMv7 library")
    source.add_argument("--so", type=Path, help="extracted libgl2jni.so")
    parser.add_argument("--json", action="store_true", help="emit a JSON report")
    args = parser.parse_args()

    path = args.apk if args.apk is not None else args.so
    try:
        report, _ = verify_path(path, args.apk is not None)
    except (OSError, VerificationError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(asdict(report), indent=2))
    else:
        print(f"supported: {report.source}")
        print(f"size: {report.size}")
        print(f"SHA-256: {report.sha256}")
        print("ELF: 32-bit little-endian ARM EABI5 shared object")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
