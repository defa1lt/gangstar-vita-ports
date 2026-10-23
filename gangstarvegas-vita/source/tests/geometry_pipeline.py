"""Execute production ARM Elements setup and index-range reuse.

Only GPU, allocator and libc boundaries are supplied. Classification, range
scanning, source offsets and byte copies execute the linked renderer.
"""
import struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ
from unicorn.arm_const import *
from vitagl_fixture import layout,load,store,word,ROOT
VAO,INDICES,GXP,CLIENT,A,B=0x100000,0x100400,0x100800,0x101000,0x102000,0x102100
STACK,RETURN,HEAP=0x208000,0x300000,0x500000


def machine():
    L=layout();cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    S=load(cpu,ROOT/'build/gangstarvegas_vita')
    for addr,size in ((VAO,65536),(STACK-32768,36864),(RETURN,4096),(HEAP,65536),(0x800000,0x400000)):
        cpu.mem_map(addr,size)
    cpu.reg_write(UC_ARM_REG_C1_C0_2,0xf<<20);cpu.reg_write(UC_ARM_REG_FPEXC,0x40000000)
    store(cpu,S['cur_program'],1);store(cpu,S['cur_vao'],VAO)
    store(cpu,S['vgl_work']+L['work_active'],0)
    p=S['progs'];vertex=S['shaders'];fragment=vertex+L['shader_size']
    cpu.mem_write(p,bytes(L['program_size']));cpu.mem_write(vertex,bytes(2*L['shader_size']))
    cpu.mem_write(p+L['status'],bytes([L['linked']]))
    store(cpu,p+L['vshader'],vertex);store(cpu,p+L['fshader'],fragment)
    store(cpu,vertex+L['shader_prog'],GXP);store(cpu,vertex+L['shader_id'],1)
    store(cpu,p+L['fprog'],0x710000)
    cpu.mem_write(p+L['blend_info'],bytes(cpu.mem_read(S['blend_info'],L['blend_size'])))
    cpu.mem_write(p+L['is_fbo_float'],bytes(cpu.mem_read(S['is_fbo_float'],1)))
    store(cpu,p+L['attr_num'],3);cpu.mem_write(p+L['attr_map'],bytes([0,1,2]))
    cpu.mem_write(GXP,bytes(128));store(cpu,GXP,0x00505847);cpu.mem_write(GXP+4,bytes([1,4]));store(cpu,GXP+8,128)
    store(cpu,A+L['vbo_ptr'],0x800000);store(cpu,A+L['vbo_bytes'],0x400000)
    store(cpu,B+L['vbo_ptr'],0x900000);store(cpu,B+L['vbo_bytes'],0x400000)
    cpu.mem_write(CLIENT,bytes((i*17+3)&255 for i in range(4096)))
    calls={'alloc':[],'streams':{},'copies':[],'index_bytes':0,'patch_attrs':[]};next_free=HEAP
    names=('sceGxmSetFragmentProgram','sceGxmShaderPatcherCreateVertexProgram','sceGxmSetVertexProgram','sceGxmSetVertexStream',
           'vgl_reserve_data_pool','sceClibMemcpy','sceClibMemset','memcpy','memset','memmove','vgl_file_log')
    boundary_names={S[n]&~1:n for n in names if n in S}
    def boundary(uc,address,size,unused):
        nonlocal next_free
        name=boundary_names.get(address)
        if not name:return
        a,b,c,d=[uc.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        if name=='vgl_reserve_data_pool':
            calls['alloc'].append(a);result=next_free;next_free+=(a+63)&~63;uc.reg_write(UC_ARM_REG_R0,result)
        elif name=='sceGxmShaderPatcherCreateVertexProgram':
            sp=uc.reg_read(UC_ARM_REG_SP)
            calls['patch_attrs']=[struct.unpack('<HHBBH',bytes(uc.mem_read(c+i*8,8))) for i in range(d)]
            store(uc,word(uc,sp+8),0x720000);uc.reg_write(UC_ARM_REG_R0,0)
        elif name=='sceGxmSetVertexStream':calls['streams'][b]=c
        elif name in ('memcpy','memmove','sceClibMemcpy'):
            calls['copies'].append((a,b,c));uc.mem_write(a,bytes(uc.mem_read(b,c)))
        elif name in ('memset','sceClibMemset'):uc.mem_write(a,bytes([b&255])*c)
        uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    def reads(uc,access,address,size,value,unused):
        if INDICES<=address<INDICES+12:calls['index_bytes']+=size
    cpu.hook_add(UC_HOOK_CODE,boundary);cpu.hook_add(UC_HOOK_MEM_READ,reads)
    def call(name,*args):
        cpu.reg_write(UC_ARM_REG_CPSR,0x30 if S[name]&1 else 0x10)
        cpu.reg_write(UC_ARM_REG_SP,STACK);cpu.reg_write(UC_ARM_REG_LR,RETURN)
        for reg,value in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),args):cpu.reg_write(reg,value)
        for i,value in enumerate(args[4:]):store(cpu,STACK+4*i,value)
        try:cpu.emu_start(S[name],RETURN,count=100000)
        except Exception:
            print(name,'PC',hex(cpu.reg_read(UC_ARM_REG_PC)),'LR',hex(cpu.reg_read(UC_ARM_REG_LR)));raise
        assert cpu.reg_read(UC_ARM_REG_PC)==RETURN,name
        assert cpu.reg_read(UC_ARM_REG_SP)==STACK,name
        return cpu.reg_read(UC_ARM_REG_R0)
    def configure(buffers,active=7):
        for i,buffer in enumerate(buffers):
            cpu.mem_write(VAO+L['attrib_size']+i,bytes([4]))
            store(cpu,VAO+L['attrib_value']+i*4,CLIENT+3000+i*16)
            cpu.mem_write(CLIENT+3000+i*16,struct.pack('<4f',0,0,0,1))
            descriptor=struct.pack('<HHBBH',i,0,L['f32'],3,i)
            cpu.mem_write(VAO+L['attrib_config']+i*8,descriptor)
            cpu.mem_write(p+L['attr']+i*8,descriptor)
            cpu.mem_write(VAO+L['stream_config']+i*4,struct.pack('<HH',12,0))
            store(cpu,VAO+L['attrib_vbo']+i*4,buffer)
            store(cpu,VAO+L['attrib_offsets']+i*4,(0x110000+i*12) if buffer else CLIENT+i*128)
        store(cpu,VAO+L['attrib_state'],active)
    return cpu,S,L,calls,call,configure


def main():
    for wide in (False,True):
        for buffers,active in (((A,B,0),7),((0,A,B),7),((A,0,B),7),((A,B,0),3),((A,B,A),7)):
            for indices in ((0,2,1),(4,6,5)):
                cpu,S,L,calls,call,configure=machine();configure(buffers,active)
                cpu.mem_write(INDICES,struct.pack('<3'+('I' if wide else 'H'),*indices))
                assert call('_vglDrawElementsFirstIndex',INDICES,3,int(wide),CLIENT+4000)==0
                assert word(cpu,CLIENT+4000)==0xffffffff
                calls['index_bytes']=0
                assert call('_glDrawElements_CustomShadersIMPL',INDICES,3,0,0,L['index32' if wide else 'index16'],0)==1
                enabled_clients=[i for i,b in enumerate(buffers) if not b and active&(1<<i)]
                assert calls['alloc']==[(max(indices)+1)*12]*len(enabled_clients),calls
                for i,b in enumerate(buffers):
                    if not active&(1<<i):
                        assert calls['streams'][i]==CLIENT+3000+i*16
                        assert calls['patch_attrs'][i][1]==0 and calls['patch_attrs'][i][3]==4
                        continue
                    if b:assert calls['streams'][i]==word(cpu,b+L['vbo_ptr'])+0x110000+i*12
                    elif i in enabled_clients:
                        n=(max(indices)+1)*12
                        assert bytes(cpu.mem_read(calls['streams'][i],n))==bytes(cpu.mem_read(CLIENT+i*128,n))
        # A valid zero minimum keeps the validated maximum and avoids a second scan.
        cpu,S,L,calls,call,configure=machine();configure((0,0,0))
        cpu.mem_write(INDICES,struct.pack('<3'+('I' if wide else 'H'),0,2,1))
        first=call('_vglDrawElementsFirstIndex',INDICES,3,int(wide),CLIENT+4000)
        highest=word(cpu,CLIENT+4000)
        assert first==0 and highest==2
        read_bytes=calls['index_bytes']
        assert read_bytes==3*(4 if wide else 2),read_bytes
        assert call('_glDrawElements_CustomShadersIMPL',INDICES,3,highest-first+1,0,L['index32' if wide else 'index16'],first)==1
        assert calls['index_bytes']==read_bytes
        assert calls['alloc']==[36,36,36],calls['alloc']
        # Eligibility and pointer-overflow failures leave the sentinel unpublished.
        store(cpu,VAO+L['attrib_divisor'],1)
        assert call('_vglDrawElementsFirstIndex',INDICES,3,int(wide),CLIENT+4000)==0
        assert word(cpu,CLIENT+4000)==0xffffffff
        store(cpu,VAO+L['attrib_divisor'],0)
        store(cpu,VAO+L['attrib_offsets'],0xfffffff0)
        assert call('_vglDrawElementsFirstIndex',INDICES,3,int(wide),CLIENT+4000)==0
        assert word(cpu,CLIENT+4000)==0xffffffff
    print('PASS: actual ARM Elements mixed-source permutations, disabled constants, full VBO addresses, U16/U32, min0 max reuse with one index scan and eligibility guards')


if __name__=='__main__':main()
