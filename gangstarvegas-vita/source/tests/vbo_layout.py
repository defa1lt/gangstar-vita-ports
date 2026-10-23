"""Execute the linked ARM VBO packing classifier with real SDK struct layouts.

Only file logging is intercepted. The classifier, active attribute selection,
32-bit offset checks and stride comparisons execute production instructions.
This does not prove on-device geometry; the bounded VBO log confirms real hits.
"""
from pathlib import Path
import struct
import sys
import subprocess
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import *
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'source/tests'))
from texture_policy import load,store
PROGRAM,STREAMS,VAO,VBO,OTHER,STACK,RETURN=0x100000,0x100800,0x101000,0x102000,0x102100,0x204000,0x300000


def main():
    cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    symbols=load(cpu,ROOT/'build/gangstarvegas_vita')
    cpu.mem_map(PROGRAM,16384);cpu.mem_map(STACK-8192,12288);cpu.mem_map(RETURN,4096)
    store(cpu,symbols['cur_vao'],VAO);store(cpu,VBO,0x400000);store(cpu,VBO+4,0x400000)
    store(cpu,OTHER,0x800000);store(cpu,OTHER+4,0x400000)
    # SDK: SceGxmVertexStream is 4B; program.attr_map+332/attr_num+360;
    # VAO offsets+20, buffers+84, active mask+148. The binary checks below
    # guard those fixtures against a silently changed production layout.
    sdk=Path('/Volumes/USB/persanal/psvita/.vitasdk-softfp/bin/arm-vita-eabi-objdump')
    asm=subprocess.check_output([str(sdk),'-d','--disassemble=vbo_attributes_are_packed',str(ROOT/'build/gangstarvegas_vita')],text=True)
    for marker in ('#360','#332','#148','#84','#20'):
        assert marker in asm,marker
    for name in ('_glDrawArrays_CustomShadersIMPL','_glMultiDrawArrays_CustomShadersIMPL','_glDrawElements_CustomShadersIMPL'):
        body=subprocess.check_output([str(sdk),'-d','--disassemble='+name,str(ROOT/'build/gangstarvegas_vita')],text=True)
        assert '<vbo_attributes_are_packed>' in body,name
    events=[]
    def boundary(uc,address,size,unused):
        if address==(symbols['vgl_file_log']&~1):
            events.append(address);uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    cpu.hook_add(UC_HOOK_CODE,boundary)
    attributes=(5,2,7)
    def classify(offsets,strides=(32,32,32),active=7,buffers=(VBO,VBO,VBO)):
        cpu.mem_write(PROGRAM,bytes(512));cpu.mem_write(VAO,bytes(512))
        cpu.mem_write(PROGRAM+332,bytes(attributes));store(cpu,PROGRAM+360,3)
        mask=0
        for i,attr in enumerate(attributes):
            store(cpu,VAO+20+attr*4,offsets[i]);store(cpu,VAO+84+attr*4,buffers[i])
            cpu.mem_write(STREAMS+i*4,struct.pack('<HH',strides[i],0))
            if active&(1<<i):mask|=1<<attr
        store(cpu,VAO+148,mask)
        cpu.reg_write(UC_ARM_REG_CPSR,0x30);cpu.reg_write(UC_ARM_REG_SP,STACK)
        cpu.reg_write(UC_ARM_REG_LR,RETURN);cpu.reg_write(UC_ARM_REG_R0,PROGRAM);cpu.reg_write(UC_ARM_REG_R1,STREAMS)
        cpu.emu_start(symbols['vbo_attributes_are_packed'],RETURN,count=10000)
        assert cpu.reg_read(UC_ARM_REG_PC)==RETURN and cpu.reg_read(UC_ARM_REG_SP)==STACK
        return cpu.reg_read(UC_ARM_REG_R0)
    assert classify((0,12,20))==1
    assert classify((0,65535,20))==1
    assert classify((0,65536,20))==0
    assert classify((0x10000,0x10008,0x1000c),active=5,buffers=(VBO,0,VBO))==0
    assert classify((0,0xffffffff,12),active=5,buffers=(VBO,0,VBO))==1
    assert classify((0,12,20),strides=(12,8,12))==0
    assert classify((0,12,20),buffers=(VBO,OTHER,VBO))==0
    assert classify((0,12,20),buffers=(VBO,0,VBO))==0
    assert classify((0,12,20),strides=(0,0,0))==0
    for _ in range(20):assert classify((0x210000,0x21000c,0x210018))==0
    assert len(events)==8,events
    print('PASS: actual ARM VBO classifier, all three draw callers, 65535/65536, large offsets, inactive client attribute, mixed sources/strides, bounded logging')


if __name__=='__main__':main()
