"""Execute linked ARM frame-window reporting, including the 64-bit clock ABI."""
from pathlib import Path
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import *
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'source/tests'))
from texture_policy import load,store,word
STACK,RETURN=0x208000,0x300000


def main():
    cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    symbols=load(cpu,ROOT/'build/gangstarvegas_vita')
    cpu.mem_map(STACK-32768,36864);cpu.mem_map(RETURN,4096)
    cpu.reg_write(UC_ARM_REG_C1_C0_2,0xf<<20);cpu.reg_write(UC_ARM_REG_FPEXC,0x40000000)
    work=symbols['vgl_work'];frame=symbols['vgl_framecount']
    state={'now':10**12,'clocks':0};logs=[]
    def boundary(uc,address,size,unused):
        if address==symbols['sceKernelGetSystemTimeWide']&~1:
            state['clocks']+=1
            uc.reg_write(UC_ARM_REG_R0,state['now']&0xffffffff)
            uc.reg_write(UC_ARM_REG_R1,state['now']>>32)
        elif address==symbols['sceClibMemset']&~1:
            a,b,c=[uc.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2)]
            uc.mem_write(a,bytes([b&255])*c)
        elif address==symbols['vgl_file_log']&~1:
            fmt=uc.reg_read(UC_ARM_REG_R0)
            elapsed=struct.unpack('<Q',bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_SP),8)))[0]
            assert elapsed==5_000_000,elapsed
            logs.append(bytes(uc.mem_read(fmt,512)).split(b'\0',1)[0].decode())
        else:return
        uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    cpu.hook_add(UC_HOOK_CODE,boundary)
    def report(number):
        store(cpu,frame,number)
        cpu.reg_write(UC_ARM_REG_CPSR,0x30 if symbols['vgl_work_report']&1 else 0x10)
        cpu.reg_write(UC_ARM_REG_SP,STACK);cpu.reg_write(UC_ARM_REG_LR,RETURN)
        cpu.emu_start(symbols['vgl_work_report'],RETURN,count=10000)
        assert cpu.reg_read(UC_ARM_REG_PC)==RETURN
    # Eight 64-bit counters, seven uint32 fields, native 8-byte struct alignment.
    assert word(cpu,work+88)==1
    cpu.mem_write(work,struct.pack('<8Q',*range(100,108)))
    report(10)
    assert bytes(cpu.mem_read(work,88))==bytes(88) and word(cpu,work+88)==1
    assert not logs
    store(cpu,work+64,77)
    state['now']+=1_000_000
    report(309)
    assert word(cpu,work+64)==77 and not logs
    state['now']+=4_000_000
    report(310)
    assert len(logs)==1 and logs[-1].startswith('Frame work:')
    assert bytes(cpu.mem_read(work,88))==bytes(88)
    for window in range(2,9):
        state['now']+=5_000_000
        report(10+300*window)
    assert len(logs)==8 and word(cpu,work+88)==0
    before=bytes(cpu.mem_read(work,96));clocks=state['clocks']
    state['now']+=100_000_000
    report(10000)
    assert len(logs)==8 and state['clocks']==clocks
    assert bytes(cpu.mem_read(work,96))==before
    print('PASS: actual ARM frame windows, 64-bit clock, startup excluded, 300-frame cadence, eight-window limit and no clock/log work after stop')


if __name__=='__main__':main()
