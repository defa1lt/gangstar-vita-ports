#!/usr/bin/env python3
"""Copy Gangstar Rio runtime diagnostics whenever VitaShell mounts ux0."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import signal
import time
from datetime import datetime
from pathlib import Path


DEFAULT_VOLUME = Path("/Volumes/Untitled")
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "runtime_logs"


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def stable_snapshot(path: Path, delay: float = 0.2) -> tuple[int, int] | None:
    try:
        first = path.stat()
        time.sleep(delay)
        second = path.stat()
    except OSError:
        return None
    if (first.st_size, first.st_mtime_ns) != (second.st_size, second.st_mtime_ns):
        return None
    return second.st_size, second.st_mtime_ns


def copy_verified(source: Path, target: Path) -> dict[str, object] | None:
    snapshot = stable_snapshot(source)
    if snapshot is None:
        return None
    size, mtime_ns = snapshot
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(target.suffix + ".partial")
    try:
        shutil.copy2(source, temporary)
        source_hash = digest(source)
        target_hash = digest(temporary)
    except OSError:
        temporary.unlink(missing_ok=True)
        return None
    if source_hash != target_hash:
        temporary.unlink(missing_ok=True)
        return None
    temporary.replace(target)
    return {
        "source": str(source),
        "target": str(target),
        "size": size,
        "mtime_ns": mtime_ns,
        "sha256": target_hash,
    }


def load_state(path: Path) -> dict[str, int]:

    try:
        raw = json.loads(path.read_text())
        return {str(key): int(value) for key, value in raw.items()}
    except (OSError, ValueError, TypeError):
        return {}


def save_state(path: Path, state: dict[str, int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".partial")
    temporary.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def timestamp() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def scan(volume: Path, output: Path, state: dict[str, int]) -> bool:
    data = volume / "data"
    if not data.is_dir():
        return False

    copied = False
    log = data / "gangstarrio" / "loader.log"
    candidates: list[tuple[Path, Path]] = []
    if log.is_file():
        candidates.append((log, output / "latest-loader.log"))
    try:
        dumps = sorted(data.glob("psp2core-*.psp2dmp"))
    except OSError:
        dumps = []
    candidates.extend((item, output / "cores" / item.name) for item in dumps)

    for source, target in candidates:
        try:
            key = str(source.relative_to(volume))
            current_mtime = source.stat().st_mtime_ns
        except OSError:
            continue
        if state.get(key) == current_mtime and target.is_file():
            continue
        report = copy_verified(source, target)
        if report is None:
            continue
        state[key] = current_mtime
        save_state(output / ".usb-watch-state.json", state)
        print(
            f"[{timestamp()}] copied {source.name}: "
            f"{report['size']} bytes sha256={report['sha256']}",
            flush=True,
        )
        copied = True
    return copied


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--volume", type=Path, default=DEFAULT_VOLUME)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()

    state_path = args.output / ".usb-watch-state.json"
    state = load_state(state_path)
    running = True

    def stop(_signum: int, _frame: object) -> None:
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(
        f"[{timestamp()}] waiting for VitaShell USB at {args.volume}",
        flush=True,
    )
    while running:
        scan(args.volume, args.output, state)
        if args.once:
            break
        time.sleep(max(args.interval, 0.2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
