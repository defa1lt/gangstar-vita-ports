"""SDK-compiled layouts and shared loader for production ARM renderer tests."""
from pathlib import Path
from functools import lru_cache
import os
import struct
import subprocess
from texture_policy import load,store,word,ROOT
SDK=Path('/Volumes/USB/persanal/psvita/.vitasdk-softfp')
FIELDS=('program_size','status','vshader','fshader','fprog','vprog','attr','attr_map','attr_num','blend_info','is_fbo_float','has_unaligned_attrs',
        'shader_size','shader_prog','shader_id','unif_buf_size','shader_type','is_glsl','source','source_size','semantics','mat','valid',
        'vao_size','attrib_config','stream_config','attrib_vbo','attrib_offsets','attrib_state','attrib_divisor',
        'attrib_size','attrib_value','vbo_size','vbo_ptr','vbo_bytes','bindings_size','linked','f32','index16','index32','postponed','work_size','work_active','blend_size')


@lru_cache(maxsize=1)
def layout():
    target=ROOT/'.agents/scratch/vitagl-test-layout.o'
    env=os.environ.copy();env['VITASDK']=str(SDK)
    env['VITA_HOST_X86_64_LIB']='/Volumes/USB/persanal/psvita/.vitasdk-host-x86_64/lib'
    command=[str(ROOT/'scripts/arm-vita-eabi-gcc'),'-O0','-mfloat-abi=softfp','-mfpu=neon',
             '-DHAVE_SOFTFP_ABI','-DHAVE_SHADER_CACHE','-DLOG_ERRORS','-DFILE_LOG','-DSKIP_SPLASHSCREEN',
             '-DVGL_GIT_HASH="layout-fixture"','-I'+str(ROOT/'build/vitaGL/source'),'-c',
             str(ROOT/'source/tests/vitagl_offsets.c'),'-o',str(target)]
    result=subprocess.run(command,env=env,text=True,capture_output=True)
    if result.returncode: raise RuntimeError(result.stderr)
    data=target.read_bytes();shoff=struct.unpack_from('<I',data,32)[0]
    shsize,count=struct.unpack_from('<HH',data,46)
    sections=[struct.unpack_from('<10I',data,shoff+i*shsize) for i in range(count)]
    for section in sections:
        if section[1]!=2:continue
        names_section=sections[section[6]];names=data[names_section[4]:names_section[4]+names_section[5]]
        for at in range(section[4],section[4]+section[5],section[9]):
            name,value,size,info,other,index=struct.unpack_from('<IIIBBH',data,at)
            if names[name:names.index(0,name)]==b'vgl_test_offsets':
                raw=sections[index][4]+value
                values=struct.unpack_from('<'+'I'*len(FIELDS),data,raw)
                assert size==len(FIELDS)*4
                return dict(zip(FIELDS,values))
    raise AssertionError('SDK layout symbol missing')
