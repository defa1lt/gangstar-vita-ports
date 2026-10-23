#!/usr/bin/env python3
"""Read only Vegas progress files directly over ADB/Frida into a fresh local backup."""
import argparse
import hashlib
import json
import multiprocessing
from pathlib import Path
import subprocess
import time

SCRIPT = r'''
const names = ['profile0.gs4', 'backupSaveFile.dat'];
rpc.exports = {
  readsave(name) {
    if (!names.includes(name)) throw new Error('Unsupported save name');
    const file = new File('/data/data/com.gameloft.android.ANMP.GloftGGHM/files/' + name, 'rb');
    try {
      file.seek(0, File.SEEK_END);
      const size = file.tell();
      if (size <= 0 || size > 1048576) throw new Error('Save size outside 1..1048576 bytes');
      file.seek(0, File.SEEK_SET);
      const data = file.readBytes(size);
      if (data.byteLength !== size) throw new Error('Short save read');
      return data;
    } finally { file.close(); }
  }
};
'''


def read_saves(output, serial, port):
    import frida
    output = Path(output)
    device = frida.get_device_manager().add_remote_device('127.0.0.1:' + port)
    session = script = None
    try:
        deadline = time.monotonic() + 30
        while True:
            try:
                session = device.attach('Gadget')
                break
            except (frida.TransportError, frida.ServerNotRunningError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(1)
        script = session.create_script(SCRIPT)
        script.load()
        manifest = {'serial': serial, 'files': [], 'transport': 'ADB loopback; no Android shared-storage copy'}
        for name in ('profile0.gs4', 'backupSaveFile.dat'):
            data = script.exports_sync.readsave(name)
            second = script.exports_sync.readsave(name)
            if not isinstance(data, bytes) or data != second:
                raise ValueError('Save changed during export: ' + name)
            path = output / name
            with path.open('xb') as target:
                target.write(data)
            if path.read_bytes() != data:
                raise ValueError('Local readback mismatch: ' + name)
            manifest['files'].append({'name': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(json.dumps(manifest, indent=2), flush=True)
    finally:
        try:
            if script:
                script.unload()
        finally:
            if session:
                session.detach()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)

    def adb(*command):
        return subprocess.check_output([args.adb, '-s', args.serial, *command], timeout=15).decode().strip()

    port = adb('forward', 'tcp:0', 'tcp:27043')
    worker = None
    try:
        worker = multiprocessing.get_context('spawn').Process(target=read_saves, args=(str(args.output), args.serial, port))
        worker.start()
        worker.join(60)
        if worker.is_alive():
            raise TimeoutError('Save export exceeded 60 seconds')
        if worker.exitcode != 0:
            raise RuntimeError('Save export failed; partial files preserved')
    finally:
        try:
            if worker is not None and worker.pid is not None:
                if worker.is_alive():
                    worker.terminate()
                    worker.join(5)
                if worker.is_alive():
                    worker.kill()
                    worker.join(5)
        finally:
            adb('forward', '--remove', 'tcp:' + port)


if __name__ == '__main__':
    main()
