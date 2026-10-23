"""Execute linked ARM preload/restore with filesystem and GXM ABI boundaries.

Synthetic cache bytes test ownership and failure handling, not GPU validity.
Actual shader validation remains a physical Vita check.
"""
import struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import *
from vitagl_fixture import layout,load,store,word,ROOT
DATA,SHADER,STACK,RETURN,HEAP=0x100000,0x101000,0x208000,0x300000,0x400000


def blob(L,matrices=()):
    gxp=bytearray(128)
    struct.pack_into('<I',gxp,0,0x00505847);gxp[4:6]=bytes([1,4])
    struct.pack_into('<I',gxp,8,128)
    return struct.pack('<I'+'I'*len(matrices),len(matrices),*matrices)+bytes(L['bindings_size'])+gxp


def machine(files):
    L=layout();cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    S=load(cpu,ROOT/'build/gangstarvegas_vita')
    for address,size in ((DATA,65536),(STACK-32768,36864),(RETURN,4096),(HEAP,0x1800000)):
        cpu.mem_map(address,size)
    cpu.reg_write(UC_ARM_REG_C1_C0_2,0xf<<20);cpu.reg_write(UC_ARM_REG_FPEXC,0x40000000)
    cache=b'ux0:data/cache';cpu.mem_write(S['vgl_shader_cache_path'],cache+b'\0')
    store(cpu,S['gxm_shader_patcher'],1)
    files={cache.decode()+'/'+name:value for name,value in files.items()}
    names=list(files);handles={};live={};events=[]
    state={'next':HEAP,'directory':0,'register_error':False,'fail_at':0,'allocations':0,'short_read':False,'padding':0}
    def string(ptr):return bytes(cpu.mem_read(ptr,512)).split(b'\0',1)[0].decode()
    def result(value):cpu.reg_write(UC_ARM_REG_R0,value&0xffffffff)
    boundaries=('sceIoDopen','sceIoDread','sceIoDclose','sceIoOpen','sceIoLseek','sceIoRead','sceIoClose','sceIoRemove',
        'vglMalloc','vglMallocUsableSize','vgl_free','sceGxmProgramGetSize','sceGxmProgramCheck',
        'sceGxmShaderPatcherRegisterProgram','sceGxmShaderPatcherForceUnregisterProgram',
        'sceGxmProgramGetParameterCount','sceGxmProgramGetDefaultUniformBufferSize',
        'sceClibMemcpy','sceClibMemmove','sceClibMemset','memcpy','memmove','memset',
        'strcmp','strncmp','strlen','snprintf','vgl_file_log')
    by_address={S[name]&~1:name for name in boundaries if name in S}
    def boundary(uc,address,size,unused):
        name=by_address.get(address)
        if not name:return
        a,b,c,d=[uc.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        if name=='vglMalloc':
            state['allocations']+=1
            if state['fail_at']==state['allocations']:result(0)
            else:
                ptr=state['next'];usable=((a+15)&~15)+state['padding']
                state['next']+=usable;live[ptr]=usable;result(ptr)
        elif name=='vglMallocUsableSize':result(live[a])
        elif name=='vgl_free':
            if a:assert a in live,('invalid/double free',hex(a));del live[a]
        elif name in ('memcpy','memmove','sceClibMemcpy','sceClibMemmove'):
            uc.mem_write(a,bytes(uc.mem_read(b,c)));result(a)
        elif name in ('memset','sceClibMemset'):uc.mem_write(a,bytes([b&255])*c);result(a)
        elif name=='strlen':result(len(string(a)))
        elif name in ('strcmp','strncmp'):
            left,right=string(a),string(b)
            if name=='strncmp':left,right=left[:c],right[:c]
            result((left>right)-(left<right))
        elif name=='snprintf':
            fmt=string(c);assert fmt=='%s/%s',fmt
            second=word(uc,uc.reg_read(UC_ARM_REG_SP))
            value=(string(d)+'/'+string(second)).encode()
            if b:uc.mem_write(a,value[:b-1]+b'\0')
            result(len(value))
        elif name=='sceIoDopen':state['directory']=0;events.append(name);result(10)
        elif name=='sceIoDread':
            i=state['directory'];state['directory']+=1
            if i==len(names):result(0)
            else:
                filename=names[i];uc.mem_write(b,bytes(0x160));store(uc,b,0x2000)
                uc.mem_write(b+8,struct.pack('<Q',len(files[filename])))
                uc.mem_write(b+0x58,filename.rsplit('/',1)[1].encode()+b'\0');result(1)
        elif name=='sceIoOpen':
            filename=string(a);events.append((name,filename,b))
            assert b==1,'restore must only read cache'
            if filename not in files:result(-1)
            else:fd=20+len(handles);handles[fd]=filename;result(fd)
        elif name=='sceIoLseek':
            whence=word(uc,uc.reg_read(UC_ARM_REG_SP));assert c==d==0
            assert whence in (0,2)
            result(len(files[handles[a]]) if whence==2 else 0);uc.reg_write(UC_ARM_REG_R1,0)
        elif name=='sceIoRead':
            value=files[handles[a]][:c-(1 if state['short_read'] else 0)]
            uc.mem_write(b,value);events.append(name);result(len(value))
        elif name in ('sceIoClose','sceIoDclose'):result(0)
        elif name=='sceIoRemove':raise AssertionError('resource failure deleted cache')
        elif name=='sceGxmProgramGetSize':events.append(name);result(word(uc,a+8))
        elif name=='sceGxmProgramCheck':events.append(name);result(0)
        elif name=='sceGxmShaderPatcherRegisterProgram':
            events.append(name)
            if state['register_error']:result(-1)
            else:store(uc,c,0x777);result(0)
        elif name=='sceGxmShaderPatcherForceUnregisterProgram':events.append(name);result(0)
        elif name=='sceGxmProgramGetParameterCount':result(2)
        elif name=='sceGxmProgramGetDefaultUniformBufferSize':result(0)
        uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    cpu.hook_add(UC_HOOK_CODE,boundary)
    def call(name,*args):
        cpu.reg_write(UC_ARM_REG_CPSR,0x30 if S[name]&1 else 0x10)
        cpu.reg_write(UC_ARM_REG_SP,STACK);cpu.reg_write(UC_ARM_REG_LR,RETURN)
        for reg,value in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),args):cpu.reg_write(reg,value)
        cpu.emu_start(S[name],RETURN,count=3000000)
        assert cpu.reg_read(UC_ARM_REG_PC)==RETURN,(name,hex(cpu.reg_read(UC_ARM_REG_PC)))
        assert cpu.reg_read(UC_ARM_REG_SP)==STACK
        return cpu.reg_read(UC_ARM_REG_R0)
    def restore(filename):
        cpu.mem_write(DATA,filename.encode()+b'\0')
        return call('load_cached_shader',DATA,SHADER,1)
    return cpu,S,L,state,live,events,files,call,restore


def main():
    L=layout();value=blob(L)
    # Count coverage exceeds the old 256-entry limit; only raw blobs are retained.
    env=machine({f'pair1-{i:04X}.gxp':value for i in range(300)})
    cpu,S,L,state,live,events,files,call,restore=env
    assert call('vglPreloadShaderCache')==300
    assert not any(isinstance(e,str) and e.startswith('sceGxm') for e in events)
    assert word(cpu,S['prepared_shader_bytes'])==sum(live.values())<4*1024*1024
    before=len(events);assert call('vglPreloadShaderCache')==300 and len(events)==before
    reads=events.count('sceIoRead')
    assert restore(next(iter(files)))==1
    assert events.count('sceIoRead')==reads
    assert word(cpu,S['prepared_shader_count'])==299
    assert word(cpu,SHADER+L['shader_id'])==0x777
    # Reset releases all unused blobs and permits a fresh preload.
    reset=machine({'pair1-reset.gxp':value})
    rcpu,rS,rL,rstate,rlive,revents,rfiles,rcall,rrestore=reset
    assert rcall('vglPreloadShaderCache')==1
    rcall('reset_custom_shaders')
    assert not rlive and word(rcpu,rS['prepared_shader_count'])==0
    assert word(rcpu,rS['prepared_shader_bytes'])==0
    assert rcall('vglPreloadShaderCache')==1
    # Restore failure consumes only the prepared entry and preserves disk bytes.
    env=machine({'pair1-failure.gxp':value})
    cpu,S,L,state,live,events,files,call,restore=env
    assert call('vglPreloadShaderCache')==1
    original=bytes(cpu.mem_read(SHADER,L['shader_size']));disk=dict(files)
    state['register_error']=True
    assert restore(next(iter(files)))==0
    assert not live and word(cpu,S['prepared_shader_count'])==0
    assert word(cpu,S['prepared_shader_bytes'])==0 and files==disk
    assert bytes(cpu.mem_read(SHADER,L['shader_size']))==original
    state['register_error']=False
    assert restore(next(iter(files)))==1 and events.count('sceIoRead')==2
    # Fail allocation of the second matrix after successful registration.
    env=machine({'pair1-matrix.gxp':blob(L,(0,1))})
    cpu,S,L,state,live,events,files,call,restore=env
    assert call('vglPreloadShaderCache')==1
    original=bytes(cpu.mem_read(SHADER,L['shader_size']))
    state['fail_at']=state['allocations']+3
    assert restore(next(iter(files)))==0 and not live
    assert bytes(cpu.mem_read(SHADER,L['shader_size']))==original
    assert events.count('sceGxmShaderPatcherForceUnregisterProgram')==1
    # Allocator overhead is charged; oversized files are skipped before reading.
    env=machine({f'pair1-{i:03X}.gxp':bytes(250000) for i in range(30)}|{'pair1-huge.gxp':bytes(262145)})
    cpu,S,L,state,live,events,files,call,restore=env;state['padding']=2048
    count=call('vglPreloadShaderCache')
    assert 0<count<30 and word(cpu,S['prepared_shader_bytes'])==sum(live.values())<=4*1024*1024
    assert not any(isinstance(e,tuple) and e[1].endswith('huge.gxp') for e in events)
    # A short read frees both the file buffer and prepared entry.
    env=machine({'pair1-short.gxp':value})
    cpu,S,L,state,live,events,files,call,restore=env;state['short_read']=True
    assert call('vglPreloadShaderCache')==0 and not live
    # Serialization OOM returns before copying through a null destination.
    env=machine({});cpu,S,L,state,live,events,files,call,restore=env
    store(cpu,SHADER+L['source_size'],128);state['fail_at']=1
    assert call('serialize_shader',0,DATA,SHADER,1)==0
    assert word(cpu,DATA)==4+L['bindings_size']+128 and not live
    print('PASS: actual ARM 300-entry blob preload, lazy GXM registration, idempotence/reset, consume without disk IO, OOM rollback/retry, matrix rollback, byte budget, file cap, short read and serialization OOM')


if __name__=='__main__':main()
