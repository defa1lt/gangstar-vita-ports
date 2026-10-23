#include <stddef.h>
#include "custom_shaders.c"
#define O(t, m) offsetof(t, m)
const uint32_t vgl_test_offsets[] = {
    sizeof(program), O(program,status), O(program,vshader), O(program,fshader),
    O(program,fprog), O(program,vprog), O(program,attr), O(program,attr_map),
    O(program,attr_num), O(program,blend_info), O(program,is_fbo_float),
    O(program,has_unaligned_attrs),
    sizeof(shader), O(shader,prog), O(shader,id), O(shader,unif_buf_size),
    O(shader,type), O(shader,is_glsl), O(shader,source), O(shader,size),
    O(shader,semantics), O(shader,mat), O(shader,valid),
    sizeof(vao), O(vao,vertex_attrib_config), O(vao,vertex_stream_config),
    O(vao,vertex_attrib_vbo), O(vao,vertex_attrib_offsets),
    O(vao,vertex_attrib_state), O(vao,vertex_attrib_divisor),
    O(vao,vertex_attrib_size), O(vao,vertex_attrib_value),
    sizeof(vbo), O(vbo,ptr), O(vbo,size), sizeof(binds_map),
    PROG_LINKED, SCE_GXM_ATTRIBUTE_FORMAT_F32,
    SCE_GXM_INDEX_SOURCE_INDEX_16BIT, SCE_GXM_INDEX_SOURCE_INDEX_32BIT,
    VGL_MODE_POSTPONED, sizeof(vgl_frame_work), O(vgl_frame_work,active), sizeof(blend_info)
};
