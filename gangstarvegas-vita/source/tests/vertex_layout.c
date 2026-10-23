#include "vertex_layout.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(vgl_vbo_attribute_is_packed(0,32,32));
    assert(vgl_vbo_attribute_is_packed(UINT16_MAX,32,32));
    assert(!vgl_vbo_attribute_is_packed(65536,32,32));
    assert(!vgl_vbo_attribute_is_packed(0x21000c,32,32));
    assert(!vgl_vbo_attribute_is_packed(12,32,8));
    assert(!vgl_vbo_attribute_is_packed(12,0,0));
    assert(vgl_client_attribute_is_packed(0x100000,0x10000c,32,32));
    assert(!vgl_client_attribute_is_packed(0x100000,0x11000c,32,32));
    puts("PASS: VBO offset16 boundary, large offsets, unequal/zero strides and independent client layout");
}
