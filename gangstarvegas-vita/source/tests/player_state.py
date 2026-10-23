"""Execute the linked read-only player probe and original singleton getter.

The native position getter is the physics/scene boundary supplied by this fixture;
its hidden vector output ABI and diagnostic ARM varargs are checked explicitly.
This does not simulate or prove native collision registration.
"""
import hashlib
import math
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import *

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'source/tests'))
from texture_policy import load, store, word, GAME, BASE, LIBRARIES
from game_profile import PLAYER_STATE as PROFILE, PHYSICS_STATE as PHYSICS

PLAYER, NODE, STACK, RETURN = 0x100000, 0x103000, 0x208000, 0x300000


def machine():
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    symbols = load(cpu, ROOT / 'build/gangstarvegas_vita')
    load(cpu, GAME, BASE)
    for address, size in ((PLAYER, 16384), (STACK - 32768, 36864), (RETURN, 4096)):
        cpu.mem_map(address, size)
    cpu.reg_write(UC_ARM_REG_C1_C0_2, 0xf << 20)
    cpu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    store(cpu, symbols['so_mod'] + 44, BASE)
    getter = BASE + PROFILE['get_player']
    assert word(cpu, getter) == PROFILE['get_player_word0']
    assert word(cpu, getter + 4) == PROFILE['get_player_word1']
    # ldr r0,[pc,#4]; ldr r0,[pc,r0] addresses the original relocated singleton.
    singleton = getter + 12 + word(cpu, getter + 12)
    assert singleton == BASE + 0x1a5ac28
    store(cpu, singleton, 0)
    records, calls = [], []
    state = {'xyz': (10.0, 20.0, 30.0), 'bad_return': False, 'physics_logs': [], 'query_calls': [], 'body': 0, 'vehicle': 0}

    def boundary(uc, address, size, unused):
        if address == BASE + PROFILE['get_position']:
            output, player = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1))
            assert player == PLAYER and STACK - 512 <= output < STACK
            calls.append(player)
            uc.mem_write(output, struct.pack('<3f', *state['xyz']))
            uc.reg_write(UC_ARM_REG_R0, output + 4 if state['bad_return'] else output)
        elif address == BASE+PROFILE['in_vehicle']:
            assert uc.reg_read(UC_ARM_REG_R0)==PLAYER
            uc.reg_write(UC_ARM_REG_R0,state['vehicle'])
        elif address == BASE+PHYSICS['functions']['get_hkp_body'][0]:
            world,body=(uc.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1))
            state['query_calls'].append(('body',world,body))
            uc.reg_write(UC_ARM_REG_R0,state['body'])
        elif address == BASE+PHYSICS['functions']['streamer_done'][0]:
            state['query_calls'].append(('done',uc.reg_read(UC_ARM_REG_R0)))
            uc.reg_write(UC_ARM_REG_R0,1)
        elif address == BASE+PHYSICS['functions']['streamer_district_loaded'][0]:
            streamer,vector=(uc.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1))
            xyz=struct.unpack('<3f',bytes(uc.mem_read(vector,12)))
            state['query_calls'].append(('district',streamer,xyz))
            uc.reg_write(UC_ARM_REG_R0,int(xyz[2]>=0))
        elif address == symbols['diagnostics_log'] & ~1:
            fmt = uc.reg_read(UC_ARM_REG_R1)
            text = bytes(uc.mem_read(fmt, 400)).split(b'\0', 1)[0].decode()
            record = {'format': text, 'frame': uc.reg_read(UC_ARM_REG_R2)}
            if text.startswith('Player probe:') and 'xyz=' in text:
                assert uc.reg_read(UC_ARM_REG_R3) == PLAYER
                stack = bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_SP), 68))
                record.update(xyz=struct.unpack_from('<3d', stack),
                              finite=struct.unpack_from('<I', stack, 24)[0],
                              dz=struct.unpack_from('<d', stack, 32)[0])
                fields = struct.unpack_from('<7I', stack, 40)
                record.update(zip(('previous', 'hide', 'node', 'node_flags', 'fall', 'physics', 'ragdoll'), fields))
            if text.startswith('Physics '):
                if 'havok=' in text:
                    record['world']=uc.reg_read(UC_ARM_REG_R3)
                    values=struct.unpack('<15I',bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_SP),60)))
                    record.update(zip(('havok','streamer','grid','bodies','fixed','done','district','anchor_district',
                                       'physics','body','body_world','shape','motion','member','vehicle'),values))
                state['physics_logs'].append(record)
            else:records.append(record)
        else:
            return
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    cpu.hook_add(UC_HOOK_CODE, boundary)

    def call(name, argument=0):
        cpu.reg_write(UC_ARM_REG_CPSR, 0x30 if symbols[name] & 1 else 0x10)
        cpu.reg_write(UC_ARM_REG_SP, STACK)
        cpu.reg_write(UC_ARM_REG_LR, RETURN)
        cpu.reg_write(UC_ARM_REG_R0, argument)
        cpu.emu_start(symbols[name], RETURN, count=10000)
        assert cpu.reg_read(UC_ARM_REG_PC) == RETURN
        assert cpu.reg_read(UC_ARM_REG_SP) == STACK
        return cpu.reg_read(UC_ARM_REG_R0)

    return cpu, symbols, singleton, state, records, calls, call


def main():
    assert hashlib.sha256(GAME.read_bytes()).hexdigest() == LIBRARIES['lib/armeabi-v7a/libGangstar4.so']['sha256']
    cpu, symbols, singleton, state, records, calls, call = machine()
    assert call('vegas_bind_player_probe') == 0
    # Prolonged menu time must not exhaust the gameplay sample budget.
    call('vegas_trace_player_state', 1)
    for frame in range(300, 18001, 300):
        call('vegas_trace_player_state', frame)
    assert len(records) == 1 and 'absent' in records[0]['format'] and not calls
    store(cpu, singleton, PLAYER)
    call('vegas_trace_player_state', 18300)
    assert 'unknown_vptr' in records[-1]['format'] and not calls
    store(cpu, PLAYER, BASE + PROFILE['vptr'])
    call('vegas_trace_player_state', 18600)
    assert 'no_node' in records[-1]['format'] and not calls
    store(cpu, PLAYER + PROFILE['node'], NODE)
    for offset, value in [('flags', 0x400), ('physics_reference', 0x1234), ('ragdoll', 0x5678), ('fall_state', 7)]:
        store(cpu, PLAYER + PROFILE[offset], value)
    store(cpu, NODE + PROFILE['node_flags'], 0x18)
    before = bytes(cpu.mem_read(PLAYER, 16384))
    call('vegas_trace_player_state', 18900)
    first = records[-1]
    assert first['xyz'] == (10, 20, 30) and first['finite'] == 1
    assert first['previous'] == 0 and first['dz'] == 0
    assert (first['hide'], first['node'], first['node_flags'], first['fall'], first['physics'], first['ragdoll']) == (0x400, NODE, 0x18, 7, 0x1234, 0x5678)
    state['xyz'] = (10, 20, -70)
    call('vegas_trace_player_state', 19200)
    assert records[-1]['dz'] == -100 and records[-1]['previous'] == 1
    for value in (math.nan, math.inf):
        state['xyz'] = (value, 20, 30)
        call('vegas_trace_player_state', 19500)
        assert records[-1]['finite'] == 0 and records[-1]['previous'] == 0
    state['bad_return'] = True
    call('vegas_trace_player_state', 19800)
    assert 'native position result=' in records[-1]['format']
    state.update(xyz=(10, 20, 30), bad_return=False)
    call('vegas_trace_player_state', 20100)
    assert records[-1]['previous'] == 0
    for frame in range(20400, 40000, 300):
        call('vegas_trace_player_state', frame)
    assert len(calls) == 32
    assert bytes(cpu.mem_read(PLAYER, 16384)) == before
    # Each guarded opcode and virtual slot must reject a mismatched profile.
    for location in (PROFILE['get_player'], PROFILE['get_player'] + 4,
                     PROFILE['get_position'], PROFILE['get_position'] + 4, PROFILE['vptr'] + 0x34):
        cpu, symbols, singleton, state, records, calls, call = machine()
        store(cpu, BASE + location, 0)
        assert call('vegas_bind_player_probe') == 0xffffffff and not calls
        assert 'profile mismatch' in records[-1]['format']
    print('PASS: linked ARM player probe, original singleton, vector output ABI, Z delta, visibility/raw fields, finite/return guards, menu-safe bounded samples and profile rejection')


if __name__ == '__main__':
    main()
