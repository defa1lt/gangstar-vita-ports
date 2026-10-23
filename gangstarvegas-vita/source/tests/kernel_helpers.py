"""Execute the built ARM kuser handlers and validate the exact game profile.

Run with Unicorn installed, after compiling source/compat.c for Vita.
"""
import hashlib
from pathlib import Path
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import (
    UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_SP, UC_ARM_REG_R0,
    UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
    UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
    UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12,
    UC_CPU_ARM_CORTEX_A9,
)

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from game_profile import KERNEL_HELPERS, LIBRARIES


def functions(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 40:
        raise ValueError('Expected little-endian ARM ELF32 object')
    offset = struct.unpack_from('<I', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 46)
    sections = [struct.unpack_from('<10I', data, offset + i * stride) for i in range(count)]
    result = {}
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            name, value, size, info, _, index = struct.unpack_from('<IIIBBH', data, at)
            if info & 15 != 2 or not size or not index:
                continue
            label = names[name:names.index(0, name)].decode()
            if label not in ('kernel_memory_barrier', 'kernel_compare_exchange'):
                continue
            assert not value & 1, 'Handlers must use ARM state'
            start = sections[index][4] + value
            result[label] = data[start:start + size]
    assert len(result) == 2, result.keys()
    return result


REGISTERS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
             UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
             UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
             UC_ARM_REG_R12]


def machine(code):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    cpu.mem_map(0x10000, 4096)
    cpu.mem_map(0x20000, 4096)
    cpu.mem_write(0x10000, code)
    cpu.reg_write(UC_ARM_REG_CPSR, 0x10)
    for i, register in enumerate(REGISTERS):
        cpu.reg_write(register, 0x12340000 + i)
    cpu.reg_write(UC_ARM_REG_SP, 0x20800)
    cpu.reg_write(UC_ARM_REG_LR, 0x10800)
    return cpu


def main():
    image = (ROOT / 'refs/android/apk/lib/armeabi-v7a/libGangstar4.so').read_bytes()
    assert hashlib.sha256(image).hexdigest() == LIBRARIES['lib/armeabi-v7a/libGangstar4.so']['sha256']
    slots = set()
    for address, offsets in KERNEL_HELPERS.items():
        for offset in offsets:
            assert offset not in slots and offset % 4 == 0
            assert struct.unpack_from('<I', image, offset)[0] == address
            slots.add(offset)
    assert len(slots) == 53
    code = functions(ROOT / 'build/CMakeFiles/gangstarvegas_vita.dir/source/compat.c.obj')
    for flags in (0, 0xf0000000, 0xa0000000, 0x50000000):
        cpu = machine(code['kernel_memory_barrier'])
        cpu.reg_write(UC_ARM_REG_CPSR, flags | 0x10)
        before = [cpu.reg_read(r) for r in REGISTERS]
        cpu.emu_start(0x10000, 0x10800, count=100)
        assert [cpu.reg_read(r) for r in REGISTERS] == before
        assert cpu.reg_read(UC_ARM_REG_CPSR) & 0xf0000000 == flags
    for actual, expected, replacement in ((7, 7, 9), (7, 8, 9),
                                          (0xffffffff, 0xffffffff, 0), (0, 0, 0xffffffff)):
        cpu = machine(code['kernel_compare_exchange'])
        cpu.mem_write(0x20000, struct.pack('<I', actual))
        cpu.reg_write(UC_ARM_REG_R0, expected)
        cpu.reg_write(UC_ARM_REG_R1, replacement)
        cpu.reg_write(UC_ARM_REG_R2, 0x20000)
        preserved = [UC_ARM_REG_R1, UC_ARM_REG_R2, *REGISTERS[4:12], UC_ARM_REG_SP]
        before = [cpu.reg_read(r) for r in preserved]
        cpu.emu_start(0x10000, 0x10800, count=100)
        success = actual == expected
        assert (cpu.reg_read(UC_ARM_REG_R0) == 0) == success
        assert bool(cpu.reg_read(UC_ARM_REG_CPSR) & 0x20000000) == success
        assert struct.unpack('<I', cpu.mem_read(0x20000, 4))[0] == (replacement if success else actual)
        assert [cpu.reg_read(r) for r in preserved] == before
    print('PASS: 53 profiled literals; actual ARM barrier/CAS code, return values, Carry and register preservation')


if __name__ == '__main__':
    main()
