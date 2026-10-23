#!/usr/bin/env python3
"""Prepare a Russian text/font overlay from the 4PDA Miami fan translation."""

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct


SHEETS = ("about", "ch1", "ch2", "ch3", "ch4", "ch5", "chr", "chx",
          "global", "init", "menu", "tunes")
FONT_SHA256 = "558ab3fa8bb2443847cb13cb92bec2a37b17c2b019e8f64e24291b4d8fc1682a"


def read_sheet(path):
    data = path.read_bytes()
    count = struct.unpack_from("<H", data)[0]
    result, offset = [], 2
    for _ in range(count):
        length = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        value = data[offset:offset + length]
        if len(value) != length or b"\0" in value:
            raise ValueError(f"{path}: truncated or NUL-containing string")
        result.append(value.decode("utf-8", errors="strict"))
        offset += length
    if offset != len(data):
        raise ValueError(f"{path}: trailing bytes")
    return result


def encode_sheet(strings):
    result = bytearray(struct.pack("<H", len(strings)))
    for text in strings:
        data = text.encode("utf-8")
        result += struct.pack("<H", len(data)) + data
    return bytes(result)


def expanded_size(text):
    # StringManager::loadPackSheet uses two fixed 0x400-byte buffers.
    out, index = [], 0
    while index < len(text):
        char = text[index]
        index += 1
        if char == "\\" and index < len(text):
            escape = text[index]
            index += 1
            out.append({"0": '<font color="#FFFF33">', "1": "</font>",
                        "\\": "\\"}.get(escape, ""))
        elif char == " " and index < len(text) and text[index] in "?!;":
            out.append("&nbsp;")
        else:
            out.append(char)
    return len("".join(out).encode("utf-8"))


def font_characters(data):
    """Read the supported Gunplay font's Unicode BMP cmap (SFNT format 4)."""
    count = struct.unpack_from(">H", data, 4)[0]
    for index in range(count):
        tag, _, offset, _ = struct.unpack_from(">4sIII", data, 12 + index * 16)
        if tag == b"cmap":
            cmap = offset
            break
    else:
        raise ValueError("font has no cmap")
    records = struct.unpack_from(">H", data, cmap + 2)[0]
    for index in range(records):
        platform, encoding, offset = struct.unpack_from(">HHI", data,
                                                        cmap + 4 + index * 8)
        table = cmap + offset
        if platform == 3 and encoding == 1 and struct.unpack_from(">H", data, table)[0] == 4:
            segments = struct.unpack_from(">H", data, table + 6)[0] // 2
            ends = table + 14
            starts = ends + segments * 2 + 2
            deltas = starts + segments * 2
            ranges = deltas + segments * 2
            characters = set()
            for segment in range(segments):
                end = struct.unpack_from(">H", data, ends + segment * 2)[0]
                start = struct.unpack_from(">H", data, starts + segment * 2)[0]
                delta = struct.unpack_from(">h", data, deltas + segment * 2)[0]
                address = ranges + segment * 2
                distance = struct.unpack_from(">H", data, address)[0]
                for code in range(start, min(end, 0xFFFE) + 1):
                    glyph = struct.unpack_from(">H", data, address + distance + (code - start) * 2)[0] if distance else code
                    if glyph and (glyph + delta) & 0xFFFF:
                        characters.add(chr(code))
            return characters
    raise ValueError("font has no supported Unicode BMP cmap")


def prepare(base, donor):
    font = (donor / "font_latin.ttf").read_bytes()
    if hashlib.sha256(font).hexdigest() != FONT_SHA256:
        raise ValueError("unsupported donor font: expected the Cyrillic Gunplay fan font")
    output = {"font_russian.ttf": font}
    all_characters, counts, largest = set(), {}, 0
    for sheet in SHEETS:
        original = read_sheet(base / f"{sheet}.english")
        donor_english = read_sheet(donor / f"{sheet}.english")
        translated = read_sheet(donor / f"{sheet}.german")
        categories = read_sheet(base / f"{sheet}.type")
        if sheet == "about":
            if len(translated) != 419 or len(categories) != len(original):
                raise ValueError("unsupported credits layout")
            # Fan credits are inserted near the start; all later rows use 'about'.
            categories = [" "] * 6 + ["about"] * (len(translated) - 6)
        else:
            if not len(translated) == len(original) == len(categories):
                raise ValueError(f"{sheet}: string IDs/counts do not match the base")
            if sheet not in ("menu", "init") and donor_english != original:
                raise ValueError(f"{sheet}: donor story differs from the supported base")
            if any(a.count("%v") != b.count("%v")
                   for a, b in zip(original, translated)):
                raise ValueError(f"{sheet}: variable placeholders changed")
        if sheet == "menu":
            translated[6:14] = ["Русский", *original[7:14]]
            translated[22] = original[22]  # Preserve the native More Games action.
        glyphs = defaultdict(set)
        for index, (text, category) in enumerate(zip(translated, categories)):
            size = max(len(text.encode("utf-8")), expanded_size(text))
            if size > 1023:
                raise ValueError(f"{sheet}[{index}]: exceeds native string buffer ({size})")
            largest = max(largest, size)
            visible = set(re.sub(r"\\[01n]|[\n\r\t|]", "", text))
            glyphs[""] |= visible
            if category != " ":
                glyphs[category] |= visible
            all_characters |= visible
        output[f"{sheet}.russian"] = encode_sheet(translated)
        for category, characters in glyphs.items():
            suffix = f".{category}" if category else ""
            output[f"{sheet}.russian{suffix}.glyph"] = "".join(sorted(characters)).encode("utf-8")
        counts[sheet] = len(translated)
    missing = all_characters - font_characters(font)
    if missing:
        raise ValueError(f"font lacks characters: {sorted(missing)}")
    return output, {"strings": counts, "largest_native_string_bytes": largest,
                    "font_sha256": FONT_SHA256}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path, help="Original supported Gangstar2 cache")
    parser.add_argument("donor", type=Path, help="Extracted fan text/font directory")
    parser.add_argument("output", type=Path, help="Overlay destination (Gangstar2)")
    args = parser.parse_args()
    output, report = prepare(args.base, args.donor)
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in output.items():
        (args.output / name).write_bytes(data)
    report["files"] = len(output)
    report["bytes"] = sum(map(len, output.values()))
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
