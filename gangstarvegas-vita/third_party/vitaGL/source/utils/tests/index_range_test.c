#include "../index_range.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "index range test failed: %s\n", message); \
            return 1; \
        } \
    } while (0)

static int test_u16_range_and_rebase(void) {
    static const uint16_t source[] = {65000u, 65535u, 65000u, 65001u};
    static const uint16_t expected[] = {0u, 535u, 0u, 1u};
    uint16_t rebased[sizeof(source) / sizeof(source[0])];
    uint16_t original[sizeof(source) / sizeof(source[0])];
    uint32_t highest = 0;

    memcpy(original, source, sizeof(source));
    CHECK(vgl_index_range(source, 4u, 0, &highest) == 65000u,
          "U16 lowest index");
    CHECK(highest == 65535u, "U16 highest index");

    vgl_rebase_indices(rebased, source, 4u, 0, 65000u);
    CHECK(memcmp(rebased, expected, sizeof(expected)) == 0,
          "U16 rebased values");
    CHECK(memcmp(source, original, sizeof(source)) == 0,
          "U16 source EBO remains unchanged");
    return 0;
}

static int test_u32_range_and_rebase(void) {
    static const uint32_t source[] = {
        0xf0000000u, 0xf0000100u, 0xf0000000u, 0xf0000001u,
    };
    static const uint32_t expected[] = {0u, 0x100u, 0u, 1u};
    uint32_t rebased[sizeof(source) / sizeof(source[0])];
    uint32_t original[sizeof(source) / sizeof(source[0])];
    uint32_t highest = 0;

    memcpy(original, source, sizeof(source));
    CHECK(vgl_index_range(source, 4u, 1, &highest) == 0xf0000000u,
          "U32 lowest index");
    CHECK(highest == 0xf0000100u, "U32 highest index");

    vgl_rebase_indices(rebased, source, 4u, 1, 0xf0000000u);
    CHECK(memcmp(rebased, expected, sizeof(expected)) == 0,
          "U32 rebased values");
    CHECK(memcmp(source, original, sizeof(source)) == 0,
          "U32 source EBO remains unchanged");
    return 0;
}

static int test_zero_and_boundary_ranges(void) {
    static const uint16_t zero_based[] = {0u, 4u, 0u};
    static const uint32_t max_value[] = {UINT32_MAX};
    uint16_t rebased[3];
    uint32_t highest = 123u;

    CHECK(vgl_index_range(NULL, 0u, 0, &highest) == UINT32_MAX,
          "empty range returns sentinel minimum");
    CHECK(highest == 0u, "empty range resets maximum");
    CHECK(vgl_index_range(zero_based, 3u, 0, &highest) == 0u,
          "zero is retained as the minimum");
    CHECK(highest == 4u, "zero based maximum");
    vgl_rebase_indices(rebased, zero_based, 3u, 0, 0u);
    CHECK(memcmp(rebased, zero_based, sizeof(zero_based)) == 0,
          "rebasing a zero based range is identity");
    CHECK(vgl_index_range(max_value, 1u, 1, &highest) == UINT32_MAX,
          "U32 maximum boundary minimum");
    CHECK(highest == UINT32_MAX, "U32 maximum boundary maximum");
    return 0;
}

static int test_geometry_equivalence_for_layouts(void) {
    enum {
        vertex_count = 8,
        first_index = 2,
        highest_index = 5,
        compact_count = highest_index - first_index + 1,
        packed_stride = 11,
        position_stride = 12,
        normalized_u8_stride = 7,
        value_stride = 4,
    };
    uint8_t packed[vertex_count][packed_stride];
    uint8_t packed_compact[compact_count][packed_stride];
    uint8_t positions[vertex_count][position_stride];
    uint8_t positions_compact[compact_count][position_stride];
    uint8_t normalized_u8[vertex_count][normalized_u8_stride];
    uint8_t normalized_u8_compact[compact_count][normalized_u8_stride];
    uint8_t values[vertex_count][value_stride];
    uint8_t values_compact[compact_count][value_stride];
    const uint16_t indices[] = {2u, 5u, 2u, 4u};
    uint16_t rebased[sizeof(indices) / sizeof(indices[0])];
    uint32_t highest = 0;

    for (uint32_t i = 0; i < vertex_count; ++i) {
        for (uint32_t byte = 0; byte < packed_stride; ++byte)
            packed[i][byte] = (uint8_t)(10u * i + byte);
        for (uint32_t byte = 0; byte < position_stride; ++byte)
            positions[i][byte] = (uint8_t)(20u * i + byte);
        for (uint32_t byte = 0; byte < normalized_u8_stride; ++byte)
            normalized_u8[i][byte] = (uint8_t)(30u * i + byte);
        for (uint32_t byte = 0; byte < value_stride; ++byte)
            values[i][byte] = (uint8_t)(40u * i + byte);
    }

    CHECK(vgl_index_range(indices, 4u, 0, &highest) == first_index,
          "geometry range minimum");
    CHECK(highest == highest_index, "geometry range maximum");
    vgl_rebase_indices(rebased, indices, 4u, 0, first_index);

    for (uint32_t i = 0; i < compact_count; ++i) {
        memcpy(packed_compact[i], packed[first_index + i], packed_stride);
        memcpy(positions_compact[i], positions[first_index + i],
               position_stride);
        memcpy(normalized_u8_compact[i], normalized_u8[first_index + i],
               normalized_u8_stride);
        memcpy(values_compact[i], values[first_index + i], value_stride);
    }

    for (uint32_t i = 0; i < 4u; ++i) {
        uint32_t original = indices[i];
        uint32_t compact = rebased[i];
        CHECK(original == first_index + compact, "rebased geometry index");
        CHECK(memcmp(packed[original], packed_compact[compact], packed_stride) == 0,
              "packed copied span");
        CHECK(memcmp(positions[original], positions_compact[compact],
                     position_stride) == 0,
              "planar position copied span");
        CHECK(memcmp(normalized_u8[original], normalized_u8_compact[compact],
                     normalized_u8_stride) == 0,
              "planar normalized U8 copied span");
        CHECK(memcmp(values[original], values_compact[compact], value_stride) == 0,
              "planar value copied span");
    }
    return 0;
}

static int test_expanded_count_boundaries(void) {
    uint32_t expanded = 0xdeadbeefu;

#define CHECK_COUNT(count, primitive, wide, expected_ok, expected_size) \
    do { \
        expanded = 0xdeadbeefu; \
        int ok = vgl_expanded_index_count((count), (primitive), (wide), \
                                          &expanded); \
        CHECK(ok == (expected_ok), "expanded count validity"); \
        if (expected_ok) \
            CHECK(expanded == (expected_size), "expanded count size"); \
        else \
            CHECK(expanded == 0xdeadbeefu, "failed count does not write size"); \
    } while (0)

    CHECK_COUNT(0u, VGL_INDEX_NATIVE, 0, 0, 0u);
    CHECK_COUNT(1u, VGL_INDEX_NATIVE, 0, 1, 1u);
    CHECK_COUNT(INT32_MAX, VGL_INDEX_NATIVE, 0, 1, INT32_MAX);
    CHECK_COUNT(INT32_MAX, VGL_INDEX_NATIVE, 1, 0, 0u);
    CHECK_COUNT((uint32_t)INT32_MAX + 1u, VGL_INDEX_NATIVE, 0, 0, 0u);

    CHECK_COUNT(3u, VGL_INDEX_QUADS, 0, 0, 0u);
    CHECK_COUNT(4u, VGL_INDEX_QUADS, 0, 1, 6u);
    CHECK_COUNT(0x55555554u, VGL_INDEX_QUADS, 0, 1, 2147483646u);
    CHECK_COUNT(0x55555554u, VGL_INDEX_QUADS, 1, 0, 0u);
    CHECK_COUNT(0x55555558u, VGL_INDEX_QUADS, 0, 0, 0u);

    CHECK_COUNT(1u, VGL_INDEX_LINE_STRIP, 0, 0, 0u);
    CHECK_COUNT(2u, VGL_INDEX_LINE_STRIP, 0, 1, 2u);
    CHECK_COUNT(0x40000000u, VGL_INDEX_LINE_STRIP, 0, 1, 2147483646u);
    CHECK_COUNT(0x40000000u, VGL_INDEX_LINE_STRIP, 1, 0, 0u);
    CHECK_COUNT(0x40000001u, VGL_INDEX_LINE_STRIP, 0, 0, 0u);

    CHECK_COUNT(1u, VGL_INDEX_LINE_LOOP, 0, 0, 0u);
    CHECK_COUNT(2u, VGL_INDEX_LINE_LOOP, 0, 1, 4u);
    CHECK_COUNT(0x3fffffffu, VGL_INDEX_LINE_LOOP, 0, 1, 2147483646u);
    CHECK_COUNT(0x3fffffffu, VGL_INDEX_LINE_LOOP, 1, 0, 0u);
    CHECK_COUNT(0x40000000u, VGL_INDEX_LINE_LOOP, 0, 0, 0u);

#undef CHECK_COUNT
    return 0;
}

static int test_expand_quads(void) {
    static const uint16_t u16_source[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
    static const uint16_t u16_expected[] = {
        0u, 1u, 3u, 1u, 2u, 3u, 4u, 5u, 7u, 5u, 6u, 7u,
    };
    static const uint32_t u32_source[] = {
        65536u, 65537u, 65538u, 65539u,
        65540u, 65541u, 65542u, 65543u,
    };
    static const uint32_t u32_expected[] = {
        0u, 1u, 3u, 1u, 2u, 3u, 4u, 5u, 7u, 5u, 6u, 7u,
    };
    static const uint16_t u16_high_source[] = {
        65000u, 65001u, 65002u, 65003u,
        65004u, 65005u, 65006u, 65007u,
    };
    static const uint32_t u32_zero_source[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
    uint16_t u16_dest[12];
    uint16_t u16_high_dest[12];
    uint32_t u32_dest[12];
    uint32_t u32_zero_dest[12];
    uint16_t u16_original[8];
    uint32_t u32_original[8];

    memcpy(u16_original, u16_source, sizeof(u16_source));
    memcpy(u32_original, u32_source, sizeof(u32_source));
    vgl_expand_rebased_indices(u16_dest, u16_source, 8u, 0,
                               VGL_INDEX_QUADS, 0u);
    vgl_expand_rebased_indices(u32_dest, u32_source, 8u, 1,
                               VGL_INDEX_QUADS, 65536u);
    vgl_expand_rebased_indices(u16_high_dest, u16_high_source, 8u, 0,
                               VGL_INDEX_QUADS, 65000u);
    vgl_expand_rebased_indices(u32_zero_dest, u32_zero_source, 8u, 1,
                               VGL_INDEX_QUADS, 0u);
    CHECK(memcmp(u16_dest, u16_expected, sizeof(u16_expected)) == 0,
          "U16 two-quad winding");
    CHECK(memcmp(u32_dest, u32_expected, sizeof(u32_expected)) == 0,
          "U32 high-bias two-quad winding");
    CHECK(memcmp(u16_high_dest, u16_expected, sizeof(u16_expected)) == 0,
          "U16 high-bias two-quad winding");
    CHECK(memcmp(u32_zero_dest, u32_expected, sizeof(u32_expected)) == 0,
          "U32 zero-bias two-quad winding");
    CHECK(memcmp(u16_source, u16_original, sizeof(u16_source)) == 0,
          "U16 quad input remains unchanged");
    CHECK(memcmp(u32_source, u32_original, sizeof(u32_source)) == 0,
          "U32 quad input remains unchanged");
    return 0;
}

static int test_expand_strips_and_loops(void) {
    static const uint16_t strip_source[] = {0u, 2u, 5u, 9u};
    static const uint16_t strip_expected[] = {0u, 2u, 2u, 5u, 5u, 9u};
    static const uint32_t strip_expected_wide[] = {0u, 2u, 2u, 5u, 5u, 9u};
    static const uint16_t strip_high_source[] = {65000u, 65002u, 65005u, 65009u};
    static const uint32_t strip_zero_source[] = {0u, 2u, 5u, 9u};
    static const uint32_t strip_wide_source[] = {
        65536u, 65538u, 65541u, 65545u,
    };
    static const uint32_t loop_source[] = {65536u, 65540u, 65546u};
    static const uint32_t loop_expected[] = {0u, 4u, 4u, 10u, 10u, 0u};
    static const uint16_t loop_high_source[] = {65000u, 65004u, 65010u};
    static const uint16_t loop_expected_narrow[] = {0u, 4u, 4u, 10u, 10u, 0u};
    static const uint32_t loop_zero_source[] = {0u, 4u, 10u};
    static const uint16_t minimal_loop[] = {7u, 12u};
    static const uint16_t minimal_expected[] = {0u, 5u, 5u, 0u};
    uint16_t strip_dest[6];
    uint16_t strip_high_dest[6];
    uint32_t loop_dest[6];
    uint32_t strip_zero_dest[6];
    uint32_t strip_wide_dest[6];
    uint16_t loop_high_dest[6];
    uint32_t loop_zero_dest[6];
    uint16_t minimal_dest[4];

    vgl_expand_rebased_indices(strip_dest, strip_source, 4u, 0,
                               VGL_INDEX_LINE_STRIP, 0u);
    vgl_expand_rebased_indices(strip_high_dest, strip_high_source, 4u, 0,
                               VGL_INDEX_LINE_STRIP, 65000u);
    vgl_expand_rebased_indices(strip_zero_dest, strip_zero_source, 4u, 1,
                               VGL_INDEX_LINE_STRIP, 0u);
    vgl_expand_rebased_indices(strip_wide_dest, strip_wide_source, 4u, 1,
                               VGL_INDEX_LINE_STRIP, 65536u);
    vgl_expand_rebased_indices(loop_dest, loop_source, 3u, 1,
                               VGL_INDEX_LINE_LOOP, 65536u);
    vgl_expand_rebased_indices(loop_high_dest, loop_high_source, 3u, 0,
                               VGL_INDEX_LINE_LOOP, 65000u);
    vgl_expand_rebased_indices(loop_zero_dest, loop_zero_source, 3u, 1,
                               VGL_INDEX_LINE_LOOP, 0u);
    vgl_expand_rebased_indices(minimal_dest, minimal_loop, 2u, 0,
                               VGL_INDEX_LINE_LOOP, 7u);
    CHECK(memcmp(strip_dest, strip_expected, sizeof(strip_expected)) == 0,
          "U16 line-strip pairs");
    CHECK(memcmp(loop_dest, loop_expected, sizeof(loop_expected)) == 0,
          "U32 high-bias line-loop closure");
    CHECK(memcmp(strip_high_dest, strip_expected, sizeof(strip_expected)) == 0,
          "U16 high-bias line-strip pairs");
    CHECK(memcmp(strip_zero_dest, strip_expected_wide,
                 sizeof(strip_expected_wide)) == 0,
          "U32 zero-bias line-strip pairs");
    CHECK(memcmp(strip_wide_dest, strip_expected_wide,
                 sizeof(strip_expected_wide)) == 0,
          "U32 high-bias line-strip pairs");
    CHECK(memcmp(loop_high_dest, loop_expected_narrow,
                 sizeof(loop_expected_narrow)) == 0,
          "U16 high-bias line-loop closure");
    CHECK(memcmp(loop_zero_dest, loop_expected, sizeof(loop_expected)) == 0,
          "U32 zero-bias line-loop closure");
    CHECK(memcmp(minimal_dest, minimal_expected, sizeof(minimal_expected)) == 0,
          "minimal two-index line-loop closure");
    return 0;
}

static void widen_u8_indices_to_u16(uint16_t *dest, const uint8_t *source,
                                    uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        dest[i] = source[i];
}

static int test_normalized_u8_u16_emitter(void) {
    static const uint8_t quad_u8[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
    static const uint8_t quad_u8_bias[] = {
        10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u,
    };
    static const uint8_t strip_u8[] = {0u, 2u, 5u, 9u};
    static const uint8_t strip_u8_bias[] = {20u, 22u, 25u, 29u};
    static const uint8_t loop_u8[] = {0u, 4u, 10u};
    static const uint8_t loop_u8_bias[] = {30u, 34u, 40u};
    static const uint16_t quad_expected[] = {
        0u, 1u, 3u, 1u, 2u, 3u, 4u, 5u, 7u, 5u, 6u, 7u,
    };
    static const uint16_t strip_expected[] = {0u, 2u, 2u, 5u, 5u, 9u};
    static const uint16_t loop_expected[] = {0u, 4u, 4u, 10u, 10u, 0u};
    uint8_t quad_original[sizeof(quad_u8)];
    uint8_t quad_bias_original[sizeof(quad_u8_bias)];
    uint8_t strip_original[sizeof(strip_u8)];
    uint8_t strip_bias_original[sizeof(strip_u8_bias)];
    uint8_t loop_original[sizeof(loop_u8)];
    uint8_t loop_bias_original[sizeof(loop_u8_bias)];
    uint16_t quad[8], quad_bias[8], strip[4], strip_bias[4], loop[3], loop_bias[3];
    uint16_t quad_dest[12], quad_bias_dest[12];
    uint16_t strip_dest[6], strip_bias_dest[6];
    uint16_t loop_dest[6], loop_bias_dest[6];

    memcpy(quad_original, quad_u8, sizeof(quad_u8));
    memcpy(quad_bias_original, quad_u8_bias, sizeof(quad_u8_bias));
    memcpy(strip_original, strip_u8, sizeof(strip_u8));
    memcpy(strip_bias_original, strip_u8_bias, sizeof(strip_u8_bias));
    memcpy(loop_original, loop_u8, sizeof(loop_u8));
    memcpy(loop_bias_original, loop_u8_bias, sizeof(loop_u8_bias));
    widen_u8_indices_to_u16(quad, quad_u8, 8u);
    widen_u8_indices_to_u16(quad_bias, quad_u8_bias, 8u);
    widen_u8_indices_to_u16(strip, strip_u8, 4u);
    widen_u8_indices_to_u16(strip_bias, strip_u8_bias, 4u);
    widen_u8_indices_to_u16(loop, loop_u8, 3u);
    widen_u8_indices_to_u16(loop_bias, loop_u8_bias, 3u);
    vgl_expand_rebased_indices(quad_dest, quad, 8u, 0, VGL_INDEX_QUADS, 0u);
    vgl_expand_rebased_indices(quad_bias_dest, quad_bias, 8u, 0,
                               VGL_INDEX_QUADS, 10u);
    vgl_expand_rebased_indices(strip_dest, strip, 4u, 0,
                               VGL_INDEX_LINE_STRIP, 0u);
    vgl_expand_rebased_indices(strip_bias_dest, strip_bias, 4u, 0,
                               VGL_INDEX_LINE_STRIP, 20u);
    vgl_expand_rebased_indices(loop_dest, loop, 3u, 0,
                               VGL_INDEX_LINE_LOOP, 0u);
    vgl_expand_rebased_indices(loop_bias_dest, loop_bias, 3u, 0,
                               VGL_INDEX_LINE_LOOP, 30u);
    CHECK(memcmp(quad_dest, quad_expected, sizeof(quad_expected)) == 0,
          "normalized U8 quads after direct U16 widening");
    CHECK(memcmp(quad_bias_dest, quad_expected, sizeof(quad_expected)) == 0,
          "normalized U8 biased quads after direct U16 widening");
    CHECK(memcmp(strip_dest, strip_expected, sizeof(strip_expected)) == 0,
          "normalized U8 strip after direct U16 widening");
    CHECK(memcmp(strip_bias_dest, strip_expected, sizeof(strip_expected)) == 0,
          "normalized U8 biased strip after direct U16 widening");
    CHECK(memcmp(loop_dest, loop_expected, sizeof(loop_expected)) == 0,
          "normalized U8 loop after direct U16 widening");
    CHECK(memcmp(loop_bias_dest, loop_expected, sizeof(loop_expected)) == 0,
          "normalized U8 biased loop after direct U16 widening");
    CHECK(memcmp(quad_u8, quad_original, sizeof(quad_u8)) == 0,
          "normalized U8 quad input remains unchanged");
    CHECK(memcmp(quad_u8_bias, quad_bias_original, sizeof(quad_u8_bias)) == 0,
          "normalized U8 biased quad input remains unchanged");
    CHECK(memcmp(strip_u8, strip_original, sizeof(strip_u8)) == 0,
          "normalized U8 strip input remains unchanged");
    CHECK(memcmp(strip_u8_bias, strip_bias_original, sizeof(strip_u8_bias)) == 0,
          "normalized U8 biased strip input remains unchanged");
    CHECK(memcmp(loop_u8, loop_original, sizeof(loop_u8)) == 0,
          "normalized U8 loop input remains unchanged");
    CHECK(memcmp(loop_u8_bias, loop_bias_original, sizeof(loop_u8_bias)) == 0,
          "normalized U8 biased loop input remains unchanged");
    return 0;
}

static int test_gxp_program_flags(void) {
    uint32_t normal[6] = {0x00505847u, 0u, 24u, 0u, 0u, 0u};
    uint32_t candidate[6];

    normal[0] = 0x00505847u;
    ((uint8_t *)normal)[4] = 1u;
    ((uint8_t *)normal)[5] = 4u;
    CHECK(vgl_program_allows_index_rebase(normal), "normal GXP program");
    CHECK(!vgl_program_allows_index_rebase(NULL), "null GXP program");

    memcpy(candidate, normal, sizeof(candidate));
    candidate[0] = 0u;
    CHECK(!vgl_program_allows_index_rebase(candidate), "unknown GXP format");

    memcpy(candidate, normal, sizeof(candidate));
    candidate[5] = 1u;
    CHECK(!vgl_program_allows_index_rebase(candidate), "fragment shader input");

    memcpy(candidate, normal, sizeof(candidate));
    candidate[5] = 1u << 17;
    CHECK(!vgl_program_allows_index_rebase(candidate), "INDEX shader input");

    memcpy(candidate, normal, sizeof(candidate));
    candidate[5] = 1u << 18;
    CHECK(!vgl_program_allows_index_rebase(candidate), "INSTANCE shader input");
    return 0;
}

int main(void) {
    if (test_u16_range_and_rebase() != 0 ||
        test_u32_range_and_rebase() != 0 ||
        test_zero_and_boundary_ranges() != 0 ||
        test_geometry_equivalence_for_layouts() != 0 ||
        test_expanded_count_boundaries() != 0 ||
        test_expand_quads() != 0 ||
        test_expand_strips_and_loops() != 0 ||
        test_normalized_u8_u16_emitter() != 0 ||
        test_gxp_program_flags() != 0) {
        return 1;
    }
    return 0;
}
