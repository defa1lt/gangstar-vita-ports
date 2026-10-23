#include "texture_mip.h"
#include <assert.h>

static void check_upload(unsigned width, unsigned height, unsigned count, unsigned cap,
                         unsigned available, unsigned expected,
                         unsigned selected_width, unsigned selected_height) {
    unsigned uploads = 0;
    for (unsigned level = 0; level < available; ++level) {
        unsigned maximum = width > height ? width : height;
        if (level == texture_select_mip_level(maximum, level, count, cap)) {
            assert(level == expected);
            assert(width == selected_width && height == selected_height);
            ++uploads;
        }
        width = width > 1 ? width >> 1 : 1;
        height = height > 1 ? height >> 1 : 1;
    }
    assert(uploads == 1);
}

static void check_dds(unsigned width, unsigned height, unsigned count, unsigned cap,
                      unsigned flags, unsigned expected, unsigned target_width,
                      unsigned target_height) {
    assert(texture_select_dds_dimensions(&width, &height, count, cap, flags) == expected);
    assert(width == target_width && height == target_height);
}

static void check_payload_range(void) {
    uint32_t header[32] = {0x20534444u, 124, 0x21007, 2048, 2048, 0, 0, 12};
    header[19] = 32; header[20] = 4; header[21] = 0x30435445u;
    unsigned offset, size;
    assert(texture_dds_payload_range(header, 2796344, 512, &offset, &size));
    assert(offset == 2621568 && size == 174776);
    assert(!texture_dds_payload_range(header, 2621568, 512, &offset, &size));
    header[21] = 0x35545844u;
    assert(texture_dds_payload_range(header, 5592560, 512, &offset, &size));
    assert(offset == 5243008 && size == 349552);
    header[28] = 0x200;
    assert(!texture_dds_payload_range(header, 5592560, 512, &offset, &size));
    header[28] = 0x200000;
    assert(!texture_dds_payload_range(header, 5592560, 512, &offset, &size));
    header[28] = 0; header[6] = 4;
    assert(!texture_dds_payload_range(header, 5592560, 512, &offset, &size));
    header[6] = 1; header[7] = 2;
    assert(!texture_dds_payload_range(header, 5592560, 512, &offset, &size));
    header[7] = 12; header[21] = 0;
    assert(!texture_dds_payload_range(header, 5592560, 512, &offset, &size));
    header[21] = 0x30435445u; header[2] |= 8;
    assert(texture_dds_payload_range(header, 2796344, 512, &offset, &size));
    assert(offset == 128 && size == 2796216);
    header[2] &= ~8u; header[3] = 4;
    assert(texture_dds_payload_range(header, 2796344, 512, &offset, &size));
    assert(offset == 128);
    header[0] = 0;
    assert(!texture_dds_payload_range(header, 2796344, 512, &offset, &size));
}

int main(void) {
    check_payload_range();
    check_upload(1024, 1024, 0, 512, 1, 0, 1024, 1024);
    check_upload(1024, 1024, 1, 512, 1, 0, 1024, 1024);
    check_upload(2048, 1024, 2, 512, 2, 1, 1024, 512);
    check_upload(4096, 4096, 2, 512, 2, 1, 2048, 2048);
    check_upload(2048, 2048, 12, 512, 12, 2, 512, 512);
    check_upload(512, 256, 10, 512, 10, 0, 512, 256);
    check_upload(2048, 2048, 0, 512, 12, 0, 2048, 2048);
    check_upload(2048, 2048, 12, 1024, 12, 1, 1024, 1024);
    check_upload(2048, 2048, 12, 2048, 12, 0, 2048, 2048);
    check_upload(4096, 2048, 12, 2048, 12, 1, 2048, 1024);
    check_upload(2048, 2048, 12, 0, 12, 0, 2048, 2048);
    check_dds(2048, 2048, 12, 512, 0x21007, 2, 512, 512);
    check_dds(2048, 1024, 12, 512, 0x21007, 2, 512, 256);
    check_dds(2048, 2048, 12, 2048, 0x21007, 0, 2048, 2048);
    check_dds(2048, 2048, 12, 512, 0x2100f, 0, 2048, 2048);
    check_dds(2048, 2048, 1, 512, 0x21007, 0, 2048, 2048);
    check_dds(2048, 2048, 2, 512, 0x21007, 1, 1024, 1024);
    check_dds(1024, 4, 11, 512, 0x21007, 0, 1024, 4);
    check_dds(1024, 12, 11, 512, 0x21007, 0, 1024, 12);
    check_dds(2048, 4, 12, 512, 0x21007, 0, 2048, 4);
    check_dds(2048, 2, 12, 512, 0x21007, 0, 2048, 2);
    check_dds(1025, 1024, 11, 512, 0x21007, 0, 1025, 1024);
    check_dds(512, 256, 10, 512, 0x21007, 0, 512, 256);
    check_dds(0, 2048, 12, 512, 0x21007, 0, 0, 2048);
    check_dds(2048, 2048, 12, 0, 0x21007, 0, 2048, 2048);
    assert(texture_dds_max_dimension(512, 0) == 512);
    assert(texture_dds_max_dimension(512, 1) == 1024);
    assert(texture_dds_max_dimension(512, 2) == 2048);
    assert(texture_dds_max_dimension(512, -1) == UINT32_MAX);
    assert(texture_dds_max_dimension(512, 23) == UINT32_MAX);
    assert(texture_dds_max_dimension(512, 32) == UINT32_MAX);
    check_dds(2048, 2048, 12, texture_dds_max_dimension(512, 1),
              0x21007, 1, 1024, 1024);
    check_dds(1024, 1024, 11, texture_dds_max_dimension(512, 1),
              0x21007, 0, 1024, 1024);
    return 0;
}
