#!/usr/bin/env python3
"""Prepare the pinned compatibility sources with Vegas ABI/path adaptations."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
base = root / 'third_party/mc2-loader/source'
args.out.mkdir(parents=True, exist_ok=True)
for name in ('reimpl/io.c', 'reimpl/pthr.c', 'reimpl/asset_manager.cpp', 'reimpl/egl.c', 'utils/glutil.c'):
    target = args.out / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(base / name, target)
for name in ('FalsoJNI.c', 'FalsoJNI_ImplBridge.c'):
    target = args.out / 'falso_jni' / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(base.parent / 'lib/falso_jni' / name, target)
subprocess.run(['patch', '--silent', '-p1', '-d', str(args.out), '-i',
                str(root / 'patches/mc2-vegas.patch')], check=True)
source = (base / 'dynlib.c').read_text()
start = source.index('so_default_dynlib default_dynlib[] = {')
end = source.index('\n};', start)
imports = set((root / 'tools/game_imports.txt').read_text().splitlines())
# Preserve preprocessing branches, but omit entries for unrelated game engines.
lines = []
for line in source[start:end].splitlines(keepends=True):
    entry = re.search(r'\{\s*"([^"]+)"\s*,', line)
    if not entry or entry[1] in imports:
        lines.append(line)
source = source[:start] + ''.join(lines) + source[end:]
(args.out / 'dynlib.c').write_text(source)
