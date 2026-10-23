#!/usr/bin/env python3
"""Compare an Android ELF import set with a Vita loader's dynlib table."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from collections import Counter

from game_profile import ELF_PROFILE, RELOCATION_TYPES
from verify_game_files import verify_library


TABLE_ENTRY = re.compile(r'\{\s*"([^"\n]+)"\s*,')


def parse_undefined_symbols(readelf_output: str) -> set[str]:
    symbols: set[str] = set()
    for line in readelf_output.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[0].rstrip(":").isdigit() and parts[6] == "UND":
            symbols.add(parts[7].split("@", 1)[0])
    return symbols


def parse_mapping_names(source: str) -> set[str]:
    return set(TABLE_ENTRY.findall(source))


def find_readelf(explicit: str | None) -> str:
    if explicit:
        return explicit
    for name in ("llvm-readelf", "readelf"):
        path = shutil.which(name)
        if path:
            return path
    raise FileNotFoundError("llvm-readelf/readelf is not in PATH; pass --readelf")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--so", type=Path, required=True)
    parser.add_argument(
        "--dynlib",
        type=Path,
        action="append",
        default=None,
        help="mapping source; may be supplied more than once",
    )
    parser.add_argument("--readelf", help="readelf executable")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    try:
        verify_library(args.so.read_bytes(), "lib/armeabi-v7a/libGangstar4.so")
        readelf = find_readelf(args.readelf)
        completed = subprocess.run(
            [readelf, "--dyn-syms", "--wide", str(args.so)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        imports = parse_undefined_symbols(completed.stdout)
        dynlib_paths = args.dynlib or [
            root / "third_party/mc2-loader/source/dynlib.c",
        ]
        mappings: set[str] = set()
        for dynlib_path in dynlib_paths:
            mappings.update(parse_mapping_names(dynlib_path.read_text(encoding="utf-8")))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    symbols = {}
    jni_exports = 0
    for line in completed.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 7 and parts[0].rstrip(":").isdigit():
            name = parts[7] if len(parts) >= 8 else ""
            symbols[int(parts[0][:-1])] = (parts[6], name)
            if parts[6] != "UND" and (name.startswith("Java_") or name == "JNI_OnLoad"):
                jni_exports += 1
    rels = subprocess.check_output([readelf, "-r", "--wide", str(args.so)], text=True)
    kinds = Counter()
    imported_relocations = 0
    for line in rels.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2].startswith("R_ARM_"):
            kinds[parts[2]] += 1
            section, name = symbols[int(parts[1], 16) >> 8]
            imported_relocations += section == "UND" and bool(name)
    dynamic = subprocess.check_output([readelf, "-d", "--wide", str(args.so)], text=True)
    init_size = re.search(r"\(INIT_ARRAYSZ\)\s+(\d+)", dynamic)
    counts = {"symbols": len(symbols), "relocations": sum(kinds.values()),
              "imports": len(imports), "import_relocations": imported_relocations,
              "jni_exports": jni_exports, "constructors": int(init_size[1]) // 4 if init_size else 0}
    missing = sorted(imports - mappings)
    report = {
        "elf": str(args.so), "dynlib": [str(path) for path in dynlib_paths],
        "counts": counts, "relocation_types": dict(kinds),
        "profile_matches": counts == ELF_PROFILE and kinds == RELOCATION_TYPES,
        "mapped_names": len(imports & mappings), "missing_names": missing,
        "qualification": "Literal source-table coverage; addresses and ABI require Vita validation.",
    }
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"ELF profile matches: {report['profile_matches']}")
        print(f"mapped names: {report['mapped_names']} / {counts['imports']}")
        print("missing names: " + ", ".join(missing))
    if not report["profile_matches"]:
        return 1
    return 0 if not missing else 2


if __name__ == "__main__":
    raise SystemExit(main())
