#ifndef VGL_VERTEX_LAYOUT_H
#define VGL_VERTEX_LAYOUT_H

#include <stdint.h>

static inline int vgl_client_attribute_is_packed(uint32_t base, uint32_t offset,
                                                 uint32_t stride,
                                                 uint32_t attribute_stride) {
    return stride && stride == attribute_stride && offset >= base &&
           offset - base < stride;
}

#endif
