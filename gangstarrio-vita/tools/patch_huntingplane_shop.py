#!/usr/bin/env python3
"""Patch the hidden Floating Point boat and Huntingplane into the vehicle shop."""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path


HUNTINGPLANE_ID = 281
FLOATING_POINT_ID = 54
SEAPLANE_ID = 409
HUNTINGPLANE_NAME_ID = 12368  # global string sheet 3, entry 80: Yellow Baron
HUNTINGPLANE_DESCRIPTION_ID = 12399  # global string sheet 3, entry 111
GLOBAL_STRING_SHEET = 3
FLOATING_POINT_NAME_INDEX = 653
FLOATING_POINT_DESCRIPTION_INDEX = 654
FLOATING_POINT_NAME_ID = GLOBAL_STRING_SHEET * 4096 + FLOATING_POINT_NAME_INDEX
FLOATING_POINT_DESCRIPTION_ID = GLOBAL_STRING_SHEET * 4096 + FLOATING_POINT_DESCRIPTION_INDEX
VEHICLE_SHOP_NAME = "IGM_VehicleShop"
HUNTINGPLANE_PRICE = 50_000
FLOATING_POINT_PRICE = 15_000


class FormatError(ValueError):
    pass


class Reader:
    def __init__(self, data: bytes, offset: int = 0):
        self.data = data
        self.offset = offset

    def take(self, length: int) -> bytes:
        if length < 0 or self.offset + length > len(self.data):
            raise FormatError(f"read past end at 0x{self.offset:x} (+{length})")
        result = self.data[self.offset:self.offset + length]
        self.offset += length
        return result

    def int(self) -> int:
        return struct.unpack("<i", self.take(4))[0]

    def int_at(self, offset: int) -> int:
        return struct.unpack_from("<i", self.data, offset)[0]

    def string(self) -> bytes:
        length = self.int()
        if length < 0 or length > 1_000_000:
            raise FormatError(f"invalid string size {length} at 0x{self.offset - 4:x}")
        return self.take(length)

    def int_array(self, maximum: int = 10_000) -> tuple[int, int, int]:
        count_offset = self.offset
        count = self.int()
        if count < 0 or count > maximum:
            raise FormatError(f"invalid array size {count} at 0x{count_offset:x}")
        start = self.offset
        self.take(count * 4)
        return count_offset, start, count


@dataclass(frozen=True)
class Item:
    start: int
    end: int
    vehicle_id: int
    item_type: int
    price_offset: int
    description_offset: int
    display_index_offset: int
    stock_name_offset: int
    price: int
    description_id: int
    display_index: int
    stock_name_id: int


def read_shop_items(data: bytes) -> tuple[int, int, list[Item]]:
    reader = Reader(data)
    reader.take(4)
    for _ in range(8):
        reader.int()
    reader.take(4)
    for expected in (b"mc_head", b"mc_legs", b"mc_torso"):
        if reader.string() != expected:
            raise FormatError("unexpected ShopItems header")
    count_offset = reader.offset
    count = reader.int()
    if count < 0 or count > 10_000:
        raise FormatError(f"invalid ShopItems count {count}")

    items: list[Item] = []
    for _ in range(count):
        start = reader.offset
        reader.take(4)  # shop/category group id
        reader.int()  # reserved id
        reader.int_array()
        reader.string()
        ints_start = reader.offset
        reader.take(4 * 4)
        reader.take(1)
        reader.take(5 * 4)
        reader.string()
        tail_start = reader.offset
        reader.take(5 * 4)

        # The fourth tail integer is the item kind. CarRentalItem's object ID
        # is the fifth integer in the first five-int block.
        item_type = reader.int_at(tail_start + 3 * 4)
        vehicle_id = reader.int_at(ints_start + 4 * 4 + 1 + 4 * 4)
        # In the first four-int block: price, reserved, description string ID,
        # and the localized display-name index.
        items.append(Item(
            start=start,
            end=reader.offset,
            vehicle_id=vehicle_id,
            item_type=item_type,
            price_offset=ints_start,
            description_offset=ints_start + 2 * 4,
            display_index_offset=ints_start + 3 * 4,
            stock_name_offset=tail_start,
            price=reader.int_at(ints_start),
            description_id=reader.int_at(ints_start + 2 * 4),
            display_index=reader.int_at(ints_start + 3 * 4),
            stock_name_id=reader.int_at(tail_start),
        ))

    records_end = reader.offset
    reader.take(4)
    for _ in range(3):
        reader.string()
    if reader.offset != len(data):
        raise FormatError(
            f"ShopItems parse ended at 0x{reader.offset:x}, file ends at 0x{len(data):x}"
        )
    return count_offset, records_end, items


def patch_shop_items(data: bytes) -> bytes:
    count_offset, records_end, items = read_shop_items(data)
    wanted_ids = {HUNTINGPLANE_ID, FLOATING_POINT_ID}
    present_ids = {
        item.vehicle_id for item in items
        if item.item_type == 0 and item.vehicle_id in wanted_ids
    }
    if present_ids:
        raise FormatError(f"target vehicle(s) already present in ShopItems: {sorted(present_ids)}")
    source = next(
        (item for item in items if item.vehicle_id == SEAPLANE_ID and item.item_type == 0),
        None,
    )
    if source is None:
        raise FormatError("could not find the Seaplane item template")

    records = []
    for vehicle_id, price, description, display_index, name_id in (
        (FLOATING_POINT_ID, FLOATING_POINT_PRICE, FLOATING_POINT_DESCRIPTION_ID, 72, FLOATING_POINT_NAME_ID),
        (HUNTINGPLANE_ID, HUNTINGPLANE_PRICE, HUNTINGPLANE_DESCRIPTION_ID, 80, HUNTINGPLANE_NAME_ID),
    ):
        record = bytearray(data[source.start:source.end])
        for offset, value in (
            (source.price_offset, price),
            (source.description_offset, description),
            (source.display_index_offset, display_index),
            (source.stock_name_offset, name_id),
            # The object type is the fifth int in the first five-int block.
            (source.price_offset + 4 * 4 + 1 + 4 * 4, vehicle_id),
        ):
            struct.pack_into("<i", record, offset - source.start, value)
        records.append(record)

    # The item kind remains CarRentalItem (0); update the outer record count.
    output = bytearray(data[:records_end] + b"".join(records) + data[records_end:])
    struct.pack_into("<i", output, count_offset, len(items) + len(records))
    return bytes(output)


@dataclass(frozen=True)
class StockRecord:
    name: str
    item_ids_count_offset: int
    item_ids_start: int
    item_ids_count: int


def read_shop_stock(data: bytes) -> tuple[int, list[StockRecord]]:
    reader = Reader(data)
    count_offset = reader.offset
    count = reader.int()
    if count < 0 or count > 10_000:
        raise FormatError(f"invalid ShopStock count {count}")
    records: list[StockRecord] = []
    for _ in range(count):
        reader.take(4)  # stock entry id
        reader.take(1)  # enabled flag
        ids_count, ids_start, ids_len = reader.int_array()
        reader.int_array()
        reader.int_array()
        name = reader.string().decode("utf-8")
        reader.take(1)  # enabled flag
        reader.int_array()
        reader.int_array()
        records.append(StockRecord(name, ids_count, ids_start, ids_len))
    if reader.offset != len(data):
        raise FormatError(
            f"ShopStock parse ended at 0x{reader.offset:x}, file ends at 0x{len(data):x}"
        )
    return count_offset, records


def patch_shop_stock(data: bytes) -> bytes:
    count_offset, records = read_shop_stock(data)
    record = next((item for item in records if item.name == VEHICLE_SHOP_NAME), None)
    if record is None:
        raise FormatError(f"missing {VEHICLE_SHOP_NAME} stock")
    ids = [
        struct.unpack_from("<i", data, record.item_ids_start + index * 4)[0]
        for index in range(record.item_ids_count)
    ]
    if HUNTINGPLANE_NAME_ID in ids or FLOATING_POINT_NAME_ID in ids:
        raise FormatError("a target vehicle is already present in IGM_VehicleShop")
    output = bytearray(data)
    insertion = record.item_ids_start + record.item_ids_count * 4
    output[insertion:insertion] = struct.pack("<ii", FLOATING_POINT_NAME_ID, HUNTINGPLANE_NAME_ID)
    struct.pack_into("<i", output, record.item_ids_count_offset, record.item_ids_count + 2)
    return bytes(output)


def global_strings(data: bytes) -> list[str]:
    reader = Reader(data)
    count = struct.unpack("<H", reader.take(2))[0]
    values = []
    for _ in range(count):
        length = struct.unpack("<H", reader.take(2))[0]
        values.append(reader.take(length).decode("utf-8"))
    if reader.offset != len(data):
        raise FormatError("global string table has trailing data")
    return values


def encode_global_strings(values: list[str]) -> bytes:
    if len(values) > 0xFFFF:
        raise FormatError("global string table exceeds its 16-bit count")
    output = bytearray(struct.pack("<H", len(values)))
    for value in values:
        encoded = value.encode("utf-8")
        if len(encoded) > 0xFFFF:
            raise FormatError("global string exceeds its 16-bit byte length")
        output.extend(struct.pack("<H", len(encoded)))
        output.extend(encoded)
    return bytes(output)


def patch_global_locale(data: bytes, boat_name: str, boat_description: str) -> bytes:
    values = global_strings(data)
    if len(values) != FLOATING_POINT_NAME_INDEX:
        raise FormatError(
            f"expected {FLOATING_POINT_NAME_INDEX} existing strings, found {len(values)}"
        )
    values.extend((boat_name, boat_description))
    return encode_global_strings(values)


def replace_length_prefixed_string(data: bytes, old: bytes, new: bytes) -> bytes:
    marker = struct.pack("<I", len(old)) + old
    start = data.find(marker)
    if start < 0 or data.find(marker, start + 1) >= 0:
        raise FormatError(f"expected one length-prefixed string {old!r}")
    old_end = start + 4 + len(old)
    padded_end = (old_end + 1 + 3) & ~3
    if data[old_end] != 0:
        raise FormatError(f"string {old!r} is not NUL terminated")
    replacement = struct.pack("<I", len(new)) + new + b"\0"
    replacement += b"\0" * ((-len(replacement)) & 3)
    return data[:start] + replacement + data[padded_end:]


def register_floating_point(data: bytes) -> bytes:
    if len(data) < 4 or struct.unpack_from("<I", data, 0)[0] != 531:
        raise FormatError("unexpected Rio metatypes root count")
    key = b"SeaDoo"
    next_key = b"Seaplane"
    if data.count(key + b"\0") != 1 or data.count(next_key + b"\0") != 1:
        raise FormatError("vehicle metatype keys are ambiguous")
    key_at = data.find(key + b"\0")
    next_at = data.find(next_key + b"\0")
    if key_at < 16 or next_at <= key_at:
        raise FormatError("could not locate SeaDoo and Seaplane metatype records")
    start = key_at - 16
    end = next_at - 16
    header = struct.unpack_from("<4I", data, start)
    if header[1] != 493 or header[2:] != (len(key), len(key)):
        raise FormatError(f"unexpected SeaDoo metatype header: {header!r}")
    row = data[start:end]
    for offset in range(16, len(data) - 16):
        _fields, type_id, first_length, second_length = struct.unpack_from("<4I", data, offset)
        if first_length != second_length or not 1 <= first_length <= 64:
            continue
        key_start = offset + 16
        if key_start + first_length >= len(data) or data[key_start + first_length] != 0:
            continue
        name = data[key_start:key_start + first_length]
        if all(byte == 95 or 48 <= byte <= 57 or 65 <= byte <= 90 or 97 <= byte <= 122
               for byte in name):
            if type_id == 543:
                raise FormatError("metatype id 543 is already in use")
    model = b"./boat_seadoo.bdae"
    if row.count(model) != 1:
        raise FormatError("SeaDoo metatype does not reference exactly one boat model")

    new_key = b"FloatingPoint"
    row = bytearray(row)
    struct.pack_into("<I", row, 4, 543)
    struct.pack_into("<II", row, 8, len(new_key), len(new_key))
    old_key_start = 16
    old_key_end = old_key_start + len(key)
    old_key_padded_end = (old_key_end + 1 + 3) & ~3
    key_payload = new_key + b"\0"
    key_payload += b"\0" * ((-len(key_payload)) & 3)
    row = row[:old_key_start] + key_payload + row[old_key_padded_end:]
    row = bytearray(replace_length_prefixed_string(
        bytes(row), model, b"./boat_floatingpoint_single.bdae"
    ))

    out = bytearray(data + row)
    struct.pack_into("<I", out, 0, struct.unpack_from("<I", data, 0)[0] + 1)
    return bytes(out)


def verify_localized_name(english: bytes, russian: bytes) -> None:
    def string_at(data: bytes, index: int) -> str:
        reader = Reader(data)
        count = struct.unpack("<H", reader.take(2))[0]
        if index >= count:
            raise FormatError(f"localized string index {index} is out of range")
        for current in range(count):
            size = struct.unpack("<H", reader.take(2))[0]
            value = reader.take(size).decode("utf-8")
            if current == index:
                return value
        raise FormatError(f"missing localized string index {index}")

    actual_huntingplane = (
        string_at(english, HUNTINGPLANE_NAME_ID & 0xFFF),
        string_at(russian, HUNTINGPLANE_NAME_ID & 0xFFF),
    )
    if actual_huntingplane != ("Yellow Baron", "Желтый барон"):
        raise FormatError(f"unexpected Huntingplane localization: {actual_huntingplane!r}")
    description = string_at(english, HUNTINGPLANE_DESCRIPTION_ID & 0xFFF)
    if description != "Don't ask where I got it, I won't ask what you want to do with it.":
        raise FormatError(f"unexpected Huntingplane description: {description!r}")
    actual_boat = (
        string_at(english, FLOATING_POINT_NAME_ID & 0xFFF),
        string_at(russian, FLOATING_POINT_NAME_ID & 0xFFF),
        string_at(english, FLOATING_POINT_DESCRIPTION_ID & 0xFFF),
    )
    if actual_boat != (
        "Floating Point",
        "Флоатинг Пойнт",
        "May not look like much, but it has it where it counts.",
    ):
        raise FormatError(f"unexpected Floating Point localization: {actual_boat!r}")


def obb_entries(path: Path) -> tuple[tuple[int, int, int, int], list[tuple[str, int, int, int, int]]]:
    with path.open("rb") as source:
        header_bytes = source.read(16)
        if len(header_bytes) != 16:
            raise FormatError(f"OBB header is truncated: {path}")
        header = struct.unpack(">IIII", header_bytes)
        _unknown, data_offset, names_offset, count = header
        if names_offset != 16 + count * 16 or data_offset < names_offset:
            raise FormatError(f"unsupported OBB index layout: {path}")
        source.seek(16)
        raw_entries = source.read(count * 16)
        names = source.read(data_offset - names_offset)
        if len(raw_entries) != count * 16 or len(names) != data_offset - names_offset:
            raise FormatError(f"OBB index is truncated: {path}")

    entries = []
    for index in range(count):
        offset, size, name_offset, flags = struct.unpack_from(">IIII", raw_entries, index * 16)
        if name_offset >= len(names):
            raise FormatError(f"OBB entry {index} has invalid name offset")
        end = names.find(b"\0", name_offset)
        if end < 0:
            raise FormatError(f"OBB entry {index} has an unterminated name")
        name = names[name_offset:end].decode("utf-8")
        entries.append((name, offset, size, name_offset, flags))
    return header, entries


def read_obb_payload(path: Path, entry: tuple[str, int, int, int, int]) -> bytes:
    _name, offset, size, _name_offset, _flags = entry
    with path.open("rb") as source:
        source.seek(offset)
        payload = source.read(size)
    if len(payload) != size:
        raise FormatError(f"OBB payload is truncated: {entry[0]}")
    return payload


def patch_obb(template: Path, output: Path,
              replacements: dict[str, tuple[bytes, bytes]],
              extra_assets: list[tuple[str, bytes]]) -> None:
    header, entries = obb_entries(template)
    indexed: dict[str, tuple[int, int, int, int]] = {}
    for record_index, (name, offset, size, name_offset, flags) in enumerate(entries):
        if name in replacements:
            if name in indexed:
                raise FormatError(f"duplicate OBB entry: {name}")
            indexed[name] = (record_index, offset, size, name_offset, flags)
    if indexed.keys() != replacements.keys():
        raise FormatError(f"OBB is missing required entries: {sorted(replacements.keys() - indexed.keys())}")

    original_size = template.stat().st_size
    for name, (expected, _replacement) in replacements.items():
        _record_index, offset, size, _name_offset, _flags = indexed[name]
        if size != len(expected):
            raise FormatError(f"extracted {name} size differs from the OBB index")
        with template.open("rb") as source:
            source.seek(offset)
            if source.read(size) != expected:
                raise FormatError(f"extracted {name} differs from the OBB payload")

    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise FormatError(f"refusing to overwrite existing output: {output}")
    # APFS clonefile keeps the 2.1 GB original data shared until modified.
    subprocess.run(["cp", "-c", str(template), str(output)], check=True)
    try:
        with output.open("r+b") as target:
            target.seek(0, os.SEEK_END)
            append_offset = target.tell()
            for name, (_expected, replacement) in replacements.items():
                record_index, _offset, _size, name_offset, flags = indexed[name]
                if append_offset + len(replacement) > 0xFFFFFFFF:
                    raise FormatError("updated OBB exceeds its 32-bit addressable size")
                target.write(replacement)
                target.seek(16 + record_index * 16)
                target.write(struct.pack(">IIII", append_offset, len(replacement), name_offset, flags))
                append_offset += len(replacement)
                target.seek(0, os.SEEK_END)

            empty_slots = [
                (record_index, offset, size, name_offset, flags, name)
                for record_index, (name, offset, size, name_offset, flags) in enumerate(entries)
                if size == 0
            ]
            if len(empty_slots) < len(extra_assets):
                raise FormatError("OBB has too few zero-length index slots for imported model assets")
            target.seek(header[2])
            names = bytearray(target.read(header[1] - header[2]))
            used_slots: set[int] = set()
            for asset_name, payload in extra_assets:
                matching_slots = [
                    slot for slot in empty_slots
                    if slot[0] not in used_slots and len(slot[5]) == len(asset_name)
                ]
                if not matching_slots:
                    raise FormatError(
                        f"no zero-length OBB slot has the exact path length for {asset_name!r}"
                    )
                slot = matching_slots[0]
                record_index, _offset, _size, name_offset, flags, old_name = slot
                encoded_name = asset_name.encode("utf-8")
                old_end = names.find(b"\0", name_offset)
                if old_end < 0 or len(encoded_name) != old_end - name_offset:
                    raise FormatError(
                        f"OBB name slot length changed for {asset_name!r}"
                    )
                if any(name == asset_name for name, *_rest in entries):
                    raise FormatError(f"OBB already has entry {asset_name!r}")
                if append_offset + len(payload) > 0xFFFFFFFF:
                    raise FormatError("updated OBB exceeds its 32-bit addressable size")
                names[name_offset:old_end + 1] = encoded_name + b"\0"
                target.seek(append_offset)
                target.write(payload)
                target.seek(16 + record_index * 16)
                target.write(struct.pack(">IIII", append_offset, len(payload), name_offset, flags))
                append_offset += len(payload)
                used_slots.add(record_index)
                target.seek(0, os.SEEK_END)
            target.seek(header[2])
            target.write(names)
            target.flush()
            os.fsync(target.fileno())

        verify_header, verify_entries = obb_entries(output)
        if verify_header != header:
            raise FormatError("OBB header changed unexpectedly")
        expected_map = dict(replacements)
        expected_assets = dict(extra_assets)
        verified_names = set()
        with output.open("rb") as source:
            for name, offset, size, _name_offset, _flags in verify_entries:
                if name in expected_map:
                    expected, wanted = expected_map[name]
                    source.seek(offset)
                    if size != len(wanted) or source.read(size) != wanted:
                        raise FormatError(f"updated OBB entry failed read-back: {name}")
                    verified_names.add(name)
                elif name in expected_assets:
                    wanted = expected_assets[name]
                    source.seek(offset)
                    if size != len(wanted) or source.read(size) != wanted:
                        raise FormatError(f"imported OBB asset failed read-back: {name}")
                    verified_names.add(name)
                else:
                    old = next(entry for entry in entries if entry[0] == name)
                    if (offset, size) != old[1:3]:
                        raise FormatError(f"unmodified OBB index entry moved: {name}")
        if verified_names != expected_map.keys() | expected_assets.keys():
            raise FormatError("not all OBB replacements and imports passed read-back")
        # The Vita-side OBB reader consumes one NUL-terminated name per index
        # record in sequence. Validate that representation as well as offsets.
        sequential_name_offset = 0
        names_start = verify_header[2]
        names_end = verify_header[1]
        with output.open("rb") as source:
            source.seek(names_start)
            sequential_names = source.read(names_end - names_start)
        for name, _offset, _size, name_offset, _flags in verify_entries:
            if name_offset != sequential_name_offset:
                raise FormatError("OBB index is not aligned with sequential runtime name parsing")
            encoded_name = name.encode("utf-8")
            if sequential_names[sequential_name_offset:sequential_name_offset + len(encoded_name)] != encoded_name:
                raise FormatError("OBB runtime name sequence disagrees with the archive index")
            if sequential_name_offset + len(encoded_name) >= len(sequential_names) or sequential_names[sequential_name_offset + len(encoded_name)] != 0:
                raise FormatError("OBB runtime name is not NUL terminated")
            sequential_name_offset += len(encoded_name) + 1
        if output.stat().st_size != (
            original_size + sum(len(replacement) for _expected, replacement in replacements.values())
            + sum(len(payload) for _name, payload in extra_assets)
        ):
            raise FormatError("updated OBB has an unexpected length")
    except BaseException:
        output.unlink(missing_ok=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shop-items", type=Path, required=True)
    parser.add_argument("--shop-stock", type=Path, required=True)
    parser.add_argument("--english", type=Path, required=True)
    parser.add_argument("--russian", type=Path, required=True)
    parser.add_argument("--obb-template", type=Path, required=True)
    parser.add_argument("--boat-model", type=Path, required=True)
    parser.add_argument("--boat-lod-model", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    english = args.english.read_bytes()
    russian = args.russian.read_bytes()
    patched_english = patch_global_locale(
        english, "Floating Point", "May not look like much, but it has it where it counts."
    )
    patched_russian = patch_global_locale(
        russian, "Флоатинг Пойнт", "На вид ничего особенного, зато он справится с любой задачей."
    )
    verify_localized_name(patched_english, patched_russian)
    patched_items = patch_shop_items(args.shop_items.read_bytes())
    patched_stock = patch_shop_stock(args.shop_stock.read_bytes())
    _, _, items = read_shop_items(patched_items)
    _, stock_records = read_shop_stock(patched_stock)
    vehicle = next(item for item in items if item.vehicle_id == HUNTINGPLANE_ID)
    boat = next(item for item in items if item.vehicle_id == FLOATING_POINT_ID)
    stock = next(item for item in stock_records if item.name == VEHICLE_SHOP_NAME)
    stock_ids = [
        struct.unpack_from("<i", patched_stock, stock.item_ids_start + index * 4)[0]
        for index in range(stock.item_ids_count)
    ]
    if (
        HUNTINGPLANE_NAME_ID not in stock_ids
        or FLOATING_POINT_NAME_ID not in stock_ids
        or vehicle.item_type != 0
        or vehicle.price != HUNTINGPLANE_PRICE
        or vehicle.description_id != HUNTINGPLANE_DESCRIPTION_ID
        or vehicle.display_index != 80
        or vehicle.stock_name_id != HUNTINGPLANE_NAME_ID
    ):
        raise FormatError("Huntingplane output did not pass structural verification")
    if (
        boat.item_type != 0
        or boat.price != FLOATING_POINT_PRICE
        or boat.description_id != FLOATING_POINT_DESCRIPTION_ID
        or boat.display_index != 72
        or boat.stock_name_id != FLOATING_POINT_NAME_ID
    ):
        raise FormatError("Floating Point output did not pass structural verification")

    boat_model = args.boat_model.read_bytes()
    boat_lod_model = args.boat_lod_model.read_bytes()
    import zipfile
    import io
    for label, payload in (("Floating Point", boat_model), ("Floating Point LOD", boat_lod_model)):
        try:
            with zipfile.ZipFile(io.BytesIO(payload)) as archive:
                if not {"dependancies.txt", "little_endian_quantized.bdae", "version.nfo"}.issubset(archive.namelist()):
                    raise FormatError(f"{label} BDAE is missing required container entries")
                dependencies = archive.read("dependancies.txt").decode("utf-8").splitlines()
                missing = [name for name in dependencies if name not in {"alphas.tga", "car_body_reflection.tga", "car_poor_texture_d.tga", "car_poor_texture_damage_d.tga"}]
                if missing:
                    raise FormatError(f"{label} has unexpected dependencies: {missing}")
        except zipfile.BadZipFile as error:
            raise FormatError(f"{label} asset is not a valid BDAE container") from error

    output_obb = args.out / args.obb_template.name
    original_entries = obb_entries(args.obb_template)[1]
    original_names = {entry[0] for entry in original_entries}
    with args.obb_template.open("rb") as source:
        for dependency in ("alphas.tga", "car_body_reflection.tga", "car_poor_texture_d.tga", "car_poor_texture_damage_d.tga"):
            if f"_packEtc/{dependency}" not in original_names:
                raise FormatError(f"Rio OBB is missing boat texture dependency: {dependency}")
    _template_header, template_entries = obb_entries(args.obb_template)
    entry_by_name = {entry[0]: entry for entry in template_entries}
    replacements = {
        "_packRes/ShopItems.gmap": (args.shop_items.read_bytes(), patched_items),
        "_packRes/ShopStock.gmap": (args.shop_stock.read_bytes(), patched_stock),
        "_packRes/global.english": (english, patched_english),
        "_packRes/global.russian": (russian, patched_russian),
    }
    locale_fallback = "Floating Point"
    locale_description = "May not look like much, but it has it where it counts."
    for language in (
        "chinese", "french", "german", "italian", "japanese", "korean",
        "portuguese", "spanish",
    ):
        name = f"_packRes/global.{language}"
        original = read_obb_payload(args.obb_template, entry_by_name[name])
        replacements[name] = (
            original,
            patch_global_locale(original, locale_fallback, locale_description),
        )
    metatypes_name = "_packRes/metatypes.gmap"
    original_metatypes = read_obb_payload(args.obb_template, entry_by_name[metatypes_name])
    patched_metatypes = register_floating_point(original_metatypes)
    if patched_metatypes.count(b"./boat_floatingpoint_single.bdae") != 1:
        raise FormatError("Floating Point metatype does not reference its registered model")
    replacements[metatypes_name] = (original_metatypes, patched_metatypes)

    patch_obb(
        args.obb_template,
        output_obb,
        replacements,
        [
            ("_packRes/boat_floatingpoint_single.bdae", boat_model),
            ("_packRes/boat_floatingpoint_single_lo.bdae", boat_lod_model),
        ],
    )
    print(f"created: {output_obb} ({output_obb.stat().st_size} bytes)")
    print("OBB replacements: ShopItems, ShopStock, all localized global tables, metatypes")
    print("OBB index entries changed: two zero-length slots for imported boat models")
    print(f"vehicle={FLOATING_POINT_ID} name=Floating Point price={FLOATING_POINT_PRICE} display_index=72")
    print(f"vehicle={HUNTINGPLANE_ID} name=Yellow Baron / Желтый барон price={HUNTINGPLANE_PRICE}")
    print(f"ShopItems records={len(items)} IGM_VehicleShop entries={stock.item_ids_count}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, FormatError) as error:
        raise SystemExit(f"error: {error}")
