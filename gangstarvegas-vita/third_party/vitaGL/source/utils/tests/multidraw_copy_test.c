#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct { void *ptr; unsigned last_frame; } vbo;
typedef struct {
    unsigned vertex_attrib_state;
    uintptr_t vertex_attrib_vbo[1], vertex_attrib_offsets[1];
} vao;
static vao *cur_vao;
static unsigned vgl_framecount;
/* This fixture exercises CPU copying only, using the real host allocator. */
#define gpu_alloc_mapped_temp malloc
#define vgl_fast_memcpy memcpy
#define disable_draw_attrib(i) abort();
#include "multidraw_copy_under_test.h"

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    unsigned char *pages = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pages != MAP_FAILED);
    assert(mprotect(pages + page, (size_t)page, PROT_NONE) == 0);
    for (unsigned stride = 24; stride <= 28; stride += 4) {
        unsigned char *source = pages + page - 14 * stride;
        for (unsigned j = 0; j < 14 * stride; ++j) source[j] = (unsigned char)j;
        vao input = {1, {0}, {(uintptr_t)source}};
        cur_vao = &input;
        for (unsigned branch = 0; branch < 2; ++branch) {
            struct { unsigned stride; } streams[1] = {{stride}};
            struct { unsigned offset; } attributes[1] = {{99}};
            void *ptrs[1] = {NULL};
            unsigned i = 0, attr_idx = 0, lowest = 10, highest = 14;
            if (branch == 0) { TEST_MULTIDRAW_COPY_0(); }
            else { TEST_MULTIDRAW_COPY_1(); }
            assert(ptrs[0] && attributes[0].offset == 0);
            assert(memcmp(ptrs[0], source + 10 * stride, 4 * stride) == 0);
            free(ptrs[0]);
        }
    }
    assert(munmap(pages, (size_t)page * 2) == 0);
    return 0;
}
