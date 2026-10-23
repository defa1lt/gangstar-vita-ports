#!/usr/bin/env python3
"""Extract local APK/OBB references and nested ZIPs for port investigation."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import zipfile

from game_profile import MAIN_OBB_NAME, PATCH_OBB_NAME, PACKAGE, LIBRARIES, OBBS, APK_SHA256
from verify_game_files import verify_apk


def hash_file(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def extract_archive(source, destination, root, inventories, nested=False):
    destination.mkdir(parents=True, exist_ok=False)
    files = []
    children = []
    print(f"extracting: {source.name} -> {destination.relative_to(root)}", flush=True)
    with zipfile.ZipFile(source) as archive:
        for entry in archive.infolist():
            path = PurePosixPath(entry.filename)
            if (path.is_absolute() or ".." in path.parts or "\\" in entry.filename
                    or stat.S_ISLNK(entry.external_attr >> 16)):
                raise ValueError(f"unsafe archive member: {entry.filename}")
            target = destination.joinpath(*path.parts)
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as reader, target.open("xb") as writer:
                shutil.copyfileobj(reader, writer, length=1024 * 1024)
            files.append({"path": entry.filename, "size": entry.file_size,
                          "crc32": f"{entry.CRC:08x}"})
            if nested and target.suffix.lower() == ".zip":
                children.append(target)
    inventories.append({"source": str(source.relative_to(root)) if source.is_relative_to(root)
                        else source.name, "output": str(destination.relative_to(root)),
                        "files": files})
    for child in children:
        extract_archive(child, child.with_name(child.name + ".unpacked"), root,
                        inventories, nested=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--mode", choices=["reference", "vita"], default="reference")
    parser.add_argument("--main-obb", type=Path)
    parser.add_argument("--patch-obb", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        libraries = verify_apk(args.apk)
        if args.main_obb is None or args.patch_obb is None:
            raise ValueError("requires --main-obb and --patch-obb")
        for path, expected in [(args.main_obb, MAIN_OBB_NAME),
                               (args.patch_obb, PATCH_OBB_NAME)]:
            if path.name != expected or not zipfile.is_zipfile(path):
                raise ValueError(f"expected ZIP-format {expected}, got {path}")
            if path.stat().st_size != OBBS[expected]["size"] or hash_file(path) != OBBS[expected]["sha256"]:
                raise ValueError(f"OBB does not match the reference profile: {path}")
        if args.mode == "vita":
            args.out.mkdir(parents=True, exist_ok=True)
            files = []
            with zipfile.ZipFile(args.apk) as archive:
                for name in LIBRARIES:
                    target = args.out / Path(name).name
                    data = archive.read(name)
                    if target.exists():
                        if hash_file(target) != LIBRARIES[name]["sha256"]:
                            raise ValueError(f"existing different file preserved: {target}")
                    else:
                        target.write_bytes(data)
                    digest = hash_file(target)
                    if digest != LIBRARIES[name]["sha256"]:
                        raise ValueError(f"output hash mismatch: {target}")
                    files.append({"path": target.name, "size": target.stat().st_size, "sha256": digest})
                for entry in archive.infolist():
                    path = PurePosixPath(entry.filename)
                    if path.parts[0] not in ("assets", "res") or entry.is_dir():
                        continue
                    if path.is_absolute() or ".." in path.parts or "\\" in entry.filename or stat.S_ISLNK(entry.external_attr >> 16):
                        raise ValueError(f"unsafe archive member: {entry.filename}")
                    target = args.out.joinpath(*path.parts)
                    data = archive.read(entry)
                    digest = hashlib.sha256(data).hexdigest()
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if target.exists():
                        if hash_file(target) != digest:
                            raise ValueError(f"existing different file preserved: {target}")
                    else:
                        target.write_bytes(data)
                    files.append({"path": entry.filename, "size": entry.file_size, "sha256": digest})
            for source, name, expected_hash in [(args.apk, "game.apk", APK_SHA256),
                    (args.main_obb, MAIN_OBB_NAME, OBBS[MAIN_OBB_NAME]["sha256"]),
                    (args.patch_obb, PATCH_OBB_NAME, OBBS[PATCH_OBB_NAME]["sha256"])]:
                target = args.out / name
                print(f"copying: {source.name} -> {name}", flush=True)
                if target.exists():
                    if hash_file(target) != expected_hash:
                        raise ValueError(f"existing different file preserved: {target}")
                else:
                    shutil.copyfile(source, target)
                digest = hash_file(target)
                if digest != expected_hash:
                    raise ValueError(f"output hash mismatch: {target}")
                files.append({"path": name, "size": target.stat().st_size, "sha256": digest})
            manifest = {"format": 1, "package": PACKAGE, "stage": "Original APK/OBB and loose APK resources for native/Java asset access",
                        "vita_destination": "ux0:data/gangstarvegas/", "files": files}
            (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            print(f"prepared Vita data: {args.out.resolve()}")
            return 0
        if args.out.exists():
            raise ValueError(f"output already exists; choose a new directory: {args.out}")
        args.out.mkdir(parents=True)
        inventories = []
        sources = []
        for path, output, nested in [(args.apk, "apk", False),
                                     (args.main_obb, "cache/main", True),
                                     (args.patch_obb, "cache/patch", True)]:
            print(f"hashing: {path.name}", flush=True)
            sources.append({"name": path.name, "size": path.stat().st_size,
                            "sha256": hash_file(path)})
            extract_archive(path, args.out / output, args.out, inventories, nested)
        manifest = {"format": 1, "package": PACKAGE, "libraries": libraries,
                    "sources": sources, "archives": inventories}
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"prepared: {args.out.resolve()}")
    print(f"archives: {len(inventories)}, files: {sum(len(a['files']) for a in inventories)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
