#!/usr/bin/env python3
"""Prepare the supplied MOD for offline startup, HID input and bounded native tracing."""
import argparse
from collections import Counter
from copy import copy
import hashlib
import json
from pathlib import Path
import struct
import zipfile
import zlib

MOD_NATIVE_SHA256 = "6a2619c7640747344e0be51abf37a3110772c8ffb04bfb6e2b18452509a41361"
MOD_SHA256 = "d19a9cc7a804f8d691b5297caea3bfda057c1db2b4189b5f956e2c3845cd89ef"
PREFIX = "Lcom/gameloft/android/ANMP/GloftGGHM/"


def analytics_payload(game_data, gadget, native_path, trace_mode="initialization"):
    import lief
    if gadget[:5] != b"\x7fELF\x01" or struct.unpack_from("<H", gadget, 18)[0] != 40:
        raise ValueError("Gadget must be ELF32 ARM")
    original = lief.ELF.parse(game_data)
    if original is None:
        raise ValueError("Cannot parse native library")
    original_symbols = {s.name: s.value for s in original.dynamic_symbols}
    if trace_mode == "physics":
        from game_profile import LIBRARIES
        if hashlib.sha256(game_data).hexdigest() not in (LIBRARIES["lib/armeabi-v7a/libGangstar4.so"]["sha256"], MOD_NATIVE_SHA256):
            raise ValueError("Physics tracer requires a fingerprinted 1.7.0g native library")
    text_address = original.get_section(".text").virtual_address
    text_bytes = bytes(original.get_section(".text").content)
    profile = {
        "native_sha256": hashlib.sha256(game_data).hexdigest(),
        "text_sha256": hashlib.sha256(text_bytes).hexdigest(),
        "jni": [{"name": s.name, "offset": s.value} for s in original.dynamic_symbols
                if s.value and (s.name.startswith("Java_") or s.name == "JNI_OnLoad")],
        "ctors": [f.address for f in original.ctor_functions],
        "hooks": [{"name": name, "offset": offset} for name, offset in [
            ("App::HasContext", 0xaf03ac), ("Thread::GetSequentialThreadId", 0xb00598),
            ("Thread::sIsMain", 0xb000ac), ("App::SetAsMainThread", 0xaf0148),
            ("App::UnsetAsMainThread", 0xaf026c), ("Task::Wait", 0xb381c0),
            ("Task::Done", 0xb383ec), ("Task::Start", 0xb384cc),
            ("CPU_GRAPHICS_TASK::Push", 0xbf1014), ("CBuffer::bindImpl", 0xc40ca0),
            ("CBufferBase::deferredBindImpl", 0xbff330), ("App::MyInit", 0xaf020c)]],
    }
    if trace_mode == "physics":
        profile["physics"] = physics_trace_profile(original)
    original.add_library("libgadget.so")
    original.write(str(native_path))
    patched = lief.ELF.parse(str(native_path))
    if (bytes(patched.get_section(".text").content) != text_bytes or
            patched.get_section(".text").virtual_address != text_address or
            {s.name: s.value for s in patched.dynamic_symbols} != original_symbols):
        raise ValueError("LIEF changed native instructions or symbol addresses")
    if trace_mode == "saves":
        configuration = {"interaction": {"type": "listen", "address": "127.0.0.1", "port": 27043,
                                          "on_port_conflict": "fail", "on_load": "wait"}, "runtime": "qjs"}
        return native_path.read_bytes(), {
            "lib/armeabi-v7a/libgadget.so": gadget,
            "lib/armeabi-v7a/libgadget.config.so": json.dumps(configuration).encode(),
        }
    here = Path(__file__).parent
    script = "const GAME_TRACE_PROFILE = " + json.dumps(profile) + ";\n"
    if trace_mode == "physics":
        script += (here / "android_physics_trace.js").read_text()
    else:
        script += ("const TRACE_SINK_SOURCE = " + json.dumps((here / "android_trace_sink.c").read_text()) + ";\n" +
                   (here / "android_trace.js").read_text())
    configuration = {"interaction": {"type": "script", "path": "libvegas_trace.js.so"}, "runtime": "qjs"}
    return native_path.read_bytes(), {
        "lib/armeabi-v7a/libgadget.so": gadget,
        "lib/armeabi-v7a/libgadget.config.so": json.dumps(configuration).encode(),
        "lib/armeabi-v7a/libvegas_trace.js.so": script.encode(),
    }



def physics_trace_profile(image):
    from game_profile import PHYSICS_STATE, PLAYER_STATE
    profile = {key: value for key, value in PHYSICS_STATE.items() if key != "functions"}
    profile["player"] = PLAYER_STATE
    functions = dict(PHYSICS_STATE["functions"])
    for name in ("get_player", "get_position"):
        guard = struct.pack("<II", PLAYER_STATE[name + "_word0"], PLAYER_STATE[name + "_word1"]).hex()
        functions[name] = (PLAYER_STATE[name], guard, "pointer")
    profile["functions"] = []
    for name, (offset, guard, result) in functions.items():
        if bytes(image.get_content_from_virtual_address(offset, 8)).hex() != guard:
            raise ValueError("Physics function guard mismatch: " + name)
        profile["functions"].append({"name": name, "offset": offset, "guard": guard, "result": result})
    return profile


def patch_dex(original):
    data = bytearray(original)

    def u32(offset):
        return struct.unpack_from("<I", data, offset)[0]

    def leb(offset):
        value = 0
        for shift in range(0, 35, 7):
            byte = data[offset]
            offset += 1
            value |= (byte & 127) << shift
            if not byte & 128:
                return value, offset
        raise ValueError("Invalid ULEB128")

    if data[:8] != b"dex\n035\0" or u32(32) != len(data) or u32(40) != 0x12345678:
        raise ValueError("Unexpected DEX header")
    if original[12:32] != hashlib.sha1(original[32:]).digest():
        raise ValueError("Invalid source DEX signature")
    if u32(8) != zlib.adler32(original[12:]) & 0xffffffff:
        raise ValueError("Invalid source DEX checksum")
    strings = []
    for i in range(u32(56)):
        _, offset = leb(u32(u32(60) + 4 * i))
        end = data.index(0, offset)
        strings.append(data[offset:end].decode("utf-8", errors="replace"))
    types = [strings[u32(u32(68) + 4 * i)] for i in range(u32(64))]
    methods = [struct.unpack_from("<HHI", data, u32(92) + 8 * i) for i in range(u32(88))]
    matches = [i for i, (owner, _, name) in enumerate(methods)
               if types[owner] == PREFIX + "GLUtils/SUtils;" and strings[name] == "getOverriddenSetting"]
    if len(matches) != 1:
        raise ValueError("Unexpected override method table")
    target = matches[0]
    _, proto, _ = methods[target]
    _, return_type, parameters = struct.unpack_from("<III", data, u32(76) + 12 * proto)
    argument_types = [types[struct.unpack_from("<H", data, parameters + 4 + 2 * i)[0]]
                      for i in range(u32(parameters))]
    if types[return_type] != "Ljava/lang/String;" or argument_types != ["Ljava/lang/String;"] * 2:
        raise ValueError("Unexpected override signature")
    skip, one = strings.index("SKIP_VALIDATION"), strings.index("1")
    if one > 65535:
        raise ValueError("String index requires jumbo encoding")
    def class_methods(class_name):
        classes = [u32(u32(100) + 32 * i + 24) for i in range(u32(96))
                   if types[u32(u32(100) + 32 * i)] == class_name]
        if len(classes) != 1 or not classes[0]:
            raise ValueError("Unexpected class: " + class_name)
        offset = classes[0]
        counts = []
        for _ in range(4):
            count, offset = leb(offset)
            counts.append(count)
        for _ in range(counts[0] + counts[1]):
            _, offset = leb(offset)
            _, offset = leb(offset)
        for count in counts[2:]:
            index = 0
            for _ in range(count):
                delta, offset = leb(offset)
                index += delta
                _, offset = leb(offset)
                code, offset = leb(offset)
                if code:
                    yield index, code

    changes = []
    for index, code in class_methods(PREFIX + "installer/GameInstaller;"):
        name = strings[methods[index][2]]
        if name not in ("p", "v"):
            continue
        if u32(u32(76) + 12 * methods[index][1] + 8) != 0:
            raise ValueError("Unexpected installer method arguments")
        start, units = code + 16, u32(code + 12)
        body = struct.unpack_from("<" + "H" * units, data, start)
        for j in range(2, units - 3):
            if body[j] != 0x2071 or body[j + 1] != target or body[j + 3] != 0x000c:
                continue
            arguments = body[j + 2]
            key_register = (arguments >> 4) & 15
            if arguments != key_register << 4 or key_register not in (1, 2):
                continue
            if body[j - 2:j] != ((key_register << 8) | 0x1a, skip):
                continue
            location = start + j * 2
            # Same code size preserves every branch, exception range and other class.
            struct.pack_into("<HHHH", data, location, 0x001a, one, 0, 0)
            changes.append((name, location))
    if Counter(name for name, _ in changes) != {"p": 2, "v": 2}:
        raise ValueError("Expected exactly two QA settings in each installer method")
    allowed = set(range(8, 32))
    for _, location in changes:
        allowed.update(range(location, location + 8))
    social = "Lcom/gameloft/GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib;"

    def social_method(name):
        found = [i for i, (owner, _, method_name) in enumerate(methods)
                 if types[owner] == social and strings[method_name] == name]
        if len(found) != 1:
            raise ValueError("Unexpected social method: " + name)
        return found[0]

    init_pending = social_method("access$202")
    auth_pending = social_method("access$302")
    complete = social_method("nativeGameAPIComplete")
    runs = [(index, code) for index, code in class_methods(social[:-1] + "$6;")
            if strings[methods[index][2]] == "run"]
    if len(runs) != 1:
        raise ValueError("Unexpected GameAPI initializer")
    _, code = runs[0]
    start, units = code + 16, u32(code + 12)
    expected = (0x1012, 0x1071, init_pending, 0, 0x1071, auth_pending, 0)
    if struct.unpack_from("<7H", data, start) != expected or struct.unpack_from("<H", data, code + 6)[0]:
        raise ValueError("Unexpected GameAPI initialization code or exception table")
    # Keep runOnUiThread dispatch. Failed sign-in during initialization also calls
    # nativeGameAPIComplete; it completes setup while IsLoggedIn stays false.
    replacement = [0x0012, 0x1071, init_pending, 0, 0x1071, auth_pending, 0,
                   0x0071, complete, 0, 0x000e]
    if len(replacement) > units:
        raise ValueError("GameAPI code does not fit")
    replacement += [0x000e] * (units - len(replacement))
    struct.pack_into("<" + "H" * units, data, start, *replacement)
    allowed.update(range(start, start + 2 * units))
    changes.append(("GameAPI offline initialization", start))

    def require_method(owner, name, result, arguments):
        found = [i for i, (t, _, n) in enumerate(methods) if types[t] == owner and strings[n] == name]
        if len(found) != 1:
            raise ValueError("Unexpected method: " + owner + name)
        index = found[0]
        _, return_type, parameters = struct.unpack_from("<III", data, u32(76) + 12 * methods[index][1])
        actual = [] if not parameters else [types[struct.unpack_from("<H", data, parameters + 4 + i * 2)[0]]
                                            for i in range(u32(parameters))]
        if types[return_type] != result or actual != arguments:
            raise ValueError("Unexpected signature: " + owner + name)
        return index

    device = "Landroid/view/InputDevice;"
    game = PREFIX + "Game;"
    get_type = require_method(game, "getDeviceType", "I", [device])
    axis_profile = require_method(game, "GetControllerByAxisHash", "I", [device])
    get_sources = require_method(device, "getSources", "I", [])
    codes = [code for index, code in class_methods(game) if index == get_type]
    if len(codes) != 1:
        raise ValueError("Unexpected controller detector")
    code = codes[0]
    if struct.unpack_from("<4H", data, code) != (3, 1, 2, 0) or u32(code + 12) != 165:
        raise ValueError("Unexpected controller detector layout")
    location = code + 16 + 124 * 2
    expected = [int(word, 16) for word in (
        "106e 1490 0002 000c 106e 8106 0000 000c 011a 3f61 206e 80eb 0010 000a 0038 0018 "
        "106e 1490 0002 000c 106e 8106 0000 000c 011a 4226 206e 80eb 0010 000a 0039 0008 "
        "1071 2908 0002 000a 0029 ff76 f012 0029 ff73").split()]
    if struct.unpack_from("<41H", data, location) != tuple(expected) or axis_profile != 0x2908 or get_sources != 0x1491:
        raise ValueError("Unexpected controller fallback profile")
    # Keep special device profiles; accept standard joysticks by source, not name.
    # Existing branches to units 124 and 162 retain their targets and register types.
    replacement = [0x106e, get_sources, 2, 0x000a, 0x0114, 0x0010, 0x0100,
                   0x10b5, 0x1033, 0x001e, 0x1071, axis_profile, 2, 0x000a, 0x000f]
    replacement += [0] * 23 + [0xf012, 0x000f, 0]
    struct.pack_into("<41H", data, location, *replacement)
    allowed.update(range(location, location + 82))
    changes.append(("Standard joystick detection", location))
    motion = require_method(game, "dispatchGenericMotionEvent", "Z", ["Landroid/view/MotionEvent;"])
    codes = [code for index, code in class_methods(game) if index == motion]
    if len(codes) != 1:
        raise ValueError("Unexpected joystick motion handler")
    code = codes[0]
    if struct.unpack_from("<4H", data, code) != (12, 2, 4, 0) or u32(code + 12) != 469:
        raise ValueError("Unexpected joystick motion layout")
    fields = [struct.unpack_from("<HHI", data, u32(84) + i * 8) for i in range(u32(80))]
    if types[fields[0x102a][0]] != game or types[fields[0x102a][1]] != "I" or strings[fields[0x102a][2]] != "bC":
        raise ValueError("Unexpected current controller profile field")
    if (require_method(game, "packFloatToInt255", "I", ["F"]) != 0x29c5 or
            require_method(game, "nativeJoyPadMove", "V", ["I"] * 4) != 0x29a2):
        raise ValueError("Unexpected joystick ABI")
    location = code + 16 + 52 * 2
    if hashlib.sha256(data[location:location + 156]).hexdigest() != "5c29cc5daca45a15f8e6f41bdb771f0ab77e11708f538401531131a197ff0237":
        raise ValueError("Unexpected right-stick instructions")
    # Generic HID uses Z/RZ; RX/RY on the observed Sony-compatible pad are triggers.
    # Send neutral values too, with abs(packed)<32 as the stick dead zone.
    replacement = [int(word, 16) for word in (
        "0013 000b 206e 14bb 000b 000a 0113 000e 206e 14bb 001b 010a "
        "0413 000c 206e 14bb 004b 040a 0513 000d 206e 14bb 005b 050a "
        "1071 29c5 0000 000a 1071 29c5 0001 010a 1071 29c5 0004 040a 1071 29c5 0005 050a "
        "0860 102a 0913 0008 9832 0007 0038 0016 0138 0014 0b28 "
        "06db 2000 0639 0003 0012 06db 2001 0639 0003 0112 "
        "0612 1712 4071 29a2 7610 9832 000c 0438 000a 0538 0008 "
        "0612 1712 4071 29a2 7654 0000").split()]
    if len(replacement) != 78:
        raise ValueError("Right-stick replacement size mismatch")
    struct.pack_into("<78H", data, location, *replacement)
    allowed.update(range(location, location + 156))
    changes.append(("Generic right-stick axes and neutral", location))
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(data[12:]) & 0xffffffff)
    if len(data) != len(original) or any(a != b and i not in allowed
                                         for i, (a, b) in enumerate(zip(original, data))):
        raise ValueError("Unexpected DEX changes")
    return bytes(data), changes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gadget", type=Path, help="embed an ARM32 Frida Gadget for bounded native tracing")
    parser.add_argument("--trace-mode", choices=("initialization", "physics", "saves"), default="initialization",
                        help="select bounded tracing or temporary loopback-only save export; restore the normal APK afterwards")
    args = parser.parse_args()
    if args.trace_mode != "initialization" and not args.gadget:
        parser.error("--trace-mode requires --gadget")
    if hashlib.sha256(args.apk.read_bytes()).hexdigest() != MOD_SHA256:
        parser.error("APK does not match the supplied 1.7.0 MOD profile")
    if args.output.exists():
        parser.error("Output exists; preserved")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    native_path = args.output.with_suffix(".native.so")
    if args.gadget and native_path.exists():
        parser.error("Native output exists; preserved")
    with zipfile.ZipFile(args.apk) as source:
        dex, changes = patch_dex(source.read("classes.dex"))
        native_name = "lib/armeabi-v7a/libGangstar4.so"
        native_data, payload = None, {}
        if args.gadget:
            native_data, payload = analytics_payload(source.read(native_name), args.gadget.read_bytes(), native_path, args.trace_mode)
        with zipfile.ZipFile(args.output, "x") as output:
            for item in source.infolist():
                if item.filename.upper().startswith("META-INF/"):
                    continue
                content = (dex if item.filename == "classes.dex" else
                           native_data if native_data is not None and item.filename == native_name else source.read(item))
                output.writestr(copy(item), content)
            for name, content in payload.items():
                output.writestr(name, content, compress_type=zipfile.ZIP_DEFLATED)
        with zipfile.ZipFile(args.output) as output:
            for item in source.infolist():
                if item.filename == "classes.dex" or item.filename.upper().startswith("META-INF/"):
                    continue
                if native_data is not None and item.filename == native_name:
                    if output.read(item.filename) != native_data:
                        raise ValueError("Native injection content mismatch")
                    continue
                if source.read(item) != output.read(item.filename):
                    raise ValueError("Unexpected APK content change: " + item.filename)
    print("Verified in-place DEX patches:", [(name, hex(location)) for name, location in changes])
    print("Native trace embedded:", bool(args.gadget), "; signing required:", args.output)


if __name__ == "__main__":
    main()
