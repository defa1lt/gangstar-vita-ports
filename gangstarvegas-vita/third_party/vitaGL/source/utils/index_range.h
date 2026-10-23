#ifndef VGL_INDEX_RANGE_H
#define VGL_INDEX_RANGE_H

#include <stdint.h>

static inline uint32_t vgl_index_range(const void *indices, uint32_t count,
                                      int wide, uint32_t *highest) {
    uint32_t lowest = UINT32_MAX;
    *highest = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = wide ? ((const uint32_t *)indices)[i]
                              : ((const uint16_t *)indices)[i];
        if (index < lowest) lowest = index;
        if (index > *highest) *highest = index;
    }
    return lowest;
}

static inline void vgl_rebase_indices(void *dest, const void *source,
                                     uint32_t count, int wide, uint32_t first) {
    if (wide) {
        for (uint32_t i = 0; i < count; ++i)
            ((uint32_t *)dest)[i] = ((const uint32_t *)source)[i] - first;
    } else {
        for (uint32_t i = 0; i < count; ++i)
            ((uint16_t *)dest)[i] = ((const uint16_t *)source)[i] - first;
    }
}

enum vgl_index_primitive {
    VGL_INDEX_NATIVE,
    VGL_INDEX_QUADS,
    VGL_INDEX_LINE_STRIP,
    VGL_INDEX_LINE_LOOP
};

static inline int vgl_expanded_index_count(uint32_t count,
        enum vgl_index_primitive primitive, int wide, uint32_t *expanded) {
    uint64_t size = count;
    switch (primitive) {
    case VGL_INDEX_QUADS:
        if (count < 4) return 0;
        size = (uint64_t)(count / 4) * 6;
        break;
    case VGL_INDEX_LINE_STRIP:
        if (count < 2) return 0;
        size = (uint64_t)(count - 1) * 2;
        break;
    case VGL_INDEX_LINE_LOOP:
        if (count < 2) return 0;
        size = (uint64_t)count * 2;
        break;
    default:
        if (!count) return 0;
        break;
    }
    if (size > INT32_MAX || size > UINT32_MAX / (wide ? 4u : 2u)) return 0;
    *expanded = (uint32_t)size;
    return 1;
}

static inline void vgl_expand_rebased_indices(void *dest, const void *source,
        uint32_t count, int wide, enum vgl_index_primitive primitive, uint32_t first) {
    if (primitive == VGL_INDEX_NATIVE) {
        vgl_rebase_indices(dest, source, count, wide, first);
        return;
    }
#define VGL_EXPAND_TYPED(type) do { \
    type *out = (type *)dest; \
    const type *in = (const type *)source; \
    if (primitive == VGL_INDEX_QUADS) { \
        for (uint32_t i = 0; i < count / 4; ++i) { \
            out[i * 6] = in[i * 4] - first; \
            out[i * 6 + 1] = in[i * 4 + 1] - first; \
            out[i * 6 + 2] = in[i * 4 + 3] - first; \
            out[i * 6 + 3] = in[i * 4 + 1] - first; \
            out[i * 6 + 4] = in[i * 4 + 2] - first; \
            out[i * 6 + 5] = in[i * 4 + 3] - first; \
        } \
    } else { \
        for (uint32_t i = 0; i < count - 1; ++i) { \
            out[i * 2] = in[i] - first; \
            out[i * 2 + 1] = in[i + 1] - first; \
        } \
        if (primitive == VGL_INDEX_LINE_LOOP) { \
            out[(count - 1) * 2] = in[count - 1] - first; \
            out[(count - 1) * 2 + 1] = in[0] - first; \
        } \
    } \
} while (0)
    if (wide) { VGL_EXPAND_TYPED(uint32_t); }
    else { VGL_EXPAND_TYPED(uint16_t); }
#undef VGL_EXPAND_TYPED
}

/* GXP 1.x program_flags at byte 0x14: INDEX and INSTANCE inputs must retain
 * their original index values, including when shader source is unavailable.
 * Vita3K's gxm/types.h and shader/gxp_parser.cpp describe this binary layout. */
static inline int vgl_program_allows_index_rebase(const void *program) {
    const uint32_t *words = (const uint32_t *)program;
    const uint8_t *bytes = (const uint8_t *)program;
    return program && words[0] == 0x00505847u && bytes[4] == 1 &&
           bytes[5] >= 4 && words[2] >= 24 &&
           !(words[5] & (1u | (1u << 17) | (1u << 18)));
}

#endif
