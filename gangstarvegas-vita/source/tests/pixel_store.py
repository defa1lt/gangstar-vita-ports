"""Execute linked VitaGL pixel-store state, queries and unpack normalization.

Allocator, libc copies and file logging are environment boundaries. No renderer
is simulated; this verifies production ARM byte transfers, not the on-Vita scene.
"""
from pathlib import Path
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import *
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'source/tests'))
from texture_policy import load,store,word
DATA, OUT, STACK, RETURN, HEAP = 0x100000,0x100800,0x204000,0x300000,0x400000
PACK,UNPACK,ROW = 0xd05,0xcf5,0xcf2
RGB,RGBA,UBYTE = 0x1907,0x1908,0x1401


def main():
    cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    symbols=load(cpu,ROOT/'build/gangstarvegas_vita')
    for address,size in ((DATA,4096),(STACK-16384,20480),(RETURN,4096),(HEAP,65536)):
        cpu.mem_map(address,size)
    cpu.reg_write(UC_ARM_REG_C1_C0_2,0xf << 20)
    cpu.reg_write(UC_ARM_REG_FPEXC,0x40000000)
    allocations=[]; freed=[]; fail=False
    names=('vglMalloc','vgl_free','malloc','free','memcpy','memmove','sceClibMemcpy','vgl_file_log','diagnostics_log','port_trace','__emutls_get_address','sceKernelGetSystemTimeWide')
    boundaries={symbols[name]&~1:name for name in names}
    def boundary(uc,address,size,unused):
        name=boundaries.get(address)
        if not name:return
        a,b,c=[uc.reg_read(reg) for reg in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2)]
        if name in ('vglMalloc','malloc'):
            allocations.append(a)
            uc.reg_write(UC_ARM_REG_R0,0 if fail else HEAP)
        elif name=='__emutls_get_address':
            store(uc,OUT+128,0);store(uc,OUT+132,512)
            uc.reg_write(UC_ARM_REG_R0,OUT+128)
        elif name in ('vgl_free','free'):freed.append(a)
        elif name in ('memcpy','memmove','sceClibMemcpy'):
            uc.mem_write(a,bytes(uc.mem_read(b,c)))
        uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    cpu.hook_add(UC_HOOK_CODE,boundary)
    def call(name,*args):
        cpu.reg_write(UC_ARM_REG_CPSR,0x30 if symbols[name]&1 else 0x10)
        cpu.reg_write(UC_ARM_REG_SP,STACK);cpu.reg_write(UC_ARM_REG_LR,RETURN)
        for reg,value in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),args):
            cpu.reg_write(reg,value&0xffffffff)
        for i,value in enumerate(args[4:]):store(cpu,STACK+4*i,value&0xffffffff)
        try:
            cpu.emu_start(symbols[name],RETURN,count=100000)
        except Exception:
            print(name,'PC',hex(cpu.reg_read(UC_ARM_REG_PC)),'LR',hex(cpu.reg_read(UC_ARM_REG_LR)))
            raise
        assert cpu.reg_read(UC_ARM_REG_PC)==RETURN,name
        assert cpu.reg_read(UC_ARM_REG_SP)==STACK,name
        return cpu.reg_read(UC_ARM_REG_R0)
    def get(pname,method='glGetIntegerv'):
        cpu.mem_write(OUT,bytes(16));call(method,pname,OUT)
        kind={'glGetIntegerv':'i','glGetBooleanv':'B','glGetFloatv':'f','glGetDoublev':'d'}[method]
        return struct.unpack('<'+kind,bytes(cpu.mem_read(OUT,struct.calcsize(kind))))[0]
    assert get(PACK)==4 and get(UNPACK)==4 and get(ROW)==0
    for pname in (PACK,UNPACK):
        for value in (1,2,4,8):
            call('glPixelStorei',pname,value)
            for method in ('glGetIntegerv','glGetFloatv','glGetDoublev','glGetBooleanv'):
                assert get(pname,method)==(1 if method=='glGetBooleanv' else value)
            assert call('glGetError')==0
        for bad in (0,3,-1,16):
            call('glPixelStorei',pname,bad)
            assert call('glGetError')==0x501 and get(pname)==8
    call('glPixelStorei',ROW,8);assert get(ROW)==8
    call('glPixelStorei',ROW,-1);assert call('glGetError')==0x501 and get(ROW)==8
    call('glPixelStorei',0xdead,4);assert call('glGetError')==0x500
    source=bytes(range(128));cpu.mem_write(DATA,source)
    def unpack(width,height,format=RGB,type=UBYTE,alignment=4,row=0):
        allocations.clear();store(cpu,OUT,0);store(cpu,OUT+4,0)
        error=call('prepare_pixel_unpack',DATA,width,height,format,type,alignment,row,OUT,OUT+4)
        return error,word(cpu,OUT),word(cpu,OUT+4)
    assert unpack(3,2)==(0,HEAP,HEAP) and allocations==[18]
    assert bytes(cpu.mem_read(HEAP,18))==source[:9]+source[12:21]
    assert unpack(3,2,alignment=1)==(0,DATA,0) and not allocations
    assert unpack(4,2,RGBA,row=8)==(0,HEAP,HEAP)
    assert bytes(cpu.mem_read(HEAP,32))==source[:16]+source[32:48]
    assert unpack(4,2,RGBA,row=2)==(0,HEAP,HEAP)
    assert bytes(cpu.mem_read(HEAP,32))==source[:16]+source[8:24]
    assert unpack(1,3,RGB,0x8363,8)==(0,HEAP,HEAP)
    assert bytes(cpu.mem_read(HEAP,6))==source[:2]+source[8:10]+source[16:18]
    fail=True;assert unpack(3,2)[0]==0x505;fail=False
    assert unpack(0x7fffffff,2,RGBA)[0]==0x501 and not allocations
    # Invalid public API arguments with unreadable source must error before transfer.
    for target,width,format,type,error in ((0xdead,3,RGB,UBYTE,0x500),(0xde1,4097,RGB,UBYTE,0x501),(0xde1,3,0xdead,UBYTE,0x501),(0xde1,3,RGB,0xdead,0x500)):
        allocations.clear()
        call('glTexImage2D',target,0,RGB,width,2,0,format,type,0xdead0000)
        assert call('glGetError')==error and not allocations
    captured=[]
    def gpu_boundary(uc,address,size,unused):
        if address==(symbols['gpu_alloc_texture']&~1):
            width,height=[uc.reg_read(reg) for reg in (UC_ARM_REG_R0,UC_ARM_REG_R1)]
            pointer=uc.reg_read(UC_ARM_REG_R3)
            captured.append((width,height,bytes(uc.mem_read(pointer,width*height*4))))
            uc.emu_stop()
    cpu.hook_add(UC_HOOK_CODE,gpu_boundary)
    def until_gpu(name,*args):
        try:call(name,*args)
        except AssertionError:
            assert cpu.reg_read(UC_ARM_REG_PC)==(symbols['gpu_alloc_texture']&~1)
        assert captured
    allocations.clear();captured.clear()
    until_gpu('vglTexImage2DTight',0xde1,0,RGBA,3,2,0,RGBA,UBYTE,DATA)
    assert captured==[(3,2,source[:24])] and not allocations
    assert get(UNPACK)==8 and get(ROW)==8
    cpu.mem_write(DATA,bytes(8));allocations.clear();captured.clear()
    until_gpu('glCompressedTexImage2D_soloader',0xde1,0,0x8c92,4,4,0,8,DATA)
    assert captured==[(4,4,bytes([0,0,0,255])*16)] and allocations==[64]
    assert get(UNPACK)==8 and get(ROW)==8
    print('PASS: actual ARM pixel-store defaults/queries/errors, padded RGB/packed/RGBA/overlap rows, allocation failure/overflow, invalid public upload before source read, actual ATC decode bypasses UNPACK state')


if __name__=='__main__':main()
