"""Execute the linked statfs adapter and original ARM storage consumers.

Only Vita OS boundaries are supplied by the fixture: device info, kernel memset,
and errno storage. Layout conversion and the game consumers execute ARM code.
"""
import hashlib
import importlib.util
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_R4, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
    UC_ARM_REG_CPSR, UC_CPU_ARM_CORTEX_A9,
)

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('texture_fixture', ROOT / 'source/tests/texture_policy.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
ELF = ROOT / 'build/gangstarvegas_vita'
OBJECT, PATH, ERRNO, STACK, RETURN = 0x100000, 0x100200, 0x100300, 0x200800, 0x300000


def word(cpu, address):
    return struct.unpack('<I', cpu.mem_read(address, 4))[0]


def machine(free_bytes, error=0):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    symbols = fixture.load(cpu, ELF)
    fixture.load(cpu, fixture.GAME, fixture.BASE)
    for address in (OBJECT, STACK & ~4095, RETURN):
        cpu.mem_map(address, 4096)
    cpu.reg_write(UC_ARM_REG_CPSR, 0x10)
    cpu.reg_write(UC_ARM_REG_SP, STACK)
    cpu.reg_write(UC_ARM_REG_LR, RETURN)
    cpu.mem_write(PATH, b'ux0:data/gangstarvegas/files\0')
    cpu.mem_write(OBJECT, bytes([0xa5]) * 96)
    calls = []
    boundaries = {symbols[n] & ~1: n for n in ('sceIoDevctl', 'memset', '__errno')}

    def boundary(cpu, address, size, unused):
        name = boundaries.get(address)
        if not name:
            return
        r0, r1, r2, r3 = [cpu.reg_read(r) for r in
                           (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if name == 'sceIoDevctl':
            assert bytes(cpu.mem_read(r0, 5)) == b'ux0:\0'
            assert (r1, r2, r3) == (0x3001, 0, 0)
            sp = cpu.reg_read(UC_ARM_REG_SP)
            out, length = word(cpu, sp), word(cpu, sp + 4)
            assert length == 24
            calls.append(name)
            if not error:
                cpu.mem_write(out, struct.pack('<qqII', max(free_bytes, 256 * 1024**3), free_bytes, 4096, 0))
            cpu.reg_write(UC_ARM_REG_R0, error & 0xffffffff)
        elif name == 'memset':
            cpu.mem_write(r0, bytes([r1 & 255]) * r2)
        else:
            cpu.reg_write(UC_ARM_REG_R0, ERRNO)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    cpu.hook_add(UC_HOOK_CODE, boundary)
    return cpu, symbols, calls


def adapter(cpu, symbols, path=PATH, output=OBJECT):
    cpu.reg_write(UC_ARM_REG_R0, path)
    cpu.reg_write(UC_ARM_REG_R1, output)
    cpu.emu_start(symbols['android_statfs'], RETURN, count=1000)
    assert cpu.reg_read(UC_ARM_REG_PC) == RETURN
    return cpu.reg_read(UC_ARM_REG_R0)


def consumer(cpu, layout, start, stop, offset):
    cpu.reg_write(UC_ARM_REG_CPSR, 0x10)
    cpu.reg_write(UC_ARM_REG_SP, STACK)
    cpu.reg_write(UC_ARM_REG_R4, ERRNO)
    cpu.mem_write(ERRNO, bytes(4))
    cpu.mem_write(STACK + 1124, bytes(4))
    cpu.mem_write(STACK + offset, layout)
    cpu.emu_start(fixture.BASE + start, fixture.BASE + stop, count=30)
    assert cpu.reg_read(UC_ARM_REG_PC) == fixture.BASE + stop
    return cpu.reg_read(UC_ARM_REG_R0) | (cpu.reg_read(UC_ARM_REG_R1) << 32)


def main():
    assert hashlib.sha256(fixture.GAME.read_bytes()).hexdigest() == fixture.LIBRARIES['lib/armeabi-v7a/libGangstar4.so']['sha256']
    for free_bytes in (0, 512 * 1024**2, 144 * 1024**3 + 123, (0x100000001 * 4096)):
        cpu, symbols, calls = machine(free_bytes)
        assert adapter(cpu, symbols) == 0 and calls == ['sceIoDevctl']
        layout = bytes(cpu.mem_read(OBJECT, 88))
        expected = free_bytes // 4096 * 4096
        assert struct.unpack_from('<I', layout, 4)[0] == 4096
        assert struct.unpack_from('<Q', layout, 24)[0] == free_bytes // 4096
        assert struct.unpack_from('<Q', layout, 16)[0] == free_bytes // 4096
        assert struct.unpack_from('<QQ', layout, 32) == (0, 0)
        assert struct.unpack_from('<III', layout, 56) == (255, 4096, 0)
        assert bytes(cpu.mem_read(OBJECT + 88, 8)) == bytes([0xa5]) * 8
        actual = consumer(cpu, layout, 0x494c38, 0x494c58, 8)
        megabytes = consumer(cpu, layout, 0x10cff58, 0x10cff78, 64)
        assert actual == expected and megabytes == expected >> 20
        print('PASS storage bytes=', free_bytes, 'native bytes=', actual, 'native MiB=', megabytes)
    cpu, symbols, calls = machine(0, -0x1234)
    assert adapter(cpu, symbols) == 0xffffffff
    assert word(cpu, ERRNO) == 5 and calls == ['sceIoDevctl']
    assert bytes(cpu.mem_read(OBJECT, 96)) == bytes([0xa5]) * 96
    for path, output in ((0, OBJECT), (PATH, 0)):
        cpu, symbols, calls = machine(0)
        assert adapter(cpu, symbols, path, output) == 0xffffffff
        assert word(cpu, ERRNO) == 14 and not calls
    print('PASS device error and null arguments preserve output; ELF SHA256=', hashlib.sha256(ELF.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
