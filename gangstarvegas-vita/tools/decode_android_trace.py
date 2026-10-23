#!/usr/bin/env python3
"""Decode bounded ARM32 native call/return records produced by the analytics APK."""
import argparse
import bisect
from collections import Counter
import csv
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--csv', type=Path, help='optionally export every record, including raw ABI values')
    args = parser.parse_args()
    import lief
    image = lief.ELF.parse(str(args.library))
    symbols = {}
    for symbol in image.symbols:
        if symbol.value and symbol.name and str(symbol.type).endswith('FUNC'):
            address = symbol.value & ~1
            if address not in symbols or symbol.size > symbols[address][1]:
                symbols[address] = (symbol.name, symbol.size)
    addresses = sorted(symbols)
    events = [json.loads(line) for line in (args.directory / 'events.jsonl').read_text().splitlines()]
    game = next(event for event in events if event['event'] == 'game_module')
    if hashlib.sha256(bytes(image.get_section('.text').content)).hexdigest() != game['text_sha256']:
        raise ValueError('Native library does not match the captured .text SHA-256')
    end = next((event for event in reversed(events) if event['event'] == 'trace_end'), None)
    if end is None:
        raise ValueError('Capture has no trace_end marker')
    expected = {f"calls-{e['tid']}.bin": e['bytes'] for e in events if e['event'] == 'thread_end'}
    actual = {p.name: p.stat().st_size for p in args.directory.glob('calls-*.bin')}
    if actual != expected or sum(actual.values()) != end['bytes']:
        raise ValueError('Capture files are incomplete or contain stale thread traces')
    base = int(game['base'], 16)
    modules = [(int(e['base'], 16), e['size'], e['name']) for e in events if e['event'] == 'module_map']

    @lru_cache(maxsize=65536)
    def name(pointer):
        if base <= pointer < base + game['size']:
            offset = (pointer & ~1) - base
            i = bisect.bisect_right(addresses, offset) - 1
            if i >= 0:
                start = addresses[i]
                label, size = symbols[start]
                if size and offset < start + size:
                    return label, offset - start
            return 'game@' + hex(offset), 0
        for start, size, label in modules:
            if start <= pointer < start + size:
                return label + '@' + hex(pointer - start), 0
        return hex(pointer), 0

    counts, flags, graph, threads = Counter(), Counter(), Counter(), {}
    csv_file = args.csv.open('x', newline='') if args.csv else None
    writer = csv.writer(csv_file) if csv_file else None
    if writer:
        writer.writerow(['tid', 'index', 'event', 'from', 'from_offset', 'target', 'target_offset', 'depth',
                         'flags', 'r0', 'r1', 'r2', 'r3', 'sp', 'lr', 'monotonic_ms'] +
                        [f's{i}_raw' for i in range(16)] + [f'stack{i}_raw' for i in range(4)])
    try:
        for path in sorted(args.directory.glob('calls-*.bin')):
            tid = int(path.stem.split('-')[1])
            if path.stat().st_size % 128:
                raise ValueError('Partial native record: ' + str(path))
            index, local = 0, Counter()
            with path.open('rb') as stream:
                while record := stream.read(128):
                    words = struct.unpack('<32I', record)
                    kind = {1: 'call', 2: 'ret'}.get(words[0])
                    if kind is None:
                        raise ValueError('Unexpected record type: ' + str(words[0]))
                    source, delta = name(words[1])
                    target, target_delta = name(words[2])
                    local[kind] += 1
                    counts[kind] += 1
                    flags[str(words[4])] += 1
                    if kind == 'call':
                        graph[(source, target)] += 1
                    if writer:
                        depth = struct.unpack('<i', record[12:16])[0]
                        writer.writerow([tid, index, kind, source, hex(delta), target, hex(target_delta), depth,
                                         words[4]] + [hex(v) for v in words[5:11]] + [words[11]] +
                                        [hex(v) for v in words[12:]])
                    index += 1
            threads[str(tid)] = dict(local)
    finally:
        if csv_file:
            csv_file.close()
    summary = {'trace_end': end, 'counts': dict(counts), 'record_flags': dict(flags), 'threads': threads,
               'hooks': next((e for e in events if e['event'] == 'hooks_installed'), None),
               'unavailable_hooks': [e for e in events if e['event'] == 'hook_unavailable'],
               'top_calls': [{'from': source, 'target': target, 'count': count}
                             for (source, target), count in graph.most_common(100)],
               'limitations': 'Only executed machine calls/returns in recorded scopes and time window; raw ABI words are not typed objects. Inline code and unrecorded scopes are not inferred.'}
    (args.directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'trace_end': end, 'counts': dict(counts), 'threads': threads}, indent=2))


if __name__ == '__main__':
    main()
