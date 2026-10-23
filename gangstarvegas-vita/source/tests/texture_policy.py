"""Exercise the linked ARM wrappers with the original native release/destructors."""
import hashlib
from pathlib import Path
import struct
import sys

from functools import lru_cache
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import (
    UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R4,
    UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_SP, UC_ARM_REG_LR,
    UC_CPU_ARM_CORTEX_A9,
)

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from game_profile import LIBRARIES, TEXTURE_POLICY as PROFILE

GAME = ROOT / 'refs/android/apk/lib/armeabi-v7a/libGangstar4.so'
BASE = 0x98000000
SELF, POLICY, STACK, RETURN = 0x100000, 0x100080, 0x200800, 0x300000


def word(cpu, address):
    return struct.unpack('<I', cpu.mem_read(address, 4))[0]


def store(cpu, address, value):
    cpu.mem_write(address, struct.pack('<I', value))


@lru_cache(maxsize=2)
def elf_image(path, base):
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x01\x01'
    phoff, shoff = struct.unpack_from('<II', data, 28)
    phsize, phcount, shsize, shcount = struct.unpack_from('<4H', data, 42)
    segments = []
    for i in range(phcount):
        kind, offset, address, _, size, memory, _, _ = struct.unpack_from('<8I', data, phoff + i * phsize)
        if kind == 1:
            segments.append((address, memory, bytearray(data[offset:offset + size])))
    sections = [struct.unpack_from('<10I', data, shoff + i * shsize) for i in range(shcount)]
    symbols = {}
    for section in sections:
        if not base and section[1] == 2:
            strings = sections[section[6]]
            names = data[strings[4]:strings[4] + strings[5]]
            for at in range(section[4], section[4] + section[5], section[9]):
                name, value = struct.unpack_from('<II', data, at)
                if name and value:
                    symbols[names[name:names.index(0, name)].decode()] = value
        if base and section[1] == 9:
            for at in range(section[4], section[4] + section[5], section[9]):
                address, info = struct.unpack_from('<II', data, at)
                if info & 255 != 23:
                    continue
                for start, memory, content in segments:
                    if start <= address < start + memory:
                        index = address - start
                        if index + 4 > len(content):
                            content.extend(bytes(index + 4 - len(content)))
                        value = struct.unpack_from('<I', content, index)[0]
                        struct.pack_into('<I', content, index, value + base)
                        break
                else:
                    raise ValueError('Relocation outside LOAD segments')
    low = min(a for a, _, _ in segments) & ~4095
    high = (max(a + n for a, n, _ in segments) + 4095) & ~4095
    return low, high, [(a, bytes(b)) for a, _, b in segments], symbols


def load(cpu, path, base=0):
    low, high, segments, symbols = elf_image(path, base)
    cpu.mem_map(base + low, high - low)
    for address, content in segments:
        cpu.mem_write(base + address, content)
    return symbols


def machine():
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    symbols = load(cpu, ROOT / 'build/gangstarvegas_vita')
    load(cpu, GAME, BASE)
    for address in (SELF, STACK & ~4095, RETURN):
        cpu.mem_map(address, 4096)
    cpu.reg_write(UC_ARM_REG_CPSR, 0x10)
    cpu.reg_write(UC_ARM_REG_SP, STACK)
    cpu.reg_write(UC_ARM_REG_LR, RETURN)
    store(cpu, SELF, BASE + PROFILE['selector_vtable'])
    store(cpu, SELF + 4, 1)
    store(cpu, SELF + 8, POLICY)
    store(cpu, POLICY, BASE + PROFILE['policy_vtable'])
    store(cpu, POLICY + 4, 2)  # One selector owner and one texture owner.
    for name, offset in [('policy_release', 'release'),
                         ('policy_destructor', 'destructor'),
                         ('policy_deleting_destructor', 'deleting_destructor')]:
        store(cpu, symbols[name], BASE + PROFILE[offset])
    return cpu, symbols


def call(cpu, address, argument, stop=RETURN):
    cpu.reg_write(UC_ARM_REG_R0, argument)
    cpu.reg_write(UC_ARM_REG_SP, STACK)
    cpu.reg_write(UC_ARM_REG_LR, RETURN)
    cpu.emu_start(address, stop, count=10000)
    assert cpu.reg_read(UC_ARM_REG_PC) == (stop & ~1), hex(cpu.reg_read(UC_ARM_REG_PC))


def main():
    assert hashlib.sha256(GAME.read_bytes()).hexdigest() == LIBRARIES['lib/armeabi-v7a/libGangstar4.so']['sha256']
    cpu, symbols = machine()
    entry = BASE + PROFILE['constructor']
    assert word(cpu, entry) == PROFILE['constructor_word0']
    assert word(cpu, entry + 4) == PROFILE['constructor_word1']
    assert word(cpu, BASE + PROFILE['selector_vtable']) == BASE + PROFILE['destructor']
    assert word(cpu, BASE + PROFILE['selector_vtable'] + 4) == BASE + PROFILE['deleting_destructor']
    store(cpu, symbols['policy_constructor_tail'], entry + 8)
    cpu.reg_write(UC_ARM_REG_R0, SELF)
    cpu.reg_write(UC_ARM_REG_R1, SELF + 256)
    for reg, value in [(UC_ARM_REG_R4, 44), (UC_ARM_REG_R5, 55), (UC_ARM_REG_R6, 66)]:
        cpu.reg_write(reg, value)
    cpu.emu_start(symbols['native_policy_selector'], entry + 8, count=30)
    assert cpu.reg_read(UC_ARM_REG_PC) == entry + 8
    assert cpu.reg_read(UC_ARM_REG_R0) == SELF
    assert cpu.reg_read(UC_ARM_REG_R1) == SELF + 256
    assert cpu.reg_read(UC_ARM_REG_R4) == SELF
    assert cpu.reg_read(UC_ARM_REG_SP) == STACK - 16
    assert list(struct.unpack('<4I', cpu.mem_read(STACK - 16, 16))) == [44, 55, 66, RETURN]

    # Actual native last-texture release must leave the selector's policy alive.
    cpu, symbols = machine()
    call(cpu, BASE + PROFILE['release'], POLICY)
    assert word(cpu, POLICY + 4) == 1 and word(cpu, SELF + 8) == POLICY

    # Both compiled destructor wrappers release exactly once, before native cleanup.
    for name, stop in [('destroy_policy_selector', RETURN),
                       ('delete_policy_selector', BASE + 0x467718)]:
        cpu, symbols = machine()
        call(cpu, symbols[name], SELF, stop)
        assert word(cpu, SELF + 8) == 0
        assert word(cpu, POLICY + 4) == 1

    # With no remaining texture, releasing the selector reaches the real policy D0.
    cpu, symbols = machine()
    store(cpu, POLICY + 4, 1)
    call(cpu, symbols['destroy_policy_selector'], SELF, BASE + 0x76b474)
    assert word(cpu, SELF + 8) == 0 and word(cpu, POLICY + 4) == 0
    print('PASS: exact profile, compiled ARM trampoline, native last-texture release, both destructor wrappers and dispatch to native policy D0')


if __name__ == '__main__':
    main()
