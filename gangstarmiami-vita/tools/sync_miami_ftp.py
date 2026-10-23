#!/usr/bin/env python3
"""Resume the Miami loose-cache upload over VitaShell's fragile FTP server."""

from __future__ import annotations

import argparse
import ftplib
import os
from pathlib import Path, PurePosixPath
import sys
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=os.environ.get("VITA_IP", "192.168.1.7"))
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path(__file__).resolve().parents[1]
        / "staging/gangstarmiami/Gangstar2",
    )
    parser.add_argument(
        "--destination", default="ux0:/data/gangstarmiami/Gangstar2"
    )
    parser.add_argument("--retries", type=int, default=10)
    return parser.parse_args()


def connect(host: str, port: int, timeout: int = 30) -> ftplib.FTP:
    ftp = ftplib.FTP()
    ftp.encoding = "utf-8"
    ftp.connect(host, port, timeout=timeout)
    ftp.login()
    ftp.set_pasv(True)
    return ftp


def ensure_directory(ftp: ftplib.FTP, path: str) -> None:
    current = ""
    for component in PurePosixPath(path).parts:
        current = component if not current else f"{current}/{component}"
        try:
            ftp.mkd(current)
        except ftplib.error_perm as error:
            if not str(error).startswith("550"):
                raise


def remote_names(ftp: ftplib.FTP, directory: str) -> set[str]:
    try:
        names = ftp.nlst(directory)
    except ftplib.error_perm as error:
        if str(error).startswith("550"):
            return set()
        raise
    return {PurePosixPath(name.rstrip("/")).name for name in names}


def inventory(ftp: ftplib.FTP, source: Path, destination: str) -> dict[str, int]:
    result: dict[str, int] = {}
    directories = [source, *(path for path in source.rglob("*") if path.is_dir())]
    for local_directory in directories:
        relative = local_directory.relative_to(source).as_posix()
        remote_directory = destination if relative == "." else f"{destination}/{relative}"
        ensure_directory(ftp, remote_directory)
        names = remote_names(ftp, remote_directory)
        for local_file in local_directory.iterdir():
            if not local_file.is_file() or local_file.name not in names:
                continue
            remote_file = f"{remote_directory}/{local_file.name}"
            try:
                size = ftp.size(remote_file)
            except ftplib.all_errors:
                continue
            if size is not None:
                result[local_file.relative_to(source).as_posix()] = size
    return result


def main() -> int:
    args = parse_args()
    source = args.source.resolve()
    if not source.is_dir():
        print(f"error: source directory does not exist: {source}", file=sys.stderr)
        return 2

    files = sorted(path for path in source.rglob("*") if path.is_file())
    print(f"source: {len(files)} files from {source}", flush=True)

    ftp = connect(args.host, args.port)
    try:
        remote = inventory(ftp, source, args.destination)
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            ftp.close()

    pending = [
        path
        for path in files
        if remote.get(path.relative_to(source).as_posix()) != path.stat().st_size
    ]
    pending_bytes = sum(path.stat().st_size for path in pending)
    print(
        f"verified: {len(files) - len(pending)}/{len(files)} files; "
        f"pending: {len(pending)} files, {pending_bytes / (1024 * 1024):.1f} MiB",
        flush=True,
    )

    uploaded_bytes = 0
    ftp = connect(args.host, args.port)
    try:
        for index, local_file in enumerate(pending, 1):
            relative = local_file.relative_to(source).as_posix()
            remote_file = f"{args.destination}/{relative}"
            attempt = 0
            while True:
                try:
                    with local_file.open("rb") as stream:
                        ftp.storbinary(f"STOR {remote_file}", stream, blocksize=128 * 1024)
                    remote_size = ftp.size(remote_file)
                    if remote_size != local_file.stat().st_size:
                        raise OSError(
                            f"size mismatch for {relative}: {remote_size} != "
                            f"{local_file.stat().st_size}"
                        )
                    break
                except ftplib.all_errors + (OSError,) as error:
                    attempt += 1
                    if attempt > args.retries:
                        raise RuntimeError(
                            f"upload failed after {args.retries} retries: {relative}"
                        ) from error
                    print(
                        f"retry {attempt}/{args.retries}: {relative}: {error}",
                        flush=True,
                    )
                    try:
                        ftp.close()
                    except OSError:
                        pass
                    time.sleep(min(attempt, 5))
                    ftp = connect(args.host, args.port)

            uploaded_bytes += local_file.stat().st_size
            if index == 1 or index % 50 == 0 or index == len(pending):
                print(
                    f"progress: {index}/{len(pending)} files, "
                    f"{uploaded_bytes / (1024 * 1024):.1f}/{pending_bytes / (1024 * 1024):.1f} MiB",
                    flush=True,
                )
    finally:
        try:
            ftp.quit()
        except ftplib.all_errors:
            ftp.close()

    print("Miami cache sync complete and size-verified", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
