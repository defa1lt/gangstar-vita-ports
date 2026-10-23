#!/usr/bin/env python3
"""Build a patched copy of the vendored VitaGL sources."""
import argparse
import json
from pathlib import Path
import subprocess
import shutil
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
base = root / 'third_party/vitaGL'
pinned = (base / '.pinned-commit').read_text().strip()
args.out.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='vitagl-', dir=args.out.parent) as temporary:
    staging = Path(temporary)
    shutil.copyfile(base / 'Makefile', staging / 'Makefile')
    shutil.copytree(base / 'source', staging / 'source',
                    ignore=shutil.ignore_patterns('*.o', '*.a', '__pycache__'))
    subprocess.run(['patch', '--silent', '-p1', '-d', str(staging), '-i',
                    str(root / 'patches/vitagl-pixel-store.patch')], check=True)
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = args.out / '.source-manifest.json'
    previous = set(json.loads(manifest.read_text())) if manifest.exists() else set()
    current = set()
    for source in staging.rglob('*'):
        if not source.is_file():
            continue
        relative = source.relative_to(staging)
        current.add(str(relative))
        target = args.out / relative
        content = source.read_bytes()
        if not target.exists() or target.read_bytes() != content:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
    for relative in previous - current:
        path = Path(relative)
        if path.is_absolute() or '..' in path.parts:
            raise ValueError('Invalid generated source manifest')
        (args.out / path).unlink(missing_ok=True)
    manifest.write_text(json.dumps(sorted(current)) + '\n')
    (args.out / '.pinned-commit').write_text(pinned + '\n')
