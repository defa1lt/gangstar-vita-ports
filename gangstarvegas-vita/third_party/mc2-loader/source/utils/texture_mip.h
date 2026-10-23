#ifndef SOLOADER_TEXTURE_MIP_H
#define SOLOADER_TEXTURE_MIP_H

#include <stdint.h>

static inline unsigned texture_dds_max_dimension(unsigned upload_cap,
                                                  int substitute_skip) {
    if (substitute_skip < 0 || substitute_skip >= 32 ||
        upload_cap > (UINT32_MAX >> substitute_skip)) return UINT32_MAX;
    return upload_cap << substitute_skip;
}

static inline unsigned texture_select_mip_level(unsigned source_max,
                                                unsigned source_level,
                                                unsigned mip_count,
                                                unsigned max_dimension) {
    if (mip_count <= 1 || source_level >= 32 || max_dimension == 0) return 0;
    uint64_t original_max = (uint64_t)source_max << source_level;
    unsigned selected = 0;
    while (original_max > max_dimension && selected < mip_count - 1) {
        original_max = (original_max + 1u) >> 1;
        ++selected;
    }
    return selected;
}

static inline unsigned texture_select_dds_dimensions(unsigned *width,
                                                      unsigned *height,
                                                      unsigned mip_count,
                                                      unsigned max_dimension,
                                                      unsigned header_flags) {
    if (!*width || !*height || (header_flags & 8u)) return 0;
    unsigned w = *width, h = *height;
    unsigned maximum = w > h ? w : h;
    unsigned selected = texture_select_mip_level(maximum, 0, mip_count,
                                                  max_dimension);
    for (unsigned level = 0; level < selected; ++level) {
        /* Rio's DDS seek uses w*h*bpp/8 without compressed-block rounding. */
        if (w < 4 || h < 4 || (w & 3u) || (h & 3u)) return 0;
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
    }
    if (selected && (w < 4 || h < 4 || (w & 3u) || (h & 3u))) return 0;
    *width = w;
    *height = h;
    return selected;
}

/* The range includes every smaller mip, including native profile substitutes. */
static inline int texture_dds_payload_range(const uint32_t *header,
                                            unsigned file_size, unsigned cap,
                                            unsigned *offset, unsigned *size) {
    if (header[0] != 0x20534444u || header[1] != 124 || header[19] != 32 ||
        !(header[20] & 4u) || header[6] > 1 ||
        (header[28] & (0x200u | 0x200000u))) return 0;
    unsigned bpp;
    switch (header[21]) {
        case 0x30435445u: /* ETC0 */
        case 0x31545844u: bpp = 4; break; /* DXT1 */
        case 0x33545844u: /* DXT3 */
        case 0x35545844u: bpp = 8; break; /* DXT5 */
        default: return 0;
    }
    unsigned width = header[4], height = header[3], count = header[7];
    if (!width || !height) return 0;
    unsigned full_count = 1;
    for (unsigned maximum = width > height ? width : height; maximum > 1; maximum >>= 1)
        ++full_count;
    if (count > 1 && count != full_count) return 0;
    unsigned selected = texture_select_dds_dimensions(&width, &height, count, cap, header[2]);
    uint64_t begin = 128;
    width = header[4]; height = header[3];
    for (unsigned level = 0; level < selected; ++level) {
        uint64_t level_size = (uint64_t)width * height / (8u / bpp);
        if (begin >= file_size || level_size >= file_size - begin) return 0;
        begin += level_size;
        width = width > 1 ? width >> 1 : 1;
        height = height > 1 ? height >> 1 : 1;
    }
    if (begin >= file_size) return 0;
    *offset = (unsigned)begin;
    *size = file_size - *offset;
    return 1;
}

#endif
