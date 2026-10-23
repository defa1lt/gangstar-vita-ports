#!/usr/bin/env python3
"""Inventory candidate Java contracts using the verified ELF, smali and expanded C table."""
from collections import defaultdict
import argparse
import json
import re
from pathlib import Path
from verify_game_files import verify_library

RETURN_TYPES = dict(V='VOID', Z='BOOLEAN', B='BYTE', C='CHAR', S='SHORT', I='INT',
                    J='LONG', F='FLOAT', D='DOUBLE', L='OBJECT', **{'[': 'OBJECT'})
ENTRY = re.compile(r'\{([^,]+),\s*((?:"[^"\n]*"\s*)+),\s*METHOD_TYPE_(\w+)\}')
REQUEST = re.compile(r'GetStaticMethodID\(env, [^,]+, "([^"]+)", "([^"]+)"\)')


def audit(so, smali_root, table_source, log=''):
    verify_library(so, 'lib/armeabi-v7a/libGangstar4.so')
    table = table_source.split('NameToMethodID nameToMethodId[] = {', 1)[1].split('\n};', 1)[0]
    bindings = {''.join(re.findall(r'"([^"\n]*)"', literals)): (identifier.strip(), kind)
                for identifier, literals, kind in ENTRY.findall(table)}
    strings = {value.decode('ascii') for value in re.findall(rb'[ -~]{3,}', so)}
    exports = {value.decode('ascii') for value in re.findall(rb'Java_[A-Za-z0-9_]+', so)}
    observed = set(REQUEST.findall(log))
    paths = set((smali_root / 'com/gameloft/android/ANMP/GloftGGHM').glob('*.smali'))
    for export in exports:
        pieces = export[5:].split('_')
        for length in range(1, len(pieces)):
            path = smali_root / ('/'.join(pieces[:length]) + '.smali')
            if path.is_file():
                paths.add(path)
    rows, matched, mismatches = [], set(), []
    for path in sorted(paths):
        text = path.read_text()
        declaration = re.search(r'^\.class[^\n]* L([^;]+);', text, re.M)
        if not declaration:
            continue
        clazz = declaration[1]
        native_class = any(name.startswith('Java_' + clazz.replace('/', '_') + '_') for name in exports)
        for match in re.finditer(r'^\.method\s+([^\n]+)', text, re.M):
            method = match[1]
            if ' static ' not in ' ' + method or ' native ' in ' ' + method or '(' not in method:
                continue
            name = method.split()[-1].split('(', 1)[0]
            signature = method[method.index('('):]
            pair = name, signature
            if pair not in observed and not (native_class and name in strings and signature in strings):
                continue
            if pair in observed:
                matched.add(pair)
            binding_key = next((key for key in (clazz + '/' + name + signature,
                               clazz + '/' + name, name) if key in bindings), None)
            binding = bindings.get(binding_key)
            kind = RETURN_TYPES[signature.split(')', 1)[1][0]]
            row = dict(clazz=clazz, name=name, signature=signature,
                       smali=str(path) + ':' + str(text.count('\n', 0, match.start()) + 1),
                       observed_in_log=pair in observed, binding=binding, binding_key=binding_key, return_type=kind)
            rows.append(row)
            if binding and binding[1] != kind:
                mismatches.append(row)
    groups = defaultdict(list)
    for row in rows:
        if row['binding'] and '(' not in row['binding_key']:
            groups[row['binding_key']].append(row)
    ambiguous = [dict(binding=key, contracts=group) for key, group in groups.items()
                 if len({row['signature'] for row in group}) > 1]
    return dict(qualification='Conservative string/export candidates, not proven callsites or lifecycle reachability. '
                'Error-level logs omit successful lookups; observed name/signature pairs do not identify classes. '
                'Bare-name bindings do not validate argument arity/types. Instance methods and behavior need separate verification.',
                bindings=len(bindings), candidates=len(rows),
                covered_candidates=sum(row['binding'] is not None for row in rows),
                type_mismatches=mismatches, ambiguous_bindings=ambiguous,
                observed_unresolved_declarations=[dict(name=n, signature=s) for n, s in sorted(observed - matched)],
                methods=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--so', type=Path, required=True)
    parser.add_argument('--smali', type=Path, required=True)
    parser.add_argument('--table', type=Path, required=True, help='C-preprocessed source/java.c')
    parser.add_argument('--log', type=Path)
    args = parser.parse_args()
    report = audit(args.so.read_bytes(), args.smali, args.table.read_text(),
                   args.log.read_text() if args.log else '')
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 2 if report['type_mismatches'] or report['ambiguous_bindings'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
