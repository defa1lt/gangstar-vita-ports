"""Run linked Vita controller ARM code and inspect calls at real Vegas JNI entries."""
from pathlib import Path
import hashlib
import re
import struct
import subprocess
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import *

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'source/tests'))
from texture_policy import load, word, store
sys.path.insert(0, str(ROOT / 'tools'))
from game_profile import LIBRARIES

SDK = Path('/Volumes/USB/persanal/psvita/.vitasdk-softfp')
ELF = ROOT / 'build/gangstarvegas_vita'
GAME = ROOT / 'refs/android/apk/lib/armeabi-v7a/libGangstar4.so'
BASE, STACK, GAME_CLASS = 0x98000000, 0x200800, 0x42424242
ENTRY = {'down': BASE + 0x45fbe8, 'up': BASE + 0x45fbf4,
         'joy': BASE + 0x45fc00}
KEYS = [(0x4000, 96), (0x2000, 97), (0x8000, 99), (0x1000, 100),
        (0x100, 102), (0x200, 103), (8, 108), (1, 109),
        (0x10, 19), (0x40, 20), (0x80, 21), (0x20, 22)]


def signed(value):
    return value if value < 0x80000000 else value - 0x100000000


def main():
    assert hashlib.sha256(GAME.read_bytes()).hexdigest() == LIBRARIES['lib/armeabi-v7a/libGangstar4.so']['sha256']
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    symbols = load(cpu, ELF)
    load(cpu, GAME, BASE)
    cpu.mem_map(STACK & ~4095, 4096)
    # The compiler inlines poll_controller. Discover its actual linked block;
    # use neither a rewritten C function nor a separately built test variant.
    text = subprocess.check_output([
        str(SDK / 'bin/arm-vita-eabi-objdump'), '-d',
        '--disassemble=vegas_java_start', str(ELF)], text=True)
    instructions = []
    for line in text.splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{4,8}\s+)+\s*([a-z][a-z0-9.]*)\s*(.*)', line)
        if match:
            instructions.append((int(match[1], 16), match[2].split('.')[0], match[3]))
    peek_at = next(i for i, (_, op, args) in enumerate(instructions)
                   if op.startswith('bl') and '<sceCtrlPeekBufferPositive>' in args)
    start = instructions[peek_at - 3][0]
    assert instructions[peek_at - 3][1:] == ('movs', 'r2, #1')
    assert instructions[peek_at - 2][1] == 'add'
    assert instructions[peek_at - 1][1:] == ('movs', 'r0, #0')
    assert instructions[peek_at + 2][1].startswith('ble')
    stop = int(instructions[peek_at + 2][2].split()[0], 16)
    body = instructions[peek_at + 3:]
    # These live anchors are retained from the surrounding Java frame loop.
    buttons = next(args for _, op, args in body if op == 'ldr' and args.startswith('r3, [r7,'))
    buttons_offset = int(re.search(r'#(\d+)', buttons)[1])
    axes = next(args for _, op, args in body if op.startswith('ldr') and args.startswith('r3, [sl,'))
    axes_offset = int(re.search(r'#(\d+)', axes)[1])
    previous_buttons = next(value for name, value in symbols.items() if name.startswith('previous_buttons.'))
    previous_axes = next(value for name, value in symbols.items() if name.startswith('previous_axes.'))
    anchor7 = previous_buttons - buttons_offset
    anchor10 = previous_axes - axes_offset
    down = next(args for _, op, args in body if op == 'ldrne' and args.startswith('r5, [r7,'))
    up = next(args for _, op, args in body if op == 'ldreq' and args.startswith('r5, [r7,'))
    joy = next(args for _, op, args in body if op == 'ldr' and args.startswith('r6, [r7,'))
    for name, args in (('key_down', down), ('key_up', up), ('joy_move', joy)):
        assert symbols[name] == anchor7 + int(re.search(r'#(\d+)', args)[1])
    assert word(cpu, ENTRY['down']) == 0xe1a00002
    assert word(cpu, ENTRY['up']) == 0xe1a00002
    assert word(cpu, ENTRY['joy']) == 0xe1a00002
    for name, kind in [('key_down', 'down'), ('key_up', 'up'), ('joy_move', 'joy')]:
        store(cpu, symbols[name], ENTRY[kind])
    sample = {'buttons': 0, 'axes': (128, 128, 128, 128), 'result': 1}
    events = []

    def intercept(uc, address, size, data):
        if address == symbols['sceCtrlPeekBufferPositive']:
            assert uc.reg_read(UC_ARM_REG_R0) == 0
            assert uc.reg_read(UC_ARM_REG_R2) == 1
            target = uc.reg_read(UC_ARM_REG_R1)
            # SceCtrlData from installed SDK psp2common/ctrl.h: 0x20 bytes,
            # timestamp +0, buttons +8, unsigned lx/ly/rx/ry +12..15.
            if sample['result'] > 0:
                uc.mem_write(target, struct.pack('<QI4B16x', 123, sample['buttons'], *sample['axes']))
            uc.reg_write(UC_ARM_REG_R0, sample['result'] & 0xffffffff)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        elif address in ENTRY.values():
            assert uc.reg_read(UC_ARM_REG_R0) == symbols['jni']
            assert uc.reg_read(UC_ARM_REG_R1) == GAME_CLASS
            kind = next(k for k, value in ENTRY.items() if value == address)
            r2 = signed(uc.reg_read(UC_ARM_REG_R2))
            r3 = signed(uc.reg_read(UC_ARM_REG_R3))
            if kind == 'joy':
                sp = uc.reg_read(UC_ARM_REG_SP)
                events.append((kind, r2, r3, word(uc, sp), word(uc, sp + 4)))
            else:
                events.append((kind, r2, r3))
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    cpu.hook_add(UC_HOOK_CODE, intercept)

    def poll(buttons=0, axes=(128,128,128,128), result=1):
        sample.update(buttons=buttons, axes=axes, result=result)
        events.clear()
        cpu.reg_write(UC_ARM_REG_CPSR, 0x30)
        cpu.reg_write(UC_ARM_REG_SP, STACK)
        cpu.reg_write(UC_ARM_REG_R7, anchor7)
        cpu.reg_write(UC_ARM_REG_R10, anchor10)
        store(cpu, STACK + 16, GAME_CLASS)
        store(cpu, STACK + 20, symbols['jni'])
        cpu.emu_start(start | 1, stop, count=10000)
        assert cpu.reg_read(UC_ARM_REG_PC) == stop, hex(cpu.reg_read(UC_ARM_REG_PC))
        return list(events)

    assert poll() == [('joy',0,0,0,0), ('joy',0,0,0,1)]
    assert poll() == []
    for button, key in KEYS:
        assert poll(button) == [('down',key,1)], (button, key, events)
        assert poll(button) == []
        assert poll() == [('up',key,1)]
    assert poll(axes=(0,255,255,0)) == [('joy',-255,-255,0,0),('joy',255,-255,0,1)]
    assert poll(axes=(255,0,0,255)) == [('joy',255,255,0,0),('joy',-255,255,0,1)]
    assert poll() == [('joy',0,0,0,0),('joy',0,0,0,1)]
    assert poll(axes=(255,128,128,255)) == [('joy',255,0,0,0),('joy',0,255,0,1)]
    assert poll(axes=(255,128,128,255)) == []
    assert poll(axes=(143,113,143,113)) == [('joy',0,0,0,0),('joy',0,0,0,1)]
    old_buttons = cpu.mem_read(previous_buttons,4)
    old_axes = cpu.mem_read(previous_axes,16)
    assert poll(0x4000, (255,0,0,255), result=-1) == []
    assert cpu.mem_read(previous_buttons,4) == old_buttons
    assert cpu.mem_read(previous_axes,16) == old_axes
    assert poll(0x4000, (255,0,0,255), result=0) == []
    assert cpu.mem_read(previous_buttons,4) == old_buttons
    assert cpu.mem_read(previous_axes,16) == old_axes
    assert poll(0x4000, (255,0,0,255)) == [('down',96,1),('joy',255,255,0,0),('joy',-255,255,0,1)]
    print('PASS: linked ARM controller block, native JNI ABI, 12 key mappings, both sticks/signs/extrema, single-axis/center/deadzone, unchanged input, failed reads preserve state')

if __name__ == '__main__':
    main()
