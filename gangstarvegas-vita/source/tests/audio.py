"""Execute linked ARM AudioTrack methods with bounded Vita OS fixtures.

Port I/O, allocator, libc byte copies, mutexes and diagnostics are environment
boundaries. Constructor validation, varargs, PCM accumulation and states execute
the actual SDK-compiled production code. This does not prove audible output.
"""
import hashlib
import importlib.util
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR,
    UC_CPU_ARM_CORTEX_A9,
)

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('texture_fixture', ROOT / 'source/tests/texture_policy.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
ELF = ROOT / 'build/gangstarvegas_vita'
ARRAY, ARGS, STACK, RETURN, HEAP, DATA = 0x100100, 0x100200, 0x200800, 0x300000, 0x400000, 0x500000
CLASS, NAME, SIGNATURE = 0x100400, 0x100480, 0x100500
CREATE, RATE, BUFFER, WRITE, STATE, PLAY, PAUSE, STOP, RELEASE = range(1000, 1009)
CONFIG = (3, 44100, 12, 2, 4096, 1)
GRAIN_BYTES = 4096


def signed(value):
    return struct.unpack('<i', struct.pack('<I', value))[0]


class Machine:
    def __init__(self):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
        self.symbols = fixture.load(self.cpu, ELF)
        for address, size in ((ARRAY & ~4095, 4096), (STACK & ~4095, 4096),
                              (RETURN, 4096), (HEAP, 65536), (DATA, 131072)):
            self.cpu.mem_map(address, size)
        self.next_heap = HEAP
        self.malloc_fail = False
        self.open_error = 0
        self.next_port = 7
        self.opened = []
        self.released = []
        self.freed = []
        self.outputs = []
        self.attempts = []
        self.output_results = []
        self.drains = 0
        self.locked = False
        names = ('sceAudioOutOpenPort', 'sceAudioOutOutput', 'sceAudioOutReleasePort',
                 'malloc', 'free', 'memcpy', 'memset', 'pthread_mutex_lock',
                 'pthread_mutex_unlock', 'diagnostics_log', 'strcmp', 'snprintf',
                 'strlen', '_fjni_log_info', '_fjni_log_debug', '_fjni_log_warn', '_fjni_log_error')
        self.boundaries = {self.symbols[n] & ~1: n for n in names}
        self.cpu.hook_add(UC_HOOK_CODE, self.boundary)

    def boundary(self, cpu, address, size, unused):
        name = self.boundaries.get(address)
        if not name:
            return
        r0, r1, r2, r3 = [cpu.reg_read(r) for r in
                           (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        result = 0
        if name == 'sceAudioOutOpenPort':
            assert self.locked and (r0, r1, r2, r3) == (1, 1024, 44100, 1)
            if self.open_error:
                result = self.open_error
            else:
                result = self.next_port
                self.next_port += 1
                self.opened.append(result)
        elif name == 'sceAudioOutOutput':
            assert self.locked and r0 in self.opened and r0 not in self.released
            if not r1:
                self.drains += 1
            else:
                data = bytes(cpu.mem_read(r1, GRAIN_BYTES))
                self.attempts.append(data)
                result = self.output_results.pop(0) if self.output_results else 0
                if result >= 0:
                    self.outputs.append(data)
        elif name == 'sceAudioOutReleasePort':
            assert self.locked and r0 in self.opened and r0 not in self.released
            self.released.append(r0)
        elif name == 'malloc':
            assert r0 == 4
            if not self.malloc_fail:
                result = self.next_heap
                self.next_heap += 16
        elif name == 'free':
            assert HEAP <= r0 < self.next_heap and r0 not in self.freed
            self.freed.append(r0)
        elif name == 'memcpy':
            cpu.mem_write(r0, bytes(cpu.mem_read(r1, r2)))
            result = r0
        elif name == 'memset':
            cpu.mem_write(r0, bytes([r1 & 255]) * r2)
            result = r0
        elif name == 'strlen':
            result = len(self.cstring(r0))
        elif name == 'strcmp':
            left, right = self.cstring(r0), self.cstring(r1)
            result = (left > right) - (left < right)
        elif name == 'snprintf':
            fmt = self.cstring(r2).decode()
            assert fmt in ('%s', '%s/%s', '%s/%s%s', '%s/%s%s%s'), fmt
            sp = cpu.reg_read(UC_ARM_REG_SP)
            pointers = [r3] + [struct.unpack('<I', cpu.mem_read(sp + 4 * i, 4))[0] for i in range(3)]
            values = tuple(self.cstring(p).decode() for p in pointers[:fmt.count('%s')])
            rendered = (fmt % values).encode()
            if r1:
                cpu.mem_write(r0, rendered[:r1 - 1] + b'\0')
            result = len(rendered)
        elif name == 'pthread_mutex_lock':
            assert not self.locked
            self.locked = True
        elif name == 'pthread_mutex_unlock':
            assert self.locked
            self.locked = False
        cpu.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def cstring(self, address):
        result = bytearray()
        for i in range(1024):
            byte = self.cpu.mem_read(address + i, 1)[0]
            if not byte:
                return bytes(result)
            result.append(byte)
        raise AssertionError('Unterminated fixture string')

    def native_call(self, method, arguments):
        registers = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
        self.cpu.reg_write(UC_ARM_REG_CPSR, 0x10)
        self.cpu.reg_write(UC_ARM_REG_SP, STACK)
        self.cpu.reg_write(UC_ARM_REG_LR, RETURN)
        for register, value in zip(registers, arguments):
            self.cpu.reg_write(register, value & 0xffffffff)
        for i, value in enumerate(arguments[4:]):
            self.cpu.mem_write(STACK + i * 4, struct.pack('<I', value & 0xffffffff))
        self.cpu.emu_start(self.symbols[method], RETURN, count=200000)
        assert self.cpu.reg_read(UC_ARM_REG_PC) == RETURN
        assert not self.locked
        return self.cpu.reg_read(UC_ARM_REG_R0)

    def lookup(self, name, signature, static=False):
        for address, value in ((CLASS, 'android/media/AudioTrack'), (NAME, name), (SIGNATURE, signature)):
            self.cpu.mem_write(address, value.encode() + b'\0')
        return self.native_call('GetStaticMethodID' if static else 'GetMethodID', (0, CLASS, NAME, SIGNATURE))

    def call(self, method, identifier, *arguments):
        self.cpu.mem_write(ARGS, struct.pack('<' + 'I' * len(arguments),
                                           *(v & 0xffffffff for v in arguments)) if arguments else bytes(4))
        self.cpu.reg_write(UC_ARM_REG_CPSR, 0x10)
        self.cpu.reg_write(UC_ARM_REG_SP, STACK)
        self.cpu.reg_write(UC_ARM_REG_LR, RETURN)
        self.cpu.reg_write(UC_ARM_REG_R0, identifier)
        self.cpu.reg_write(UC_ARM_REG_R1, ARGS)
        self.cpu.emu_start(self.symbols[method], RETURN, count=200000)
        assert self.cpu.reg_read(UC_ARM_REG_PC) == RETURN
        assert not self.locked
        return self.cpu.reg_read(UC_ARM_REG_R0)

    def create(self, config=CONFIG):
        return self.call('vegas_audio_create', CREATE, *config)

    def integer(self, identifier, *args):
        return signed(self.call('vegas_audio_integer', identifier, *args))

    def void(self, identifier):
        self.call('vegas_audio_void', identifier)

    def array(self, payload, magic=0xB00B1E5, kind=3, length=None, pointer=DATA):
        assert len(payload) <= 131072
        if payload:
            self.cpu.mem_write(DATA, payload)
        self.cpu.mem_write(ARRAY, struct.pack('<IIiI', magic, pointer,
                                           len(payload) if length is None else length, kind))
        return ARRAY

    def write(self, payload, offset=0, size=None, **array_args):
        array = self.array(payload, **array_args)
        return self.integer(WRITE, array, offset, len(payload) - offset if size is None else size)


def configured():
    m = Machine()
    token = m.create()
    assert token and struct.unpack('<I', m.cpu.mem_read(token, 4))[0] == 0x4154524b
    assert struct.unpack('<I', m.cpu.mem_read(token, 4))[0] != 0xB00B1E5
    assert m.integer(STATE) == 1
    m.void(PLAY)
    assert m.integer(STATE) == 3
    return m


def main():
    m = Machine()
    bindings = [('<init>', '(IIIIII)V', CREATE, False),
                ('getNativeOutputSampleRate', '(I)I', RATE, True),
                ('getMinBufferSize', '(III)I', BUFFER, True),
                ('write', '([BII)I', WRITE, False), ('getPlayState', '()I', STATE, False),
                ('play', '()V', PLAY, False), ('pause', '()V', PAUSE, False),
                ('stop', '()V', STOP, False), ('release', '()V', RELEASE, False)]
    for name, signature, identifier, static in bindings:
        assert m.lookup(name, signature, static) == identifier, (name, signature)
    assert m.lookup('write', '([SII)I') == 0
    m.cpu.mem_write(NAME, b'getPackage\0')
    m.cpu.mem_write(SIGNATURE, b'()Ljava/lang/String;\0')
    assert m.native_call('GetMethodID', (0, 0x42424242, NAME, SIGNATURE)) == 5
    assert m.native_call('GetStaticMethodID', (0, 0x69696969, NAME, SIGNATURE)) == 5
    token = m.native_call('NewObject', (0, CLASS, CREATE, *CONFIG))
    assert token
    assert m.native_call('NewGlobalRef', (0, token)) == token
    m.native_call('DeleteLocalRef', (0, token))
    assert token not in m.freed
    assert m.native_call('CallStaticIntMethod', (0, CLASS, RATE, 3)) == 44100
    assert m.native_call('CallStaticIntMethod', (0, CLASS, BUFFER, 44100, 12, 2)) == GRAIN_BYTES
    m.native_call('CallNonvirtualVoidMethod', (0, token, CLASS, PLAY))
    assert m.native_call('CallNonvirtualIntMethod', (0, token, CLASS, STATE)) == 3
    payload = bytes([0x5a]) * GRAIN_BYTES
    array = m.array(payload)
    assert m.native_call('CallNonvirtualIntMethod', (0, token, CLASS, WRITE, array, 0, len(payload))) == len(payload)
    assert m.outputs == [payload]
    for method, state in ((PAUSE, 2), (STOP, 1), (RELEASE, 1)):
        m.native_call('CallNonvirtualVoidMethod', (0, token, CLASS, method))
        assert m.native_call('CallNonvirtualIntMethod', (0, token, CLASS, STATE)) == state
    assert m.released == [7]
    m.native_call('DeleteGlobalRef', (0, token))
    assert m.freed == [token]
    print('PASS all nine actual JNI lookups, constructor/static calls and nonvirtual write/lifecycle dispatch')

    m = Machine()
    assert m.integer(RATE, 3) == 44100
    assert m.integer(BUFFER, 44100, 12, 2) == GRAIN_BYTES
    for args in ((22050, 12, 2), (44100, 4, 2), (44100, 12, 3)):
        assert m.integer(BUFFER, *args) == -2
    for index, value in ((0, 2), (1, 22050), (2, 4), (3, 3), (4, 4092), (4, 4097), (5, 0)):
        config = list(CONFIG)
        config[index] = value
        assert m.create(config) == 0
    assert not m.opened and m.next_heap == HEAP
    m.malloc_fail = True
    assert m.create() == 0 and not m.opened
    m.malloc_fail = False
    m.open_error = -5
    assert m.create() == 0 and len(m.freed) == 1
    m.open_error = 0
    assert m.create()
    assert m.create() == 0 and len(m.opened) == 1 and len(m.freed) == 2
    print('PASS constructor/configuration/token ownership and port failures')

    m = configured()
    first, second = bytes([0x11]) * 512, bytes([0x22]) * 3584
    assert m.write(first) == 512 and not m.outputs
    assert m.write(second) == 3584 and m.outputs == [first + second]
    multiple = b''.join(bytes([v]) * GRAIN_BYTES for v in (0x33, 0x44, 0x55))
    assert m.write(multiple) == len(multiple) and m.outputs[1:] == [multiple[i:i + GRAIN_BYTES] for i in range(0, len(multiple), GRAIN_BYTES)]
    m.void(PAUSE)
    assert m.integer(STATE) == 2 and m.write(bytes(4)) == 0
    m.void(PLAY)
    assert m.write(first) == 512
    m.void(STOP)
    assert m.integer(STATE) == 1
    m.void(PLAY)
    fresh = bytes([0x66]) * GRAIN_BYTES
    assert m.write(fresh) == GRAIN_BYTES and m.outputs[-1] == fresh
    m.void(RELEASE)
    assert m.integer(STATE) == 1 and m.write(bytes(4)) == -3
    assert m.released == [7] and m.drains == 1
    assert m.create() and m.opened == [7, 8]
    print('PASS exact partial/multiple grains, pause/stop/release and recreation')

    m = configured()
    for options in ({'magic': 0}, {'kind': 7}, {'length': -1}, {'pointer': 0},
                    {'offset': -4}, {'size': -4}, {'offset': 4100},
                    {'offset': 4092, 'size': 8}, {'offset': 1, 'size': 4}, {'size': 2}):
        assert m.write(bytes(4096), **options) == -2, options
    assert m.integer(WRITE, 0, 0, 4) == -2
    assert m.write(b'') == 0 and not m.outputs
    assert m.write(b'abcd' + bytes([0x77]) * GRAIN_BYTES, offset=4, size=GRAIN_BYTES) == GRAIN_BYTES
    assert m.outputs == [bytes([0x77]) * GRAIN_BYTES]
    print('PASS byte-array magic/type/range/alignment and nonzero offset')

    m = configured()
    prefix, suffix = bytes([0x12]) * 1024, bytes([0x34]) * 3072
    assert m.write(prefix) == len(prefix)
    m.output_results = [-5]
    assert m.write(suffix) == -3 and not m.outputs
    assert m.write(suffix) == len(suffix) and m.outputs == [prefix + suffix]
    chunks = bytes([0x56]) * GRAIN_BYTES + bytes([0x78]) * GRAIN_BYTES
    m.output_results = [0, -5]
    assert m.write(chunks) == GRAIN_BYTES
    assert m.outputs[-1] == chunks[:GRAIN_BYTES]
    assert m.write(chunks[GRAIN_BYTES:]) == GRAIN_BYTES
    assert m.outputs[-1] == chunks[GRAIN_BYTES:]
    assert len(m.attempts) == 5
    print('PASS output error rollback, partial accepted byte count and retry')
    print('PASS linked ARM AudioTrack; ELF SHA256=', hashlib.sha256(ELF.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
