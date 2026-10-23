"""Validate physics trace identities against both supplied real native images."""
import hashlib
from pathlib import Path
import sys
import zipfile
import lief
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from game_profile import LIBRARIES
from prepare_android_debug import MOD_NATIVE_SHA256,physics_trace_profile,patch_dex


def main():
    original=(ROOT/'refs/android/apk/lib/armeabi-v7a/libGangstar4.so').read_bytes()
    with zipfile.ZipFile(ROOT/'refs/GANGSTAR-VEGAS-1.7.0-MOD-APK.apk') as source:
        mod=source.read('lib/armeabi-v7a/libGangstar4.so')
        patched_dex,_=patch_dex(source.read('classes.dex'))
    assert hashlib.sha256(original).hexdigest()==LIBRARIES['lib/armeabi-v7a/libGangstar4.so']['sha256']
    assert hashlib.sha256(mod).hexdigest()==MOD_NATIVE_SHA256
    image=lief.ELF.parse(original);modified=lief.ELF.parse(mod)
    assert {s.name:s.value for s in image.dynamic_symbols}=={s.name:s.value for s in modified.dynamic_symbols}
    expected=bytearray(original)
    # The supplied MOD changes only these three currency/VIP routines.
    for address in (0x672f3c,0x7e1ab0,0xa9efbc):
        offset=image.virtual_address_to_offset(address)
        expected[offset:offset+8]=bytes.fromhex('0804a0e31eff2fe1')
    assert bytes(expected)==mod,'MOD differs outside the three verified instructions'
    baseline=physics_trace_profile(image)
    assert physics_trace_profile(modified)==baseline and len(baseline['functions'])==21
    with zipfile.ZipFile(ROOT/'dist/gangstar-vegas-android-gamepad2.apk') as working:
        assert patched_dex==working.read('classes.dex'),'camera/controller DEX changed'
    for function in baseline['functions']:
        address=function['offset'];saved=list(image.get_content_from_virtual_address(address,8))
        image.patch_address(address,[saved[0]^1]+saved[1:])
        try:physics_trace_profile(image)
        except ValueError:pass
        else:raise AssertionError('modified production guard accepted: '+function['name'])
        image.patch_address(address,saved)
    from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_MEM_WRITE
    from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
    from texture_policy import load,store
    cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM)
    base=0x98000000;load(cpu,ROOT/'refs/android/apk/lib/armeabi-v7a/libGangstar4.so',base)
    player,raw,vtable,stack,ret=0x100000,0x101000,0x102000,0x200800,0x300000
    cpu.mem_map(player,16384);cpu.mem_map(stack&~4095,4096);cpu.mem_map(ret,4096)
    store(cpu,raw,vtable+16)
    writes=[];cpu.hook_add(UC_HOOK_MEM_WRITE,lambda *args:writes.append(args[2]))
    for reference,adjustment in ((raw,0x74),(raw,-32),(0,0)):
        store(cpu,player+baseline['player']['physics_reference'],reference)
        store(cpu,vtable+4,adjustment&0xffffffff)
        writes.clear()
        cpu.reg_write(UC_ARM_REG_R0,player);cpu.reg_write(UC_ARM_REG_SP,stack);cpu.reg_write(UC_ARM_REG_LR,ret)
        getter=next(f for f in baseline['functions'] if f['name']=='get_physics_body')
        cpu.emu_start(base+getter['offset'],ret,count=100)
        assert cpu.reg_read(UC_ARM_REG_PC)==ret
        assert cpu.reg_read(UC_ARM_REG_R0)==(reference+adjustment if reference else 0)
        assert not writes
    print('PASS: actual ARM Character::GetPhysicsBody positive/negative adjustment, null and read-only behavior')
    print('PASS: whole-file original/MOD identity and exact24-byte difference;21physics guards match both and reject corruption;camera/controller DEX unchanged')


if __name__=='__main__':main()
