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
            root / "source/rio_missing_imports.c",
        ]
        mappings: set[str] = set()
        for dynlib_path in dynlib_paths:
            mappings.update(parse_mapping_names(dynlib_path.read_text(encoding="utf-8")))
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    missing = sorted(imports - mappings)
    report = {
        "elf": str(args.so),
        "dynlib": [str(path) for path in dynlib_paths],
        "imports": len(imports),
        "mapped_names": len(imports & mappings),
        "missing_names": missing,
    }
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"imports: {report['imports']}")
        print(f"mapped names: {report['mapped_names']}")
        print(f"missing names: {len(missing)}")
        for name in missing:
            print(f"  {name}")
    return 0 if not missing else 2


if __name__ == "__main__":
    raise SystemExit(main())
