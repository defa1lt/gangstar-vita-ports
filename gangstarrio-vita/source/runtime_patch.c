/* Android kuser helper redirection adapted from 9mm-vita's MIT-licensed
 * runtime patch. ARM Android binaries call fixed kernel helper addresses that
 * are unmapped on the PS Vita, so provide equivalent code on a local RX page
 * and rewrite the address literals before any game code can reach them. */

#include "runtime_patch.h"

#include "diagnostics.h"
#include "controller_labels.h"

#include <kubridge.h>
#include <reimpl/io.h>
#include <reimpl/pthr.h>
#include <reimpl/sys.h>
#include <utils/glutil.h>
#include <utils/texture_mip.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <vitashark.h>
#include <pthread.h>
#ifdef HAVE_SHARK_LOG
#include <vitaGL.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#endif
#include <stdlib.h>

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX ((SceKernelMemBlockType)0x0C20D050)
#define KUSER_PAGE_ADDRESS ((uintptr_t)0x9A000000)
#define KUSER_MEMORY_BARRIER_ANDROID ((uint32_t)0xFFFF0FA0)
#define KUSER_CMPXCHG_ANDROID ((uint32_t)0xFFFF0FC0)
#define KUSER_MEMORY_BARRIER_VITA ((uint32_t)0x9A000FA0)
#define KUSER_CMPXCHG_VITA ((uint32_t)0x9A000FC0)
#define EXPECTED_MEMORY_BARRIER_LITERALS 4
#define EXPECTED_CMPXCHG_LITERALS 43

#ifdef HAVE_SHARK_LOG
static void (*hud_batch_original)(const void *);
static void (*hud_element_original)(const void *);
static uintptr_t hud_batch_returns[2];
static const unsigned char *hud_optimized;
static unsigned hud_batch_samples[2], hud_element_samples, hud_matrix_samples[3];
static unsigned hud_world_seen;
static void (*map_draw_original)(const void *);
static __thread const void *hud_draw_self;
static __thread unsigned hud_draw_kind;
static unsigned hud_draw_samples[3];
typedef struct {
    GLint location;
    GLsizei count;
    GLboolean transpose;
    unsigned sequence;
    uint32_t words[16];
} HudMatrix;
static __thread HudMatrix hud_recent_matrices[32];
static __thread unsigned hud_recent_matrix_count;

static void hud_log_matrix(const char *source, unsigned kind, GLint program,
                           const HudMatrix *matrix) {
    const uint32_t *w = matrix->words;
    LOG_INFO("HUD MATRIX: source=%s kind=%u program=%d location=%x count=%d transpose=%d last_observed_sequence=%u age_uploads=%u data=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x",
             source, kind, program, (unsigned)matrix->location, matrix->count, matrix->transpose,
             matrix->sequence, hud_recent_matrix_count - matrix->sequence,
             w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
             w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
}

static void hud_capture_matrix(GLint program, const char *name, unsigned kind) {
    GLint location = glGetUniformLocation(program, name);
    if (location == -1 || location == 0) return;
    unsigned count = hud_recent_matrix_count < 32 ? hud_recent_matrix_count : 32;
    for (unsigned i = 0; i < count; ++i) {
        const HudMatrix *matrix = &hud_recent_matrices[(hud_recent_matrix_count - i - 1) % 32];
        if (matrix->location == location) {
            hud_log_matrix(name, kind, program, matrix);
            return;
        }
    }
    LOG_INFO("HUD MATRIX: source=%s kind=%u program=%d location=%x recent_upload=absent",
             name, kind, program, (unsigned)location);
}

static void hud_draw_scoped(const void *self, unsigned kind,
                            void (*original)(const void *)) {
    const void *previous_self = hud_draw_self;
    unsigned previous_kind = hud_draw_kind;
    hud_draw_self = self;
    hud_draw_kind = kind;
    original(self);
    hud_draw_self = previous_self;
    hud_draw_kind = previous_kind;
}

static void hud_batch_hook(const void *self) {
    uintptr_t caller = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    if (__atomic_load_n(&hud_world_seen, __ATOMIC_RELAXED) &&
        (caller == hud_batch_returns[0] || caller == hud_batch_returns[1])) {
        unsigned path = caller == hud_batch_returns[1];
        unsigned vertices = *(const unsigned *)((const uint8_t *)self + 0x18);
        if (__atomic_fetch_add(&hud_batch_samples[path], 1, __ATOMIC_RELAXED) < 6)
            LOG_INFO("HUD BATCH: self=%p vertices=%u atlas=%p streams=%p caller=0x%x optimized=%u",
                     self, vertices, *(void *const *)((const uint8_t *)self + 4),
                     *(void *const *)((const uint8_t *)self + 0x14), (unsigned)caller,
                     (unsigned)*hud_optimized);
        hud_draw_scoped(self, 1, hud_batch_original);
    } else hud_batch_original(self);
}

static void hud_element_hook(const void *self) {
    if (__atomic_load_n(&hud_world_seen, __ATOMIC_RELAXED) &&
        __atomic_fetch_add(&hud_element_samples, 1, __ATOMIC_RELAXED) < 6)
        LOG_INFO("HUD ELEMENT: self=%p flags=%x frame=%d optimized=%u",
                 self, *(const unsigned *)((const uint8_t *)self + 0x24),
                 *(const int *)((const uint8_t *)self + 0x30), (unsigned)*hud_optimized);
    hud_draw_scoped(self, 1, hud_element_original);
}

static void map_draw_hook(const void *self) {
    hud_draw_scoped(self, 2, map_draw_original);
}

static void hud_capture_submit(GLenum mode, GLint first, GLsizei count,
                                GLenum index_type, const void *indices) {
    unsigned kind = hud_draw_kind;
    if (kind == 2) __atomic_store_n(&hud_world_seen, 1, __ATOMIC_RELAXED);
    if (kind && __atomic_load_n(&hud_world_seen, __ATOMIC_RELAXED) &&
        __atomic_fetch_add(&hud_draw_samples[kind], 1, __ATOMIC_RELAXED) < 6) {
        GLint program = 0, viewport[4], scissor[4], texture = 0, linked = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        LOG_INFO("HUD: kind=%u self=%p mode=%x first=%d count=%d program=%d texture=%d viewport=%d/%d/%d/%d scissor=%d:%d/%d/%d/%d",
                 kind, hud_draw_self, mode, first, count, program, texture,
                 viewport[0], viewport[1], viewport[2], viewport[3],
                 glIsEnabled(GL_SCISSOR_TEST), scissor[0], scissor[1], scissor[2], scissor[3]);
        if (program > 0) glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked) {
            GLint uniforms = 0;
            glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniforms);
            for (GLint i = 0; i < uniforms && i < 32; ++i) {
                GLchar name[128] = {0};
                GLsizei length = 0; GLint size = 0; GLenum type = 0;
                glGetActiveUniform(program, (GLuint)i, sizeof(name), &length, &size, &type, name);
                name[sizeof(name) - 1] = 0;
                LOG_INFO("HUD UNIFORM: kind=%u program=%d name=%s type=%x size=%d", kind, program, name, type, size);
                if (length > 0 && length < (GLsizei)sizeof(name) - 1 && type == GL_FLOAT_MAT4)
                    hud_capture_matrix(program, name, kind);
            }
            unsigned vertex_indices[6], vertex_count = 0;
            unsigned samples = count > 6 ? 6 : count > 0 ? (unsigned)count : 0;
            if (first >= 0) {
                for (unsigned i = 0; i < samples; ++i) vertex_indices[vertex_count++] = (unsigned)first + i;
            } else {
                GLint element_buffer = 0;
                glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
                unsigned width = index_type == GL_UNSIGNED_BYTE ? 1 :
                    index_type == GL_UNSIGNED_SHORT ? 2 : index_type == GL_UNSIGNED_INT ? 4 : 0;
                unsigned bytes = samples * width;
                if (!element_buffer && bytes && (uintptr_t)indices <= UINT32_MAX - bytes &&
                    sceKernelFindMemBlockByAddr(indices, bytes) >= 0) {
                    for (unsigned i = 0; i < samples; ++i) {
                        uint32_t index = 0;
                        memcpy(&index, (const uint8_t *)indices + i * width, width);
                        vertex_indices[vertex_count++] = index;
                        LOG_INFO("HUD INDEX: kind=%u sample=%u type=%x value=%u", kind, i, index_type, index);
                    }
                }
            }
            for (GLint location = 0; location < 16; ++location) {
                GLint enabled = 0, components = 0, stride = 0, format = 0, normalized = 0, buffer = 0;
                void *pointer = NULL;
                glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
                if (!enabled) continue;
                glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_SIZE, &components);
                glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
                glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_TYPE, &format);
                glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
                glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
                glGetVertexAttribPointerv(location, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
                LOG_INFO("HUD: location=%d enabled=%d components=%d stride=%d format=%x normalized=%d buffer=%x pointer=%p",
                         location, enabled, components, stride, format, normalized, buffer, pointer);
                unsigned width = format == GL_FLOAT ? 4u : format == GL_UNSIGNED_BYTE ? 1u : 0u;
                if (enabled && !buffer && width && components > 0 && components <= 4 &&
                    stride >= 0) for (unsigned i = 0; i < vertex_count; ++i) {
                    uint64_t address = (uintptr_t)pointer + (uint64_t)vertex_indices[i] *
                        (stride ? (unsigned)stride : (unsigned)components * width);
                    unsigned bytes = (unsigned)components * width;
                    if (address <= UINT32_MAX - bytes &&
                        sceKernelFindMemBlockByAddr((const void *)(uintptr_t)address, bytes) >= 0) {
                        uint32_t words[4] = {0};
                        memcpy(words, (const void *)(uintptr_t)address, bytes);
                        LOG_INFO("HUD VERTEX: kind=%u sample=%u index=%u location=%d bytes=%u data=%08x/%08x/%08x/%08x",
                                 kind, i, vertex_indices[i], location, bytes, words[0], words[1], words[2], words[3]);
                    }
                }
            }
        }
    }
}

void __real_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void __wrap_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    hud_capture_submit(mode, first, count, 0, NULL);
    __real_glDrawArrays(mode, first, count);
}

void __real_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
void __wrap_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    hud_capture_submit(mode, -1, count, type, indices);
    __real_glDrawElements(mode, count, type, indices);
}

void __real_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void __wrap_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    unsigned kind = hud_draw_kind;
    if (__atomic_load_n(&hud_world_seen, __ATOMIC_RELAXED) &&
        hud_recent_matrix_count < 8192 &&
        (__atomic_load_n(&hud_draw_samples[1], __ATOMIC_RELAXED) < 6 ||
         __atomic_load_n(&hud_draw_samples[2], __ATOMIC_RELAXED) < 6) &&
        location != -1 && location != 0 && count > 0 && value &&
        sceKernelFindMemBlockByAddr(value, 16 * sizeof(*value)) >= 0) {
        /* Default VitaGL locations encode live uniform pointers. This is the
         * last observed upload, not a query of current GPU uniform state. */
        HudMatrix *matrix = &hud_recent_matrices[hud_recent_matrix_count++ % 32];
        matrix->location = location; matrix->count = count; matrix->transpose = transpose;
        matrix->sequence = hud_recent_matrix_count;
        memcpy(matrix->words, value, sizeof(matrix->words));
        if (kind && __atomic_load_n(&hud_world_seen, __ATOMIC_RELAXED) &&
            __atomic_fetch_add(&hud_matrix_samples[kind], 1, __ATOMIC_RELAXED) < 8) {
            GLint program = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            hud_log_matrix("scoped", kind, program, matrix);
        }
    }
    __real_glUniformMatrix4fv(location, count, transpose, value);
}

#endif

static SceUID kuser_block = -1;

static void kuser_memory_barrier(void) {
    __sync_synchronize();
}

int runtime_patch_install_trampoline(uintptr_t address, uintptr_t target) {
    const uint32_t trampoline[2] = {
        0xE51FF004, /* ldr pc, [pc, #-4] */
        (uint32_t)target,
    };
    int ret = kuKernelCpuUnrestrictedMemcpy((void *)address, trampoline,
                                            sizeof(trampoline));
    if (ret >= 0) {
        kuKernelFlushCaches((void *)address, sizeof(trampoline));
    }
    return ret;
}

static uintptr_t immutable_continuation(so_module *module, uintptr_t address,
                                        const uint32_t prologue[2]) {
    if (!address || memcmp((const void *)address, prologue, 8) != 0) return 0;
    const uint32_t code[4] = {prologue[0], prologue[1], 0xe51ff004u, (uint32_t)(address + 8u)};
    uintptr_t original = so_alloc_arena(module, 0, 0, sizeof(code));
    if (!original || kuKernelCpuUnrestrictedMemcpy((void *)original, code, sizeof(code)) < 0)
        return 0;
    kuKernelFlushCaches((void *)original, sizeof(code));
    return original;
}

static int install_trampoline(uintptr_t address, uintptr_t target) {
    return runtime_patch_install_trampoline(address, target);
}

int runtime_patch_android_kuser(so_module *module) {
    if (!module || !module->text_base || !module->text_size) {
        LOG_ERROR("M2 kuser patch: invalid module text range");
        return -1;
    }

    SceKernelAllocMemBlockKernelOpt options;
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.attr = 0x1;
    options.field_C = (SceUInt32)KUSER_PAGE_ADDRESS;

    kuser_block = kuKernelAllocMemBlock(
        "rio-kuser", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &options);
    if (kuser_block < 0) {
        LOG_ERROR("M2 kuser patch: RX page allocation failed: 0x%08x",
                  kuser_block);
        return -1;
    }

    if (install_trampoline(KUSER_MEMORY_BARRIER_VITA,
                           (uintptr_t)&kuser_memory_barrier) < 0 ||
        install_trampoline(KUSER_CMPXCHG_VITA,
                           (uintptr_t)&__atomic_cmpxchg) < 0) {
        LOG_ERROR("M2 kuser patch: trampoline installation failed");
        return -1;
    }

    unsigned barriers = 0;
    unsigned compares = 0;
    uintptr_t end = module->text_base + module->text_size;
    for (uintptr_t address = module->text_base;
         address + sizeof(uint32_t) <= end; address += sizeof(uint32_t)) {
        uint32_t value = *(const uint32_t *)address;
        uint32_t replacement;
        if (value == KUSER_CMPXCHG_ANDROID) {
            replacement = KUSER_CMPXCHG_VITA;
            ++compares;
        } else if (value == KUSER_MEMORY_BARRIER_ANDROID) {
            replacement = KUSER_MEMORY_BARRIER_VITA;
            ++barriers;
        } else {
            continue;
        }

        if (kuKernelCpuUnrestrictedMemcpy((void *)address, &replacement,
                                          sizeof(replacement)) < 0) {
            LOG_ERROR("M2 kuser patch: write failed at 0x%08x",
                      (unsigned int)address);
            return -1;
        }
    }

    if (barriers != EXPECTED_MEMORY_BARRIER_LITERALS ||
        compares != EXPECTED_CMPXCHG_LITERALS) {
        LOG_ERROR("M2 kuser patch: profile mismatch barriers=%u cmpxchg=%u",
                  barriers, compares);
        return -1;
    }

    kuKernelFlushCaches((void *)KUSER_PAGE_ADDRESS, 0x1000);
    kuKernelFlushCaches((void *)module->text_base, module->text_size);
    LOG_SUCCESS("M2 Android kuser helpers redirected: barriers=%u cmpxchg=%u",
                barriers, compares);
    return 0;
}

static int stub_ret0(void) {
    return 0;
}

static int stub_curl_perform(void *curl) {
    (void)curl;
    return 7; /* CURLE_COULDNT_CONNECT */
}

static so_hook gs_main_menu_ctor_hook;
static so_hook gs_main_menu_draw_hook;
static so_hook gs_main_menu_update_hook;
static so_hook draw_player_hook;
static so_hook pink_material_hook;
static so_hook choose_perf_profile_hook;
static so_hook touch_moved_hook;
static so_hook load_mission_hook;
static so_hook load_reputation_hook;
static so_hook load_scripts_hook;
static so_hook load_dialog_sounds_hook;
static so_hook load_interrupt_conditions_hook;
static so_hook load_props_hook;
static so_hook load_highlights_hook;
static so_hook load_particles_hook;
static so_hook resolve_behavior_scripts_hook;
static so_hook control_manager_init_hook;
static so_hook train_manager_init_hook;
static so_hook hud_manager_enable_hook;
static so_hook loading_menu_update_hook;
static so_hook play_main_menu_music_hook;
static so_hook application_start_sound_thread_hook;
static so_hook sound_play_low_fps_music_hook;
static so_hook sound_init_hook;
static so_hook sound_pack_xml_hook;
static so_hook sound_data_source_hook;
static so_hook sound_stop_low_fps_music_hook;
static void (*city_grid_process_original)(void *, void *, unsigned);
static void (*city_lod_process_original)(void *, void *, unsigned);
static int (*texture_update_data_original)(void *, int);
static int (*dds_header_original)(const void *, void *, void *);
static int (*dds_read_header_original)(void *, void *);
typedef struct {
    const void *file;
    unsigned flags, mip_count;
    int valid;
} DdsHeaderCapture;
static __thread DdsHeaderCapture *dds_header_capture;
static unsigned dds_mip_selections;
static void (*graphics_task_push_original)(void *);
static const void *(*collada_get_material)(const void *, int);
static uintptr_t material_callback_run, material_enqueue_return;
static unsigned material_prefetch_count;
static int (*filesystem_add_original)(void *, const char *, int, int);
static int (*filesystem_remove_original)(void *, const char *);
static int (*filesystem_write_lock)(void *, unsigned);
static int (*filesystem_write_unlock)(void *);
static void *filesystem_lock;
static void *(*filesystem_string_assign)(void *, const void *);
static void (*filesystem_string_destroy)(void *);
static void *(*filesystem_string_assign_span)(void *, const char *, unsigned);
static unsigned filesystem_adds, filesystem_removes, filesystem_normalized, filesystem_mismatches;

static unsigned filesystem_folder_count(void *self, unsigned offset) {
    const uint32_t *header = (const uint32_t *)((const uint8_t *)self + offset);
    return (header[1] - header[0]) / 4u;
}

static void filesystem_erase_prefix(void *self, unsigned index) {
    uint32_t *header = (uint32_t *)((uint8_t *)self + 0x3c);
    uint32_t *begin = (uint32_t *)(uintptr_t)header[0];
    unsigned count = filesystem_folder_count(self, 0x3c);
    for (unsigned i = index; i + 1 < count; ++i)
        filesystem_string_assign(begin + i, begin + i + 1);
    filesystem_string_destroy(begin + count - 1);
    header[1] -= 4u;
}

static int filesystem_add_folder(void *self, const char *path,
                                 int ignore_case, int ignore_paths) {
    if (!filesystem_write_lock(filesystem_lock, 0)) {
        LOG_ERROR("FILESYSTEM: native write lock failed");
        return 0;
    }
    unsigned before = filesystem_folder_count(self, 0x20);
    int result = filesystem_add_original(self, path, ignore_case, ignore_paths);
    unsigned loose = filesystem_folder_count(self, 0x20);
    unsigned custom = filesystem_folder_count(self, 0x3c);
    ++filesystem_adds;
    if (result && loose == before + 1 && custom == before + 1) {
        uint32_t *begin = *(uint32_t **)((uint8_t *)self + 0x3c);
        const char *prefix = (const char *)(uintptr_t)begin[before];
        unsigned length = strlen(prefix), trimmed = length;
        if (trimmed && prefix[trimmed - 1] == '/') --trimmed;
        /* Native lookup appends '/'. Equivalent loose-reader aliases must
         * therefore own equivalent custom prefixes, including root -> "". */
        if (trimmed != length) {
            filesystem_string_assign_span(begin + before, prefix, trimmed);
            ++filesystem_normalized;
        }
    }
    if (!result && loose == before && custom == before + 1)
        filesystem_erase_prefix(self, before);
    if (filesystem_folder_count(self, 0x3c) != loose) {
        ++filesystem_mismatches;
        LOG_ERROR("FILESYSTEM: folder registration mismatch loose=%u custom=%u result=%d",
                  loose, filesystem_folder_count(self, 0x3c), result);
    }
    filesystem_write_unlock(filesystem_lock);
    return result;
}

static int filesystem_remove_archive(void *self, const char *path) {
    if (!filesystem_write_lock(filesystem_lock, 0)) {
        LOG_ERROR("FILESYSTEM: native write lock failed");
        return 0;
    }
    unsigned before = filesystem_folder_count(self, 0x20);
    unsigned custom = filesystem_folder_count(self, 0x3c);
    uint32_t *readers = *(uint32_t **)((uint8_t *)self + 0x20);
    unsigned index = before;
    for (unsigned i = 0; i < before; ++i) {
        const char *folder = *(const char **)((uintptr_t)readers[i] + 0x30);
        if (strcmp(path, folder) == 0) { index = i; break; }
    }
    int result = filesystem_remove_original(self, path);
    unsigned after = filesystem_folder_count(self, 0x20);
    ++filesystem_removes;
    /* The base method searches ZIP/PAK first. Only a loose-reader removal
     * owns a corresponding prefix; its normalized path identifies the index. */
    if (after + 1 == before && custom == before && index < before)
        filesystem_erase_prefix(self, index);
    if (filesystem_folder_count(self, 0x3c) != after) {
        ++filesystem_mismatches;
        LOG_ERROR("FILESYSTEM: folder removal mismatch loose=%u custom=%u result=%d",
                  after, filesystem_folder_count(self, 0x3c), result);
    }
    filesystem_write_unlock(filesystem_lock);
    return result;
}

static int filesystem_patch(so_module *module) {
    uintptr_t table = so_symbol(module, "_ZTV17CCustomFileSystem");
    uintptr_t add = so_symbol(module, "_ZN17CCustomFileSystem20addFolderFileArchiveEPKcbb");
    uintptr_t remove = so_symbol(module, "_ZN6glitch2io14CGlfFileSystem17removeFileArchiveEPKc");
    filesystem_lock = (void *)so_symbol(module, "_ZN6glitch2io14CGlfFileSystem6RWLockE");
    filesystem_write_lock = (int (*)(void *, unsigned))so_symbol(module,
        "_ZN3glf18ReadWriteMutexLock9writeLockEj");
    filesystem_write_unlock = (int (*)(void *))so_symbol(module,
        "_ZN3glf18ReadWriteMutexLock11writeUnlockEv");
    filesystem_string_assign = (void *(*)(void *, const void *))so_symbol(module,
        "_ZNSbIcSt11char_traitsIcEN6glitch4core10SAllocatorIcLNS1_6memory13E_MEMORY_HINTE0EEEEaSERKS7_");
    filesystem_string_destroy = (void (*)(void *))so_symbol(module,
        "_ZNSbIcSt11char_traitsIcEN6glitch4core10SAllocatorIcLNS1_6memory13E_MEMORY_HINTE0EEEED1Ev");
    filesystem_string_assign_span = (void *(*)(void *, const char *, unsigned))so_symbol(module,
        "_ZNSbIcSt11char_traitsIcEN6glitch4core10SAllocatorIcLNS1_6memory13E_MEMORY_HINTE0EEEE6assignEPKcj");
    if (!table || !add || !remove || !filesystem_lock ||
        !filesystem_write_lock || !filesystem_write_unlock ||
        !filesystem_string_assign || !filesystem_string_destroy || !filesystem_string_assign_span ||
        *(uintptr_t *)(table + 8 + 0x20) != add ||
        *(uintptr_t *)(table + 8 + 0x2c) != remove) {
        LOG_ERROR("PATCH: native filesystem method profile mismatch");
        return -1;
    }
    filesystem_add_original = (int (*)(void *, const char *, int, int))add;
    filesystem_remove_original = (int (*)(void *, const char *))remove;
    const uintptr_t replacements[2] = {
        (uintptr_t)&filesystem_add_folder, (uintptr_t)&filesystem_remove_archive
    };
    if (kuKernelCpuUnrestrictedMemcpy((void *)(table + 8 + 0x20),
            replacements, sizeof(uintptr_t)) < 0 ||
        kuKernelCpuUnrestrictedMemcpy((void *)(table + 8 + 0x2c),
            replacements + 1, sizeof(uintptr_t)) < 0) return -1;
    LOG_SUCCESS("PATCH: native temporary filesystem folders removed symmetrically");
    return 0;
}

#ifdef HAVE_SHARK_LOG
static so_hook sound_file_open_hook;
static so_hook sound_stream_open_hook;
static so_hook sound_file_stream_open_hook;
static so_hook sound_decoder_cursor_hook;
static void **sound_application_slot;
static void *(*sound_get_device)(void);
static uintptr_t sound_filesystem_vptr;
#endif
static void **phone_perf_slot;
static int (*profile_viewport_width)(void);
static int (*profile_viewport_height)(void);
static void **camera_instance_slot;
static so_module *task_game_module;
static unsigned task_set_count;
static uintptr_t task_done_address;
static uintptr_t cpu_task_vptr;
static uintptr_t graphics_task_vptr;
static unsigned task_run_count;
static unsigned task_done_skip_count;
static unsigned task_handler_reject_count;
static unsigned task_wait_count;
static unsigned condition_wait_count;
static unsigned condition_wait_error_count;
static unsigned condition_signal_count;
static unsigned condition_broadcast_count;
static unsigned condition_timeout_count;
static unsigned condition_fallback_count;
static unsigned task_handler_run_count;
static void *(*loading_state_machine_get_instance)(void);
static void *(*loading_state_machine_get_state)(void *state_machine);
static const char **loading_state_names;
static int *loading_percent_weights;
static int loading_last_stage = -2;
static int loading_last_progress = -2;
static void (*task_dispatch_address)(void *manager, void *handler,
                                     int wait_for_task);
static int (*task_keep_running_address)(void *manager, void *handler);
static int (*task_has_pending_address)(void *manager);

static const char *task_callback_name(uintptr_t address) {
    if (!task_game_module || !address) return "unknown";
    uintptr_t offset = (address & ~(uintptr_t)1) - task_game_module->text_base;
    switch (offset) {
        case 0x413e50: return "city-stream";
        case 0x413e48: return "city-material";
        case 0x79cb40: return "generate-mipmaps";
        case 0x79cb5c: return "image-loader";
        default: return "unknown";
    }
}

static void city_process_profile(void (*original)(void *, void *, unsigned),
                                 const char *name,
                                 void *self, void *context, unsigned flags) {
    IoPhysicalStats before, after;
    io_get_physical_stats(&before);
    uint64_t started = sceKernelGetSystemTimeWide();
    original(self, context, flags);
    unsigned elapsed = (unsigned)(sceKernelGetSystemTimeWide() - started);
    if (elapsed < 75000) return;
    io_get_physical_stats(&after);
    diagnostics_log("PERF",
                    "CITY %s tid=0x%08x elapsed=%u us pread_calls=%u pread_us=%u pread_bytes=%u",
                    name, (unsigned)sceKernelGetThreadId(), elapsed,
                    after.pread_calls - before.pread_calls,
                    after.pread_us - before.pread_us,
                    after.pread_bytes - before.pread_bytes);
}

static void city_grid_process_hook_func(void *self, void *context,
                                        unsigned flags) {
    city_process_profile(city_grid_process_original, "grid", self, context,
                         flags);
}

static void city_lod_process_hook_func(void *self, void *context,
                                       unsigned flags) {
    city_process_profile(city_lod_process_original, "lod", self, context,
                         flags);
}

static uint64_t load_profile_begin(const char *name) {
    uint64_t started = sceKernelGetSystemTimeWide();
    LOG_INFO("LOADPROF: begin %s", name);
    diagnostics_flush();
    return started;
}

static void load_profile_end(const char *name, uint64_t started) {
    uint64_t elapsed = sceKernelGetSystemTimeWide() - started;
    LOG_INFO("LOADPROF: end %s took=%llu ms", name,
             (unsigned long long)(elapsed / 1000));
    diagnostics_flush();
}

static void load_mission_hook_func(void *self, const char *path) {
    uint64_t started = load_profile_begin("Mission::load");
    SO_CONTINUE(int, load_mission_hook, self, path);
    load_profile_end("Mission::load", started);
}

static void load_reputation_hook_func(void *self, const char *path) {
    uint64_t started = load_profile_begin("StoryManager::loadReputationLevels");
    SO_CONTINUE(int, load_reputation_hook, self, path);
    load_profile_end("StoryManager::loadReputationLevels", started);
}

static void load_scripts_hook_func(void *self) {
    uint64_t started = load_profile_begin("ScriptManager::loadScripts");
    SO_CONTINUE(int, load_scripts_hook, self);
    load_profile_end("ScriptManager::loadScripts", started);
}

static void load_dialog_sounds_hook_func(void *self) {
    uint64_t started = load_profile_begin("ScriptManager::loadDialogSounds");
    SO_CONTINUE(int, load_dialog_sounds_hook, self);
    load_profile_end("ScriptManager::loadDialogSounds", started);
}

static void load_interrupt_conditions_hook_func(void *self) {
    uint64_t started = load_profile_begin(
        "ScriptManager::loadInterruptConditions");
    SO_CONTINUE(int, load_interrupt_conditions_hook, self);
    load_profile_end("ScriptManager::loadInterruptConditions", started);
}

static void load_props_hook_func(void *self) {
    uint64_t started = load_profile_begin("ScriptManager::loadProps");
    SO_CONTINUE(int, load_props_hook, self);
    load_profile_end("ScriptManager::loadProps", started);
}

static void load_highlights_hook_func(void *self, const char *path) {
    uint64_t started = load_profile_begin("GameObjectManager::loadHighlights");
    SO_CONTINUE(int, load_highlights_hook, self, path);
    load_profile_end("GameObjectManager::loadHighlights", started);
}

static void load_particles_hook_func(void) {
    uint64_t started = load_profile_begin("Character::loadParticles");
    SO_CONTINUE(int, load_particles_hook);
    load_profile_end("Character::loadParticles", started);
}

static void resolve_behavior_scripts_hook_func(void *self) {
    uint64_t started = load_profile_begin("BehaviorManager::resolveScripts");
    SO_CONTINUE(int, resolve_behavior_scripts_hook, self);
    load_profile_end("BehaviorManager::resolveScripts", started);
}

static int control_manager_init_hook_func(void *self, void *ctx) {
    uint64_t started = load_profile_begin("CControlManager::init");
    int result = SO_CONTINUE(int, control_manager_init_hook, self, ctx);
    load_profile_end("CControlManager::init", started);
    return result;
}

static int train_manager_init_hook_func(void *self, void *ctx) {
    uint64_t started = load_profile_begin("TrainManager::init");
    int result = SO_CONTINUE(int, train_manager_init_hook, self, ctx);
    load_profile_end("TrainManager::init", started);
    return result;
}

static void hud_manager_enable_hook_func(void *self) {
    uint64_t started = load_profile_begin("CHudManager::enable");
    SO_CONTINUE(int, hud_manager_enable_hook, self);
    load_profile_end("CHudManager::enable", started);
}

static void loading_menu_update_hook_func(void *self, void *gs_main) {
    int before = self ? *(const int *)((const uint8_t *)self + 4) : -1;
    uint64_t started = sceKernelGetSystemTimeWide();
    SO_CONTINUE(int, loading_menu_update_hook, self, gs_main);
    uint64_t elapsed = sceKernelGetSystemTimeWide() - started;
    int after = self ? *(const int *)((const uint8_t *)self + 4) : -1;
    if ((before != after && before <= 0x11) || elapsed >= 100000) {
        LOG_INFO("LOADPROF: LoadingMenu::update stage=%d->%d took=%llu ms",
                 before, after, (unsigned long long)(elapsed / 1000));
        diagnostics_flush();
    }
}

static int play_main_menu_music_hook_func(void *self) {
    uint64_t started = load_profile_begin("GSMain::playMainMenuMusic");
    int result = SO_CONTINUE(int, play_main_menu_music_hook, self);
    load_profile_end("GSMain::playMainMenuMusic", started);
    return result;
}

static void application_start_sound_thread_hook_func(void *self) {
    uint64_t started = load_profile_begin("Application::startSoundThread");
    SO_CONTINUE(int, application_start_sound_thread_hook, self);
    load_profile_end("Application::startSoundThread", started);
}

#ifdef HAVE_SHARK_LOG
extern shark_opt compiler_opts;
extern int32_t compiler_fastmath, compiler_fastprecision, compiler_fastint;
extern void shark_log_cb(const char *message, shark_log_level level, int line);

static void compiler_probe_log(const char *message, shark_log_level level, int line) {
    LOG_INFO("COMPILER PROBE: level=%d line=%d message=%s", level, line, message);
}

static void probe_shader_compiler(const char *phase) {
    diagnostics_compiler_checkpoint(phase, 0);
    const char *path = "ux0:data/gangstarrio/compiler-probe.cg";
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        LOG_INFO("COMPILER PROBE: phase=%s fixture unavailable result=0x%x", phase, fd);
        return;
    }
    SceOff length = sceIoLseek(fd, 0, SCE_SEEK_END);
    char *source = length > 0 && length <= 65536 ? malloc((size_t)length + 1) : NULL;
    int read_size = -1;
    if (source && sceIoLseek(fd, 0, SCE_SEEK_SET) >= 0)
        read_size = sceIoRead(fd, source, (SceSize)length);
    sceIoClose(fd);
    if (!source || read_size != length) {
        LOG_INFO("COMPILER PROBE: phase=%s fixture read failed length=%lld read=%d", phase, (long long)length, read_size);
        free(source);
        return;
    }
    source[length] = 0;
    uint32_t size = (uint32_t)length;
    uint32_t fpscr_before, fpscr_after;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr_before));
    uint64_t started = sceKernelGetSystemTimeWide();
    shark_install_log_cb(compiler_probe_log);
    const SceGxmProgram *program = shark_compile_shader_extended(source, &size,
        SHARK_VERTEX_SHADER, compiler_opts, compiler_fastmath,
        compiler_fastprecision, compiler_fastint);
    uint64_t elapsed = sceKernelGetSystemTimeWide() - started;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr_after));
    int checked = program ? sceGxmProgramCheck(program) : -1;
    LOG_INFO("COMPILER PROBE: phase=%s thread=0x%x source_bytes=%u result=%p binary_bytes=%u check=0x%x fpscr=0x%x->0x%x opt=%d fast=%d/%d/%d took=%llu us",
             phase, (unsigned)sceKernelGetThreadId(), (unsigned)length,
             program, size, checked, fpscr_before, fpscr_after,
             compiler_opts, compiler_fastmath, compiler_fastprecision, compiler_fastint,
             (unsigned long long)elapsed);
    shark_clear_output();
    shark_install_log_cb(shark_log_cb);
    diagnostics_compiler_checkpoint(phase, 1);
    free(source);
    diagnostics_flush();
}

#endif

static void sound_init_hook_func(void *self) {
#ifdef HAVE_SHARK_LOG
    diagnostics_compiler_trace_begin();
    probe_shader_compiler("before native audio");
#endif
    uint64_t started = load_profile_begin("SoundManager::init");
    SO_CONTINUE(int, sound_init_hook, self);
    load_profile_end("SoundManager::init", started);
#ifdef HAVE_SHARK_LOG
    probe_shader_compiler("after native audio");
    diagnostics_compiler_trace_end();
    diagnostics_flush();
#endif
}

static int sound_pack_xml_hook_func(void *self, const char *name) {
    LOG_INFO("AUDIO: XML begin name=%s", name ? name : "(null)");
    uint64_t started = load_profile_begin("VoxSoundPackXML::LoadXML");
    int result = SO_CONTINUE(int, sound_pack_xml_hook, self, name);
    LOG_INFO("AUDIO: XML end name=%s result=%d", name ? name : "(null)", result);
    load_profile_end("VoxSoundPackXML::LoadXML", started);
    return result;
}

#ifdef HAVE_SHARK_LOG
typedef struct {
    uint32_t open_calls;
    uint32_t open_us;
    uint32_t stream_calls;
    uint32_t stream_us;
    uint32_t stdio_calls;
    uint32_t stdio_us;
    uint32_t decoder_calls;
    uint32_t decoder_us;
} NativeAudioStats;

static __thread NativeAudioStats native_audio_stats;

static uint32_t filesystem_prefix_hash(void *fs) {
    unsigned count = filesystem_folder_count(fs, 0x3c);
    const uint32_t *prefixes = *(const uint32_t **)((uint8_t *)fs + 0x3c);
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *p = (const unsigned char *)(uintptr_t)prefixes[i];
        do { hash = (hash ^ *p) * 16777619u; } while (*p++);
    }
    return hash;
}

static int filesystem_native_test(void *fs) {
    const char *alias = "__rio_fs_probe_0066_9b7e__";
    const char *alias_slash = "__rio_fs_probe_0066_9b7e__/";
    const char *middle_slash = "__rio_fs_middle_0066_9b7e__/";
    unsigned before = filesystem_folder_count(fs, 0x20);
    if (filesystem_folder_count(fs, 0x3c) != before) {
        LOG_ERROR("FILESYSTEM TEST: FAIL initial folder lists differ; probe not started");
        return 0;
    }
    uint32_t hash = filesystem_prefix_hash(fs);
    int passed = 1;
    passed &= filesystem_add_folder(fs, alias, 1, 1) != 0;
    passed &= filesystem_add_folder(fs, middle_slash, 1, 1) != 0;
    passed &= filesystem_add_folder(fs, alias_slash, 1, 1) != 0;
    passed &= filesystem_folder_count(fs, 0x20) == before + 3 &&
              filesystem_folder_count(fs, 0x3c) == before + 3;
    if (filesystem_folder_count(fs, 0x3c) == before + 3) {
        const uint32_t *prefixes = *(const uint32_t **)((uint8_t *)fs + 0x3c);
        passed &= strcmp((const char *)(uintptr_t)prefixes[before], alias) == 0 &&
                  strcmp((const char *)(uintptr_t)prefixes[before + 1], "__rio_fs_middle_0066_9b7e__") == 0 &&
                  strcmp((const char *)(uintptr_t)prefixes[before + 2], alias) == 0;
    }
    passed &= filesystem_remove_archive(fs, alias_slash) != 0;
    if (filesystem_folder_count(fs, 0x3c) == before + 2) {
        const uint32_t *prefixes = *(const uint32_t **)((uint8_t *)fs + 0x3c);
        passed &= strcmp((const char *)(uintptr_t)prefixes[before], "__rio_fs_middle_0066_9b7e__") == 0 &&
                  strcmp((const char *)(uintptr_t)prefixes[before + 1], alias) == 0;
    } else passed = 0;
    passed &= filesystem_remove_archive(fs, alias_slash) != 0;
    passed &= filesystem_remove_archive(fs, middle_slash) != 0;
    passed &= filesystem_remove_archive(fs, "__rio_fs_absent_0066_9b7e__/") == 0;
    passed &= filesystem_folder_count(fs, 0x20) == before &&
              filesystem_folder_count(fs, 0x3c) == before && filesystem_prefix_hash(fs) == hash;
    LOG_INFO("FILESYSTEM TEST: %s native alias/middle erase, missing removal, original prefix order restored count=%u",
             passed ? "PASS" : "FAIL", before);
    return passed;
}

static void sound_filesystem_snapshot(const char *path) {
    static unsigned sampled;
    if (__atomic_exchange_n(&sampled, 1, __ATOMIC_RELAXED)) return;
    if (!sound_application_slot || !*sound_application_slot ||
        !sound_get_device || !sound_filesystem_vptr) return;
    void *device = sound_get_device();
    void *fs = device ? *(void **)((uint8_t *)device + 0x2c) : NULL;
    if (!fs || ((uintptr_t)fs & 3) || *(uintptr_t *)fs != sound_filesystem_vptr) {
        LOG_INFO("AUDIO: filesystem snapshot unavailable device=%p fs=%p", device, fs);
        return;
    }
    if (!filesystem_write_lock(filesystem_lock, 0)) {
        LOG_ERROR("FILESYSTEM TEST: FAIL snapshot write lock unavailable");
        diagnostics_flush();
        sceKernelExitProcess(1);
        return;
    }
    if (!filesystem_native_test(fs)) {
        filesystem_write_unlock(filesystem_lock);
        LOG_ERROR("FILESYSTEM TEST: failed; stopping diagnostic trial before audio");
        diagnostics_flush();
        sceKernelExitProcess(1);
        return;
    }
    const uint32_t *header = (const uint32_t *)((uint8_t *)fs + 0x30);
    unsigned archives = (header[1] - header[0]) / 4u;
    unsigned custom = filesystem_folder_count(fs, 0x3c);
    unsigned loose = filesystem_folder_count(fs, 0x20);
    unsigned adds = filesystem_adds, removes = filesystem_removes;
    unsigned normalized = filesystem_normalized, mismatches = filesystem_mismatches;
    filesystem_write_unlock(filesystem_lock);
    LOG_INFO("AUDIO: filesystem snapshot path=%s fs=%p archives=%u folders=%u loose_folders=%u synchronized=%d adds=%u removes=%u normalized=%u mismatches=%u",
             path ? path : "(null)", fs, archives, custom, loose, custom == loose,
             adds, removes, normalized, mismatches);
}

static void *sound_file_open_hook_func(void *self, char *path, int mode) {
    sound_filesystem_snapshot(path);
    uint64_t started = sceKernelGetSystemTimeWide();
    void *result = SO_CONTINUE(void *, sound_file_open_hook, self, path, mode);
    uint32_t elapsed = (uint32_t)(sceKernelGetSystemTimeWide() - started);
    native_audio_stats.open_calls++;
    native_audio_stats.open_us += elapsed;
    return result;
}

static int sound_file_stream_open_hook_func(void *self, const char *path,
                                            unsigned flags) {
    uint64_t started = sceKernelGetSystemTimeWide();
    int result = SO_CONTINUE(int, sound_file_stream_open_hook, self, path, flags);
    uint32_t elapsed = (uint32_t)(sceKernelGetSystemTimeWide() - started);
    native_audio_stats.stdio_calls++;
    native_audio_stats.stdio_us += elapsed;
    return result;
}

static int sound_stream_open_hook_func(void *self, const char *path,
                                       unsigned flags) {
    uint64_t started = sceKernelGetSystemTimeWide();
    int result = SO_CONTINUE(int, sound_stream_open_hook, self, path, flags);
    uint32_t elapsed = (uint32_t)(sceKernelGetSystemTimeWide() - started);
    native_audio_stats.stream_calls++;
    native_audio_stats.stream_us += elapsed;
    return result;
}

static void *sound_decoder_cursor_hook_func(void *self, void *stream_cursor) {
    uint64_t started = sceKernelGetSystemTimeWide();
    void *result = SO_CONTINUE(void *, sound_decoder_cursor_hook, self,
                              stream_cursor);
    uint32_t elapsed = (uint32_t)(sceKernelGetSystemTimeWide() - started);
    native_audio_stats.decoder_calls++;
    native_audio_stats.decoder_us += elapsed;
    return result;
}
#endif

static void *sound_data_source_hook_func(void *out_handle, const char *path,
                                         int format, unsigned flags,
                                         int final_arg) {
    LOG_INFO("AUDIO: data source begin path=%s format=%d flags=0x%x argument=%d",
             path ? path : "(null)", format, flags, final_arg);
    IoPhysicalStats before, after;
    io_get_physical_stats(&before);
#ifdef HAVE_SHARK_LOG
    NativeAudioStats native_before = native_audio_stats;
#endif
    uint64_t started = sceKernelGetSystemTimeWide();
    /* ARM returns the C++ DataHandle through the caller-owned r0 pointer.
     * Forward that pointer and the stack argument without copying the object. */
    void *result = SO_CONTINUE(void *, sound_data_source_hook, out_handle, path,
                              format, flags, final_arg);
    uint64_t elapsed = sceKernelGetSystemTimeWide() - started;
    io_get_physical_stats(&after);
#ifdef HAVE_SHARK_LOG
    NativeAudioStats native_after = native_audio_stats;
    /* OpenFile includes archive lookup and FileStream::Open, which includes
     * Impl::Open (stdio). These nested timings must not be added together. */
    LOG_INFO("AUDIO: native source path=%s thread_native(open=%u/%u us stdio_in_open=%u/%u us decoder=%u/%u us)",
             path ? path : "(null)",
             native_after.open_calls - native_before.open_calls,
             native_after.open_us - native_before.open_us,
             native_after.stdio_calls - native_before.stdio_calls,
             native_after.stdio_us - native_before.stdio_us,
             native_after.decoder_calls - native_before.decoder_calls,
             native_after.decoder_us - native_before.decoder_us);
    LOG_INFO("AUDIO: native lookup path=%s thread_lookup(stream_in_open=%u/%u us)",
             path ? path : "(null)",
             native_after.stream_calls - native_before.stream_calls,
             native_after.stream_us - native_before.stream_us);
#endif
    LOG_INFO("AUDIO: data source end path=%s took=%llu ms thread_io(pread=%u/%u us/%u bytes fopen=%u/%u us stat=%u/%u us)",
             path ? path : "(null)", (unsigned long long)(elapsed / 1000),
             after.pread_calls - before.pread_calls, after.pread_us - before.pread_us,
             after.pread_bytes - before.pread_bytes, after.fopen_calls - before.fopen_calls,
             after.fopen_us - before.fopen_us, after.stat_calls - before.stat_calls,
             after.stat_us - before.stat_us);
    return result;
}

static void sound_play_low_fps_music_hook_func(void *self, const char *name,
                                               int loop) {
    LOG_INFO("AUDIO: native music play name=%s loop=%d",
             name ? name : "(null)", loop);
    uint64_t started = load_profile_begin("SoundManager::playLowFpsMusic");
    SO_CONTINUE(int, sound_play_low_fps_music_hook, self, name, loop);
    load_profile_end("SoundManager::playLowFpsMusic", started);
}

static void sound_stop_low_fps_music_hook_func(void *self) {
    uint64_t started = load_profile_begin("SoundManager::stopLowFpsMusic");
    SO_CONTINUE(int, sound_stop_low_fps_music_hook, self);
    load_profile_end("SoundManager::stopLowFpsMusic", started);
}

/* Read the same native state consumed by NativeGetMainLoadingProgress:
 * StateMachine::getState() -> GSMain + 4 -> LoadingMenu progress at +8.
 * This is deliberately a read-only observer. Hooking GS3DStuff::load itself
 * is unsafe here because its C++ ABI/lifetime boundary is not stable under
 * the loader trampoline. */
static void runtime_patch_dump_loading_state(void) {
    if (!loading_state_machine_get_instance ||
        !loading_state_machine_get_state) {
        return;
    }

    void *state_machine = loading_state_machine_get_instance();
    void *state = state_machine
        ? loading_state_machine_get_state(state_machine) : NULL;
    if (!state) {
        return;
    }

    void *loading_menu = *(void **)((uint8_t *)state + 4);
    if (!loading_menu) {
        return;
    }

    int stage = *(const int *)((const uint8_t *)loading_menu + 4);
    int progress = *(const int *)((const uint8_t *)loading_menu + 8);
    if (stage < 0 || stage > 0x11 || progress < 0 || progress > 100) {
        return;
    }

    if (stage == loading_last_stage && progress == loading_last_progress) {
        return;
    }

    loading_last_stage = stage;
    loading_last_progress = progress;
    const char *name = (loading_state_names && stage <= 0x10)
        ? loading_state_names[stage] : NULL;
    int weight = (loading_percent_weights && stage > 0 && stage <= 0x10)
        ? loading_percent_weights[stage - 1] : 0;
    LOG_INFO("LOAD: native progress=%d stage=%d(%s) table_slot_value=%d menu=%p state=%p",
             progress, stage, name ? name : "unknown", weight,
             loading_menu, state);
    diagnostics_flush();
}

static volatile void *s_active_wait_task = NULL;
static volatile SceUID s_active_wait_tid = 0;
static volatile uint64_t s_active_wait_start = 0;
static volatile uintptr_t s_active_wait_cb = 0;

static volatile void *s_active_cond = NULL;
static volatile void *s_active_cond_mutex = NULL;
static volatile SceUID s_active_cond_tid = 0;
static volatile uint64_t s_active_cond_start = 0;

static volatile void *s_active_run_task = NULL;
static volatile SceUID s_active_run_tid = 0;
static volatile uint64_t s_active_run_start = 0;
static volatile uintptr_t s_active_run_callback = 0;

typedef struct {
    volatile SceUID tid;
    volatile unsigned phase;
    volatile void *handler;
    volatile void *manager;
    volatile void *task;
    volatile uintptr_t callback;
    volatile int wait_for_task;
    volatile uint64_t started;
    volatile uint64_t phase_started;
} ActiveHandlerTrace;

#define ACTIVE_HANDLER_TRACE_COUNT 8
static ActiveHandlerTrace s_active_handlers[ACTIVE_HANDLER_TRACE_COUNT];

enum {
    HANDLER_PHASE_DISPATCH = 1,
    HANDLER_PHASE_EXECUTE = 2,
    HANDLER_PHASE_FINISH = 3,
};

void runtime_patch_dump_state(void) {
    uint64_t now = sceKernelGetSystemTimeWide();
    runtime_patch_dump_loading_state();
    diagnostics_log("WATCHDOG",
                    "TASK COUNTERS: waits=%u conds=%u cond_signals=%u "
                    "cond_broadcasts=%u cond_timeouts=%u cond_fallbacks=%u "
                    "runs=%u sets=%u handler_runs=%u",
                    __atomic_load_n(&task_wait_count, __ATOMIC_RELAXED),
                    __atomic_load_n(&condition_wait_count, __ATOMIC_RELAXED),
                    __atomic_load_n(&condition_signal_count, __ATOMIC_RELAXED),
                    __atomic_load_n(&condition_broadcast_count, __ATOMIC_RELAXED),
                    __atomic_load_n(&condition_timeout_count, __ATOMIC_RELAXED),
                    __atomic_load_n(&condition_fallback_count, __ATOMIC_RELAXED),
                    __atomic_load_n(&task_run_count, __ATOMIC_RELAXED),
                    __atomic_load_n(&task_set_count, __ATOMIC_RELAXED),
                    __atomic_load_n(&task_handler_run_count, __ATOMIC_RELAXED));

    if (s_active_wait_task) {
        uint64_t elapsed_ms = (now - s_active_wait_start) / 1000;
        diagnostics_log("WATCHDOG",
                        "ACTIVE ITask::wait: task=%p cb=0x%08x tid=0x%08x elapsed=%llu ms",
                        s_active_wait_task, (unsigned)s_active_wait_cb,
                        (unsigned)s_active_wait_tid, (unsigned long long)elapsed_ms);
    }
    if (s_active_cond) {
        uint64_t elapsed_ms = (now - s_active_cond_start) / 1000;
        diagnostics_log("WATCHDOG",
                        "ACTIVE CCondition::wait: cond=%p mutex=%p tid=0x%08x elapsed=%llu ms",
                        s_active_cond, s_active_cond_mutex,
                        (unsigned)s_active_cond_tid, (unsigned long long)elapsed_ms);
    }
    if (s_active_run_task) {
        uint64_t elapsed_ms = (now - s_active_run_start) / 1000;
        diagnostics_log("WATCHDOG",
                        "ACTIVE CCpuTask::run: task=%p cb=0x%08x tid=0x%08x elapsed=%llu ms",
                        s_active_run_task, (unsigned)s_active_run_callback,
                        (unsigned)s_active_run_tid, (unsigned long long)elapsed_ms);
    }

    for (unsigned i = 0; i < ACTIVE_HANDLER_TRACE_COUNT; ++i) {
        ActiveHandlerTrace *trace = &s_active_handlers[i];
        SceUID tid = __atomic_load_n(&trace->tid, __ATOMIC_ACQUIRE);
        unsigned phase = __atomic_load_n(&trace->phase, __ATOMIC_ACQUIRE);
        if (tid <= 0 || phase == 0) continue;

        uint64_t phase_start = __atomic_load_n(&trace->phase_started,
                                               __ATOMIC_RELAXED);
        uint64_t elapsed_ms = (now - phase_start) / 1000;
        const char *phase_name = phase == HANDLER_PHASE_DISPATCH
            ? "dispatchTask" : phase == HANDLER_PHASE_EXECUTE
            ? "task.execute" : "handler-finish";
        diagnostics_log("WATCHDOG",
                        "ACTIVE CCpuTaskHandler::run: phase=%s handler=%p "
                        "manager=%p task=%p callback=0x%08x wait=%d "
                        "tid=0x%08x phase_elapsed=%llu ms",
                        phase_name, trace->handler, trace->manager,
                        trace->task, (unsigned)trace->callback,
                        trace->wait_for_task, (unsigned)tid,
                        (unsigned long long)elapsed_ms);

        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(tid, &info) >= 0) {
            diagnostics_log("WATCHDOG",
                            "HANDLER THREAD: name=\"%s\" status=0x%08X "
                            "waitType=0x%X waitId=0x%08X cpu=%d",
                            info.name, (unsigned)info.status,
                            (unsigned)info.waitType, (unsigned)info.waitId,
                            info.currentCpuId);
        }
    }
}

static int address_in_game_text(uintptr_t address) {
    if (!task_game_module) return 0;
    address &= ~(uintptr_t)1;
    return address >= task_game_module->text_base &&
           address < task_game_module->text_base + task_game_module->text_size;
}

static int address_in_game_data(uintptr_t address, size_t size) {
    if (!task_game_module || address + size < address) return 0;
    for (int i = 0; i < task_game_module->n_data; ++i) {
        uintptr_t begin = task_game_module->data_base[i];
        uintptr_t end = begin + task_game_module->data_size[i];
        if (address >= begin && address + size <= end) return 1;
    }
    return 0;
}

typedef struct {
    void *condition;
    SceUID sema;
    unsigned waiters;
    int retired;
} NativeCondition;

#define MAX_NATIVE_CONDITIONS 64
#define CONDITION_WAIT_CAP_US 10000
#define CONDITION_REGISTRY_LOCK_US 10000
static NativeCondition s_native_conditions[MAX_NATIVE_CONDITIONS];
static SceKernelLwMutexWork s_native_condition_lock;
static int s_native_condition_lock_ready;

static int native_condition_registry_init(void) {
    if (s_native_condition_lock_ready) return 0;
    memset(s_native_conditions, 0, sizeof(s_native_conditions));
    for (unsigned i = 0; i < MAX_NATIVE_CONDITIONS; ++i) {
        s_native_conditions[i].sema = -1;
    }
    int result = sceKernelCreateLwMutex(&s_native_condition_lock,
                                       "rio_cond_registry", 0, 0, NULL);
    if (result < 0) {
        LOG_ERROR("PATCH: condition registry mutex creation failed: 0x%08x",
                  result);
        return result;
    }
    s_native_condition_lock_ready = 1;
    return 0;
}

static int native_condition_registry_lock(void) {
    if (!s_native_condition_lock_ready) return -1;
    SceUInt timeout = CONDITION_REGISTRY_LOCK_US;
    return sceKernelLockLwMutex(&s_native_condition_lock, 1, &timeout);
}

static void native_condition_clear_locked(NativeCondition *entry) {
    if (entry->sema >= 0) {
        int result = sceKernelDeleteSema(entry->sema);
        if (result < 0) {
            diagnostics_log("COND", "semaphore delete failed cond=%p sema=%d err=0x%08x",
                            entry->condition, entry->sema, result);
        }
    }
    entry->condition = NULL;
    entry->sema = -1;
    entry->waiters = 0;
    entry->retired = 0;
}

static NativeCondition *native_condition_find_locked(void *condition) {
    for (unsigned i = 0; i < MAX_NATIVE_CONDITIONS; ++i) {
        if (s_native_conditions[i].condition == condition) {
            return &s_native_conditions[i];
        }
    }
    return NULL;
}

static NativeCondition *native_condition_create_locked(void *condition) {
    NativeCondition *entry = native_condition_find_locked(condition);
    if (entry) return entry->retired ? NULL : entry;

    for (unsigned i = 0; i < MAX_NATIVE_CONDITIONS; ++i) {
        entry = &s_native_conditions[i];
        if (entry->condition) continue;

        char name[32];
        snprintf(name, sizeof(name), "rio_cond_%u", i);
        SceUID sema = sceKernelCreateSema(name, 0, 0, 65535, NULL);
        if (sema < 0) {
            diagnostics_log("COND", "semaphore create failed cond=%p slot=%u err=0x%08x",
                            condition, i, sema);
            return NULL;
        }
        entry->sema = sema;
        entry->waiters = 0;
        entry->retired = 0;
        entry->condition = condition;
        return entry;
    }
    return NULL;
}

/* The waiter is registered while it still owns Rio's mutex. Producers that
 * obey the condition-variable contract cannot signal until the waiter unlocks,
 * closing the lost-wakeup window in the earlier semaphore experiment. */
static NativeCondition *native_condition_register_waiter(void *condition) {
    if (native_condition_registry_lock() < 0) return NULL;
    NativeCondition *entry = native_condition_create_locked(condition);
    if (entry) ++entry->waiters;
    sceKernelUnlockLwMutex(&s_native_condition_lock, 1);
    return entry;
}

static void native_condition_unregister_waiter(NativeCondition *entry) {
    if (!entry) return;
    if (native_condition_registry_lock() < 0) {
        unsigned error = __atomic_add_fetch(&condition_wait_error_count, 1,
                                            __ATOMIC_RELAXED);
        if (error <= 16 || error % 256 == 0) {
            diagnostics_log("ERROR",
                            "condition waiter unregister lock failed "
                            "cond=%p error_count=%u",
                            entry->condition, error);
        }
        return;
    }
    if (entry->waiters > 0) --entry->waiters;
    if (entry->retired && entry->waiters == 0) {
        native_condition_clear_locked(entry);
    }
    sceKernelUnlockLwMutex(&s_native_condition_lock, 1);
}

static void condition_signal_common(void *condition, int broadcast) {
    unsigned call = broadcast
        ? __atomic_add_fetch(&condition_broadcast_count, 1, __ATOMIC_RELAXED)
        : __atomic_add_fetch(&condition_signal_count, 1, __ATOMIC_RELAXED);
    int result = 0;
    unsigned waiters = 0;
    if (condition && native_condition_registry_lock() >= 0) {
        NativeCondition *entry = native_condition_find_locked(condition);
        if (entry && !entry->retired && entry->waiters > 0) {
            waiters = entry->waiters;
            result = sceKernelSignalSema(entry->sema,
                                         broadcast ? (int)waiters : 1);
        }
        sceKernelUnlockLwMutex(&s_native_condition_lock, 1);
    } else {
        result = -1;
    }

    if ((call <= 16 || call % 1024 == 0) && (waiters > 0 || result < 0)) {
        diagnostics_log("COND", "CCondition::%s #%u cond=%p waiters=%u result=0x%08x",
                        broadcast ? "broadcast" : "signal", call,
                        condition, waiters, result);
    }
}

static void condition_signal_hook_func(void *condition) {
    condition_signal_common(condition, 0);
}

static void condition_broadcast_hook_func(void *condition) {
    condition_signal_common(condition, 1);
}

static void condition_destroy_hook_func(void *condition) {
    if (!condition || native_condition_registry_lock() < 0) return;
    NativeCondition *entry = native_condition_find_locked(condition);
    if (entry) {
        if (entry->waiters == 0) {
            native_condition_clear_locked(entry);
        } else {
            entry->retired = 1;
            diagnostics_log("COND", "condition destroyed with active waiters cond=%p waiters=%u",
                            condition, entry->waiters);
        }
    }
    sceKernelUnlockLwMutex(&s_native_condition_lock, 1);
}

/* Signal wakes a registered waiter immediately. A 10 ms ceiling remains even
 * for the native API's infinite-wait value so a missed/unsupported signal can
 * never recreate the old permanent loading hang. */
static int condition_wait_hook_func(void *condition, unsigned timeout_ms) {
    if (!condition) return 0;
    void *mutex_object = *(void **)condition;
    if (!mutex_object) return 0;

    pthread_mutex_t_bionic *mutex =
        (pthread_mutex_t_bionic *)((uint8_t *)mutex_object + 4);
    unsigned call = __atomic_add_fetch(
        &condition_wait_count, 1, __ATOMIC_RELAXED);
    uintptr_t caller = (uintptr_t)__builtin_return_address(0) &
                       ~(uintptr_t)1;
    if (call <= 8 || call % 10000 == 0) {
        diagnostics_log("COND",
                        "CCondition::wait #%u caller=0x%08x offset=0x%08x "
                        "condition=%p mutex=%p tid=0x%08x timeout_ms=%u",
                        call, (unsigned)caller,
                        task_game_module && caller >= task_game_module->text_base
                            ? (unsigned)(caller - task_game_module->text_base)
                            : 0,
                        condition, mutex, (unsigned)sceKernelGetThreadId(),
                        timeout_ms);
    }
    s_active_cond = condition;
    s_active_cond_mutex = mutex;
    s_active_cond_tid = sceKernelGetThreadId();
    s_active_cond_start = sceKernelGetSystemTimeWide();

    NativeCondition *entry = native_condition_register_waiter(condition);
    SceUID condition_sema = entry ? entry->sema : -1;
    int result = pthread_mutex_unlock_soloader(mutex);
    if (result != 0) {
        native_condition_unregister_waiter(entry);
        unsigned error = __atomic_add_fetch(
            &condition_wait_error_count, 1, __ATOMIC_RELAXED);
        if (error <= 16 || error % 1024 == 0) {
            diagnostics_log("ERROR",
                            "CCondition::wait #%u unlock failed "
                            "condition=%p mutex=%p err=%d failures=%u",
                            call, condition, mutex, result, error);
        }
        if (s_active_cond_tid == sceKernelGetThreadId()) {
            s_active_cond = NULL;
            s_active_cond_tid = 0;
        }
        return 0;
    }

    int wait_result = -1;
    if (entry) {
        SceUInt wait_us = timeout_ms == 0 || timeout_ms >= 10
            ? CONDITION_WAIT_CAP_US : timeout_ms * 1000;
        wait_result = sceKernelWaitSema(condition_sema, 1, &wait_us);
    } else {
        __atomic_add_fetch(&condition_fallback_count, 1, __ATOMIC_RELAXED);
        sceKernelDelayThread(CONDITION_WAIT_CAP_US);
    }

    result = pthread_mutex_lock_soloader(mutex);
    native_condition_unregister_waiter(entry);
    uint64_t elapsed_us = sceKernelGetSystemTimeWide() - s_active_cond_start;
    if (s_active_cond_tid == sceKernelGetThreadId()) {
        s_active_cond = NULL;
        s_active_cond_tid = 0;
    }
    if (result != 0) {
        unsigned error = __atomic_add_fetch(
            &condition_wait_error_count, 1, __ATOMIC_RELAXED);
        if (error <= 16 || error % 1024 == 0) {
            diagnostics_log("ERROR",
                            "CCondition::wait #%u relock failed "
                            "condition=%p mutex=%p err=%d failures=%u",
                            call, condition, mutex, result, error);
        }
        return 0;
    }

    if (entry && wait_result < 0) {
        unsigned timeouts = __atomic_add_fetch(
            &condition_timeout_count, 1, __ATOMIC_RELAXED);
        if (timeouts <= 16 || timeouts % 256 == 0) {
            diagnostics_log("COND",
                            "CCondition::wait timeout #%u call=%u cond=%p "
                            "sema=%d result=0x%08x elapsed=%llu us",
                            timeouts, call, condition, condition_sema,
                            wait_result, (unsigned long long)elapsed_us);
        }
    } else if (call <= 16 || elapsed_us >= CONDITION_WAIT_CAP_US) {
        diagnostics_log("COND",
                        "CCondition::wait complete call=%u cond=%p result=%s "
                        "elapsed=%llu us",
                        call, condition, entry ? "signaled" : "fallback",
                        (unsigned long long)elapsed_us);
    }
    return 1;
}

/* Rio creates many CPU/graphics tasks on the caller's stack and destroys them
 * immediately after ITask::wait.  Do not use the broken condition wrapper on
 * this lifetime boundary: keep the object alive until ITask::done publishes
 * its completion byte. */
static int task_wait_hook_func(void *task, unsigned timeout_ms) {
    if (!task) return 0;

    volatile uint8_t *done = (volatile uint8_t *)task + 0x2d;
    unsigned call = __atomic_add_fetch(&task_wait_count, 1, __ATOMIC_RELAXED);
    uint64_t started = sceKernelGetSystemTimeWide();
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    uintptr_t vtable = *(const uintptr_t *)task;
    uintptr_t execute = address_in_game_data(vtable, 20)
        ? *(const uintptr_t *)(vtable + 16) : 0;
    const char *task_kind = "ITask";
    uintptr_t callback_object = 0;
    uintptr_t callback_vtable = 0;
    uintptr_t callback_run = 0;
    if (vtable == cpu_task_vptr || vtable == graphics_task_vptr) {
        task_kind = vtable == graphics_task_vptr
            ? "CCpuGraphicsTask" : "CCpuTask";
        callback_object = *(const uintptr_t *)((const uint8_t *)task + 48);
        if (callback_object) {
            callback_vtable = *(const uintptr_t *)callback_object;
            if (address_in_game_data(callback_vtable, 12)) {
                callback_run = *(const uintptr_t *)(callback_vtable + 8);
            }
        }
    }
    unsigned io_opens_before = 0;
    unsigned io_reads_before = 0;
    unsigned io_bytes_before = 0;
    unsigned io_active_before = 0;
    unsigned io_exhaustions_before = 0;
    unsigned ram_before = 0;
    unsigned ram_total = 0;
    unsigned vram_before = 0;
    unsigned vram_total = 0;
    unsigned shader_count_before = 0;
    unsigned shader_us_before = 0;
    unsigned texture_count_before = 0;
    unsigned texture_bytes_before = 0;
    unsigned texture_us_before = 0;
    unsigned mipmap_count_before = 0;
    unsigned mipmap_us_before = 0;
    io_get_virtual_obb_stats(&io_opens_before, &io_reads_before,
                             &io_bytes_before, &io_active_before,
                             &io_exhaustions_before);
    ram_before = (unsigned)vglMemFree(VGL_MEM_RAM);
    ram_total = (unsigned)vglMemTotal(VGL_MEM_RAM);
    vram_before = (unsigned)vglMemFree(VGL_MEM_VRAM);
    vram_total = (unsigned)vglMemTotal(VGL_MEM_VRAM);
    gl_get_work_counters(&shader_count_before, &shader_us_before,
                         &texture_count_before, &texture_bytes_before,
                         &texture_us_before, &mipmap_count_before,
                         &mipmap_us_before);
    unsigned polls = 0;
    if (call <= 64) {
        diagnostics_log(
            "TASK",
            "ITask::wait #%u begin task=%p kind=%s tid=0x%08x "
            "callback=%p callback_vtable=0x%08x "
            "callback_run=0x%08x(%s) set=%u run=%u cond=%u",
            call, task, task_kind, (unsigned)sceKernelGetThreadId(),
            (void *)callback_object, (unsigned)callback_vtable,
            (unsigned)callback_run, task_callback_name(callback_run),
            __atomic_load_n(&task_set_count, __ATOMIC_RELAXED),
            __atomic_load_n(&task_run_count, __ATOMIC_RELAXED),
            __atomic_load_n(&condition_wait_count, __ATOMIC_RELAXED));
    }
    s_active_wait_task = task;
    s_active_wait_cb = callback_run;
    s_active_wait_tid = sceKernelGetThreadId();
    s_active_wait_start = sceKernelGetSystemTimeWide();

    /* Fast spin path: most CPU tasks finish in a handful of microseconds */
    for (int spin = 0; spin < 200; ++spin) {
        if (__atomic_load_n(done, __ATOMIC_ACQUIRE)) break;
        __asm__ volatile("nop");
    }
    while (!__atomic_load_n(done, __ATOMIC_ACQUIRE)) {
        sceKernelDelayThread(50);
        ++polls;
    }

    if (s_active_wait_tid == sceKernelGetThreadId()) {
        s_active_wait_task = NULL;
        s_active_wait_tid = 0;
    }

    uint64_t elapsed_us = sceKernelGetSystemTimeWide() - started;
    if (call <= 32 || elapsed_us >= 1000 * 1000) {
        unsigned io_opens_after = 0;
        unsigned io_reads_after = 0;
        unsigned io_bytes_after = 0;
        unsigned io_active_after = 0;
        unsigned io_exhaustions_after = 0;
        unsigned ram_after = 0;
        unsigned vram_after = 0;
        unsigned shader_count_after = 0;
        unsigned shader_us_after = 0;
        unsigned texture_count_after = 0;
        unsigned texture_bytes_after = 0;
        unsigned texture_us_after = 0;
        unsigned mipmap_count_after = 0;
        unsigned mipmap_us_after = 0;
        io_get_virtual_obb_stats(&io_opens_after, &io_reads_after,
                                 &io_bytes_after, &io_active_after,
                                 &io_exhaustions_after);
        ram_after = (unsigned)vglMemFree(VGL_MEM_RAM);
        vram_after = (unsigned)vglMemFree(VGL_MEM_VRAM);
        gl_get_work_counters(&shader_count_after, &shader_us_after,
                             &texture_count_after, &texture_bytes_after,
                             &texture_us_after, &mipmap_count_after,
                             &mipmap_us_after);
        diagnostics_log("TASK",
                        "ITask::wait #%u task=%p kind=%s caller=0x%08x "
                        "vtable=0x%08x execute=0x%08x timeout=%u "
                        "callback=0x%08x callback_vtable=0x%08x "
                        "callback_run=0x%08x "
                        "elapsed=%llu ms polls=%u OBB(open=%u read=%u "
                        "bytes=%u active=%u->%u exhausted=%u) "
                        "GL(shader=%u/%u ms texture=%u/%u KB/%u ms "
                        "mipmap=%u/%u ms) "
                        "RAM=%u->%u/%u KB VRAM=%u->%u/%u KB",
                        call, task, task_kind,
                        (unsigned)(caller & ~(uintptr_t)1),
                        (unsigned)vtable, (unsigned)execute, timeout_ms,
                        (unsigned)callback_object,
                        (unsigned)callback_vtable,
                        (unsigned)callback_run,
                        (unsigned long long)(elapsed_us / 1000), polls,
                        io_opens_after - io_opens_before,
                        io_reads_after - io_reads_before,
                        io_bytes_after - io_bytes_before,
                        io_active_before, io_active_after,
                        io_exhaustions_after - io_exhaustions_before,
                        shader_count_after - shader_count_before,
                        (shader_us_after - shader_us_before) / 1000,
                        texture_count_after - texture_count_before,
                        (texture_bytes_after - texture_bytes_before) / 1024,
                        (texture_us_after - texture_us_before) / 1000,
                        mipmap_count_after - mipmap_count_before,
                        (mipmap_us_after - mipmap_us_before) / 1000,
                        ram_before / 1024, ram_after / 1024,
                        ram_total / 1024, vram_before / 1024,
                        vram_after / 1024, vram_total / 1024);
    }
    return polls ? 1 : 0;
}

/* CCpuTaskHandler::setTask is exactly `str r1, [r0,#16]; bx lr` in the
 * supported ARM library.  Replacing it directly avoids SO_CONTINUE's
 * temporary unpatch/repatch window on this heavily concurrent path. */
static void cpu_task_set_hook_func(void *handler, void *task) {
    unsigned call = ++task_set_count;
    uintptr_t vtable = 0;
    uintptr_t execute = 0;
    /* dispatchTask has already read task->type immediately before this call,
     * so reading the first word cannot introduce a new invalid access here.
     * sceKernelFindMemBlockByAddr cannot see allocations inside newlib's heap
     * and therefore produced false negatives for every legitimate task. */
    if (task) vtable = *(const uintptr_t *)task;
    if (address_in_game_data(vtable, 20)) {
        execute = *(const uintptr_t *)(vtable + 16);
    }

    int suspicious = task && (!address_in_game_data(vtable, 20) ||
                              !address_in_game_text(execute));
    /* A stale task cannot be executed or completed safely.  Leave this
     * handler idle; dispatchTask will still unlink its queue node while
     * holding the manager mutex, allowing the remaining queue to progress. */
    *(void **)((uint8_t *)handler + 16) = suspicious ? NULL : task;

    if (call <= 24 || suspicious || call % 1024 == 0) {
        diagnostics_log(suspicious ? "ERROR" : "TASK",
                        "setTask #%u handler=%p task=%p "
                        "vtable=0x%08x execute=0x%08x",
                        call, handler, task,
                        (unsigned)vtable, (unsigned)execute);
        if (suspicious) {
            diagnostics_log("TASK",
                            "dropped stale task #%u before virtual call",
                            call);
            diagnostics_flush();
        }
    }
}

/* CCpuTask::run invokes the user callback and then unconditionally enters
 * ITask::done.  Rio sometimes destroys the stack-backed task from inside that
 * callback, leaving task->vtable == NULL; the original code then jumps via
 * [NULL + 0x14].  Replace this tiny runner so the post-callback lifetime is
 * checked before ITask::done is entered. */
static void cpu_task_run_hook_func(void *task) {
    unsigned call = __atomic_add_fetch(&task_run_count, 1, __ATOMIC_RELAXED);
    void *callback_object = task
        ? *(void **)((uint8_t *)task + 48)
        : NULL;
    uintptr_t callback_vtable = callback_object
        ? *(const uintptr_t *)callback_object
        : 0;
    uintptr_t callback_run = 0;
    if (address_in_game_data(callback_vtable, 12)) {
        callback_run = *(const uintptr_t *)(callback_vtable + 8);
    }

    if (!callback_object || !address_in_game_text(callback_run)) {
        diagnostics_log("ERROR",
                        "CCpuTask::run #%u rejected callback task=%p "
                        "object=%p vtable=0x%08x run=0x%08x",
                        call, task, callback_object,
                        (unsigned)callback_vtable, (unsigned)callback_run);
        diagnostics_flush();
        return;
    }

    uint64_t callback_started = sceKernelGetSystemTimeWide();
    if (call <= 128) {
        diagnostics_log(
            "TASK",
            "CCpuTask::run #%u begin tid=0x%08x task=%p object=%p "
            "vtable=0x%08x run=0x%08x(%s) RAM=%u KB VRAM=%u KB",
            call, (unsigned)sceKernelGetThreadId(), task, callback_object,
            (unsigned)callback_vtable, (unsigned)callback_run,
            task_callback_name(callback_run),
            (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024),
            (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024));
    }
    s_active_run_task = task;
    s_active_run_callback = callback_run;
    s_active_run_tid = sceKernelGetThreadId();
    s_active_run_start = sceKernelGetSystemTimeWide();

    IoPhysicalStats physical_before;
    io_get_physical_stats(&physical_before);
    uint64_t execute_started = sceKernelGetSystemTimeWide();
    ((void (*)(void *))callback_run)(callback_object);
    uint64_t execute_elapsed = sceKernelGetSystemTimeWide() - execute_started;
    IoPhysicalStats physical_after;
    io_get_physical_stats(&physical_after);

    if (s_active_run_tid == sceKernelGetThreadId()) {
        s_active_run_task = NULL;
        s_active_run_tid = 0;
    }

    uint64_t callback_elapsed = sceKernelGetSystemTimeWide() - callback_started;
    if (call <= 128 || callback_elapsed >=
            (callback_run == material_callback_run ? 50u : 500u) * 1000u) {
        diagnostics_log(
            callback_elapsed >= 500 * 1000 ? "PERF" : "TASK",
            "CCpuTask::run #%u end tid=0x%08x run=0x%08x "
            "elapsed=%llu ms execute=%llu us thread_io(pread_calls=%u pread_us=%u pread_bytes=%u fopen_calls=%u fopen_us=%u stat_calls=%u stat_us=%u cache_lock_calls=%u cache_lock_us=%u) RAM=%u KB VRAM=%u KB",
            call, (unsigned)sceKernelGetThreadId(),
            (unsigned)callback_run,
            (unsigned long long)(callback_elapsed / 1000),
            (unsigned long long)execute_elapsed,
            physical_after.pread_calls - physical_before.pread_calls,
            physical_after.pread_us - physical_before.pread_us,
            physical_after.pread_bytes - physical_before.pread_bytes,
            physical_after.fopen_calls - physical_before.fopen_calls,
            physical_after.fopen_us - physical_before.fopen_us,
            physical_after.stat_calls - physical_before.stat_calls,
            physical_after.stat_us - physical_before.stat_us,
            physical_after.cache_lock_calls - physical_before.cache_lock_calls,
            physical_after.cache_lock_us - physical_before.cache_lock_us,
            (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024),
            (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024));
    }

    uintptr_t task_vtable = task ? *(const uintptr_t *)task : 0;
    uintptr_t on_done = 0;
    if (address_in_game_data(task_vtable, 24)) {
        on_done = *(const uintptr_t *)(task_vtable + 20);
    }
    if (!address_in_game_text(on_done) || !task_done_address) {
        unsigned skipped = __atomic_add_fetch(
            &task_done_skip_count, 1, __ATOMIC_RELAXED);
        diagnostics_log("TASK",
                        "CCpuTask::run #%u skipped stale done #%u task=%p "
                        "vtable=0x%08x onDone=0x%08x",
                        call, skipped, task,
                        (unsigned)task_vtable, (unsigned)on_done);
        diagnostics_flush();
        return;
    }

    ((void (*)(void *))task_done_address)(task);
}

static ActiveHandlerTrace *active_handler_trace_begin(void *handler,
                                                       void *manager,
                                                       int wait_for_task) {
    SceUID tid = sceKernelGetThreadId();
    ActiveHandlerTrace *trace = NULL;
    for (unsigned i = 0; i < ACTIVE_HANDLER_TRACE_COUNT; ++i) {
        if (__atomic_load_n(&s_active_handlers[i].tid, __ATOMIC_ACQUIRE) == tid) {
            trace = &s_active_handlers[i];
            break;
        }
    }
    if (!trace) {
        for (unsigned i = 0; i < ACTIVE_HANDLER_TRACE_COUNT; ++i) {
            SceUID empty = 0;
            if (__atomic_compare_exchange_n(&s_active_handlers[i].tid, &empty,
                                             tid, 0, __ATOMIC_ACQ_REL,
                                             __ATOMIC_RELAXED)) {
                trace = &s_active_handlers[i];
                break;
            }
        }
    }
    if (trace) {
        trace->handler = handler;
        trace->manager = manager;
        trace->task = NULL;
        trace->callback = 0;
        trace->wait_for_task = wait_for_task;
        trace->started = sceKernelGetSystemTimeWide();
        trace->phase_started = trace->started;
        __atomic_store_n(&trace->phase, HANDLER_PHASE_DISPATCH,
                         __ATOMIC_RELEASE);
    }
    return trace;
}

static void active_handler_trace_set(ActiveHandlerTrace *trace,
                                     unsigned phase, void *task,
                                     uintptr_t callback) {
    if (!trace) return;
    trace->task = task;
    trace->callback = callback;
    trace->phase_started = sceKernelGetSystemTimeWide();
    __atomic_store_n(&trace->phase, phase, __ATOMIC_RELEASE);
}

static void active_handler_trace_end(ActiveHandlerTrace *trace) {
    if (!trace) return;
    __atomic_store_n(&trace->phase, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&trace->tid, 0, __ATOMIC_RELEASE);
}

/* setTask validation alone has a time-of-check/time-of-use window: Rio can
 * destroy a short-lived task after dispatchTask assigns it but before the
 * handler reaches task->vtable[4].  The 0.26 trace caught that exact path at
 * CCpuTaskHandler::run+0x34, jumping into an unrelated zlib function with
 * 0xDEADBEEF arguments.  Reproduce the compact original handler and validate
 * both virtual calls immediately before dispatching them. */
static int cpu_task_handler_run_hook_func(void *handler, void *manager,
                                          int wait_for_task) {
    if (!handler || !manager || !task_dispatch_address ||
        !task_keep_running_address || !task_has_pending_address) {
        diagnostics_log("ERROR",
                        "CCpuTaskHandler::run guard missing dependency "
                        "handler=%p manager=%p", handler, manager);
        diagnostics_flush();
        return 0;
    }

    unsigned call = __atomic_add_fetch(&task_handler_run_count, 1,
                                       __ATOMIC_RELAXED);
    uint64_t started = sceKernelGetSystemTimeWide();
    ActiveHandlerTrace *trace = active_handler_trace_begin(
        handler, manager, wait_for_task);
    task_dispatch_address(manager, handler, wait_for_task);
    uint64_t dispatch_elapsed = sceKernelGetSystemTimeWide() - started;

    void **task_slot = (void **)((uint8_t *)handler + 16);
    void *task = *task_slot;
    if (task) {
        uintptr_t vtable = *(const uintptr_t *)task;
        uintptr_t execute = 0;
        if (address_in_game_data(vtable, 20)) {
            execute = *(const uintptr_t *)(vtable + 16);
        }

        if (!address_in_game_text(execute)) {
            active_handler_trace_set(trace, HANDLER_PHASE_FINISH, task, execute);
            unsigned rejected = __atomic_add_fetch(
                &task_handler_reject_count, 1, __ATOMIC_RELAXED);
            *task_slot = NULL;
            diagnostics_log("TASK",
                            "CCpuTaskHandler::run rejected stale #%u "
                            "handler=%p task=%p vtable=0x%08x "
                            "execute=0x%08x slot_now=%p",
                            rejected, handler, task, (unsigned)vtable,
                            (unsigned)execute, *task_slot);
            diagnostics_flush();
        } else {
            active_handler_trace_set(trace, HANDLER_PHASE_EXECUTE, task, execute);
            const unsigned destroy_after_run =
                *((const uint8_t *)task + 44) != 0;
            uint64_t task_started = sceKernelGetSystemTimeWide();
            ((void (*)(void *))execute)(task);
            uint64_t task_elapsed = sceKernelGetSystemTimeWide() - task_started;
            if (task_elapsed >= 50 * 1000) {
                diagnostics_log("TASK",
                                "CCpuTaskHandler::run slow execute call=%u "
                                "task=%p callback=0x%08x elapsed=%llu us",
                                call, task, (unsigned)execute,
                                (unsigned long long)task_elapsed);
            }

            if (destroy_after_run) {
                active_handler_trace_set(trace, HANDLER_PHASE_FINISH,
                                         *task_slot, 0);
                void *current = *task_slot;
                uintptr_t current_vtable = current
                    ? *(const uintptr_t *)current : 0;
                uintptr_t destroy = 0;
                if (address_in_game_data(current_vtable, 8)) {
                    destroy = *(const uintptr_t *)(current_vtable + 4);
                }
                if (current && address_in_game_text(destroy)) {
                    ((void (*)(void *))destroy)(current);
                } else if (current) {
                    unsigned rejected = __atomic_add_fetch(
                        &task_handler_reject_count, 1, __ATOMIC_RELAXED);
                    diagnostics_log(
                        "TASK",
                        "CCpuTaskHandler::run skipped stale destroy #%u "
                        "handler=%p task=%p vtable=0x%08x destroy=0x%08x",
                        rejected, handler, current,
                        (unsigned)current_vtable, (unsigned)destroy);
                    diagnostics_flush();
                }
            }
            *task_slot = NULL;
        }
    } else {
        active_handler_trace_set(trace, HANDLER_PHASE_FINISH, NULL, 0);
    }

    const int state = *(const int *)((const uint8_t *)handler + 20);
    int result = 0;
    if (state == 1) {
        result = task_has_pending_address(manager)
            ? task_keep_running_address(manager, handler) : 0;
    } else if ((state < 1 || state > 3) &&
               task_keep_running_address(manager, handler)) {
        const unsigned type =
            *(const unsigned *)((const uint8_t *)handler + 12);
        result = type <= 1 ? 1 - (int)type : 0;
    }

    uint64_t elapsed = sceKernelGetSystemTimeWide() - started;
    if (call <= 16 || dispatch_elapsed >= 50 * 1000 || elapsed >= 50 * 1000) {
        diagnostics_log("TASK",
                        "CCpuTaskHandler::run #%u wait=%d task=%p state=%d "
                        "dispatch=%llu us total=%llu us result=%d",
                        call, wait_for_task, task, state,
                        (unsigned long long)dispatch_elapsed,
                        (unsigned long long)elapsed, result);
    }
    active_handler_trace_end(trace);
    return result;
}

static void gs_main_menu_ctor_hook_func(void *self, void *sm) {
    LOG_INFO("GSMainMenu::ctor called (self=%p, sm=%p)", self, sm);
    diagnostics_flush();
    SO_CONTINUE(int, gs_main_menu_ctor_hook, self, sm);
    LOG_INFO("GSMainMenu::ctor returned");
    diagnostics_flush();
}

static void gs_main_menu_draw_hook_func(void *self, void *sm) {
    static int draw_count = 0;
    if (++draw_count <= 5 || (draw_count % 300 == 0)) {
        LOG_INFO("GSMainMenu::draw #%d (self=%p, sm=%p)", draw_count, self, sm);
    }
    SO_CONTINUE(int, gs_main_menu_draw_hook, self, sm);
}

static void gs_main_menu_update_hook_func(void *self, void *sm) {
    static int update_count = 0;
    if (++update_count <= 5 || (update_count % 300 == 0)) {
        LOG_INFO("GSMainMenu::update #%d (self=%p, sm=%p)", update_count, self, sm);
    }
    SO_CONTINUE(int, gs_main_menu_update_hook, self, sm);
}

static void draw_player_hook_func(void *render_state, void *player_ptr) {
    static int dp_count = 0;
    if (++dp_count <= 5 || (dp_count % 300 == 0)) {
        LOG_INFO("DrawPlayer #%d (render_state=%p, player_ptr=%p)", dp_count, render_state, player_ptr);
    }
    if (!render_state || !player_ptr) {
        LOG_INFO("DrawPlayer: safely skipped drawing due to null argument (state=%p, ptr=%p)", render_state, player_ptr);
        return;
    }
    SO_CONTINUE(int, draw_player_hook, render_state, player_ptr);
}

static int console_vprintln_hook_func(const char *format, va_list arguments) {
    static unsigned console_count;
    char text[768];
    if (!format) return 0;
    vsnprintf(text, sizeof(text), format, arguments);
    unsigned call = ++console_count;
    if (call <= 80 || call % 2000 == 0 ||
        strstr(text, "profile") || strstr(text, "Profile") ||
        strstr(text, "shader") || strstr(text, "Shader") ||
        strstr(text, "material") || strstr(text, "Material") ||
        strstr(text, "invalid") || strstr(text, "ERROR") ||
        strstr(text, "Error")) {
        LOG_INFO("GAME[%u]: %s", call, text);
    }
    return 0;
}

static unsigned texture_max_dimension(const char *name) {
    const char *file = name;
    if (name) {
        for (const char *p = name; *p; ++p)
            if (*p == '/' || *p == '\\') file = p + 1;
    }
    return file && (strcmp(file, "huds.bmp") == 0 ||
                    strcmp(file, "huds_alpha.bmp") == 0) ? 2048u : 1024u;
}

static int prefetch_material_image(const char *resource, const char *name) {
    if (!name || !*name) return 0;
    char path[1024];
    const char *slash = resource ? strrchr(resource, '/') : NULL;
    const char *backslash = resource ? strrchr(resource, '\\') : NULL;
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    int length = slash ? snprintf(path, sizeof(path), "%.*s%s",
        (int)(slash - resource + 1), resource, name) : snprintf(path, sizeof(path), "%s", name);
    if (length < 0 || length >= (int)sizeof(path)) return 0;
    for (char *cursor = path; *cursor; ++cursor) if (*cursor == '\\') *cursor = '/';
    unsigned max_dimension = strstr(name, "copacabana_lightmap")
        ? UINT32_MAX : texture_max_dimension(name);
    unsigned bytes = io_prefetch_texture(path, max_dimension);
    if (!strstr(path, "_alpha.")) {
        char *extension = strrchr(path, '.');
        if (extension && (!strcmp(extension, ".tga") || !strcmp(extension, ".bmp")) &&
            (size_t)length + 6 < sizeof(path)) {
            char suffix[5];
            memcpy(suffix, extension, sizeof(suffix));
            strcpy(extension, "_alpha");
            strcat(extension, suffix);
            bytes += io_prefetch_texture(path, max_dimension);
        }
    }
    return (int)bytes;
}

static unsigned prefetch_copacabana_lightmaps(void) {
    unsigned bytes = 0;
    for (unsigned i = 0; i < 4; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "_packEtc/copacabana_lightmap%02u.tga", i);
        bytes += io_prefetch_texture(path, UINT32_MAX);
    }
    return bytes;
}

static void graphics_task_push_hook_func(void *task) {
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    const uint8_t *callback = task ? *(const uint8_t **)((uint8_t *)task + 0x30) : NULL;
    uintptr_t vtable = callback ? *(const uintptr_t *)callback : 0;
    if (caller == material_enqueue_return && address_in_game_data(vtable, 12) &&
        *(const uintptr_t *)(vtable + 8) == material_callback_run) {
        /* The streaming worker owns the database here. Read immutable DDS
         * ranges before the graphics task can run on the main thread. */
        uint64_t started = sceKernelGetSystemTimeWide();
        const uint8_t *database = *(const uint8_t *const *)(callback + 4);
        unsigned index = *(const uint16_t *)(callback + 8);
        const uint8_t *material = database ? collada_get_material(database, (int)index) : NULL;
        const uint8_t *resource = database ? *(const uint8_t *const *)database : NULL;
        const char *resource_name = resource ? *(const char *const *)(resource + 0xc) : NULL;
        unsigned prefetched_bytes = 0, images = 0;
        if (index == 37) {
            unsigned lightmap_bytes = prefetch_copacabana_lightmaps();
            prefetched_bytes += lightmap_bytes;
            LOG_INFO("MATERIAL PREFETCH: copacabana index=%u resource=%s bytes=%u",
                     index, resource_name ? resource_name : "<none>",
                     lightmap_bytes);
        }
        if (material) {
            unsigned count = *(const unsigned *)(material + 0x10);
            const uint8_t *parameters = *(const uint8_t *const *)(material + 0x14);
            if (parameters && count <= 64) for (unsigned i = 0; i < count; ++i) {
                const uint8_t *parameter = parameters + i * 0x18;
                if (*(const unsigned *)(parameter + 8) != 0xdu) continue;
                const unsigned *element_count = *(const unsigned *const *)(parameter + 0x10);
                const uint8_t *const *const *values =
                    *(const uint8_t *const *const *const *)(parameter + 0x14);
                if (!element_count || !values || *element_count > 64) continue;
                for (unsigned j = 0; j < *element_count; ++j) {
                    const uint8_t *image = values[j] ? *values[j] : NULL;
                    const char *name = image && !*(const void *const *)(image + 0xc)
                        ? *(const char *const *)(image + 8) : NULL;
                    if (name) {
                        prefetched_bytes += prefetch_material_image(resource_name, name);
                        ++images;
                    }
                }
            }
        }
        unsigned call = __sync_add_and_fetch(&material_prefetch_count, 1);
        if (call <= 8 || call % 100 == 0 ||
            sceKernelGetSystemTimeWide() - started >= 100000)
            LOG_INFO("MATERIAL PREFETCH: #%u tid=0x%08x index=%u images=%u bytes=%u took=%llu us",
                     call, (unsigned)sceKernelGetThreadId(), index, images,
                     prefetched_bytes,
                     (unsigned long long)(sceKernelGetSystemTimeWide() - started));
    }
    graphics_task_push_original(task);
}

static unsigned dds_max_dimension(const char *name) {
    const uint8_t *profile = phone_perf_slot ? *phone_perf_slot : NULL;
    if (!profile || !name) return UINT32_MAX;
    const uint32_t *begin = *(const uint32_t *const *)(profile + 0xf0);
    const uint32_t *end = *(const uint32_t *const *)(profile + 0xf4);
    int skip = strstr(name, "splash") ? 0 : *(const int *)(profile + 0xfc);
    for (const uint32_t *entry = begin; entry != end; entry += 2) {
        const char *pattern = (const char *)(uintptr_t)entry[0];
        if (pattern && *pattern && strstr(name, pattern)) {
            skip = (int)entry[1];
            break;
        }
    }
    /* CustomTexturePolicy later removes this many levels from the descriptor. */
    return texture_dds_max_dimension(texture_max_dimension(name), skip);
}

static int dds_read_header_hook_func(void *file, void *header) {
    int result = dds_read_header_original(file, header);
    DdsHeaderCapture *capture = dds_header_capture;
    if (result && capture && capture->file == file) {
        const uint32_t *words = (const uint32_t *)header;
        capture->flags = words[1];
        capture->mip_count = words[6];
        capture->valid = 1;
    }
    return result;
}

static int dds_header_hook_func(const void *loader, void *file, void *descriptor) {
    DdsHeaderCapture capture = {.file = file};
    DdsHeaderCapture *previous = dds_header_capture;
    dds_header_capture = &capture;
    int result = dds_header_original(loader, file, descriptor);
    dds_header_capture = previous;
    uint8_t *desc = (uint8_t *)descriptor;
    if (!result || !capture.valid || *(const uint32_t *)desc != 1u ||
        *(const uint32_t *)(desc + 0x18) != 1u || !desc[0x1c]) return result;
    unsigned format = *(const uint32_t *)(desc + 4);
    if (format != 0x1du && (format < 0x12u || format > 0x14u)) return result;

    const uintptr_t *vtable = *(const uintptr_t *const *)file;
    const char *(*get_name)(const void *) =
        (const char *(*)(const void *))vtable[0x28 / sizeof(uintptr_t)];
    const char *name = get_name(file);
    unsigned width = *(const uint32_t *)(desc + 0x10);
    unsigned height = *(const uint32_t *)(desc + 0x14);
    unsigned source_width = width, source_height = height;
    unsigned selected = texture_select_dds_dimensions(&width, &height,
        capture.mip_count, dds_max_dimension(name), capture.flags);
    if (selected) {
        /* Native loadTextureData now seeks past these mips before reading. */
        *(uint32_t *)(desc + 0x10) = width;
        *(uint32_t *)(desc + 0x14) = height;
        unsigned count = __sync_add_and_fetch(&dds_mip_selections, 1);
        if (count <= 8 || count % 100 == 0)
            LOG_INFO("TEXTURE: DDS mip selection #%u name=%s %ux%u -> %ux%u skip_levels=%u",
                     count, name ? name : "?", source_width, source_height,
                     width, height, selected);
    }
    return result;
}

static int texture_update_data_hook_func(void *self, int new_allocation) {
    const uint8_t *texture = (const uint8_t *)self;
    unsigned count = (texture[0x38] & 2u) ? 1u : texture[0x3b];
    const char *name = *(const char *const *)(texture + 8);
    GlTextureUploadLimits limits = {count ? count : 1u, texture_max_dimension(name)};
    if (limits.max_dimension == 2048u)
        LOG_INFO("TEXTURE: HUD name=%s mip_count=%u max_dimension=%u",
                 name, limits.mip_count, limits.max_dimension);
    GlTextureUploadLimits previous = gl_set_texture_upload_limits(limits);
    int result = texture_update_data_original(self, new_allocation);
    gl_set_texture_upload_limits(previous);
    return result;
}

/* Rio mounts its native low-memory resource tier from PerfProfile::useLowMem. */
static int choose_perf_profile_hook_func(int requested_profile) {
    int result = SO_CONTINUE(int, choose_perf_profile_hook,
                             requested_profile);
    void *profile = phone_perf_slot ? *phone_perf_slot : NULL;
    if (profile) {
        uint8_t *bytes = (uint8_t *)profile;
        const char *name = *(const char **)(bytes + 0x30);
        int width = profile_viewport_width();
        int height = profile_viewport_height();
        float previous_ratio = *(float *)(bytes + 0x70);
        /* 544 misses the native 540-only profiles, but SetupFolders still
         * selects androidWVGA540. Its atlas coordinates use ratio 0.8. */
        if (width == 960 && height == 544 && name &&
            strcmp(name, "DefaultProfile") == 0) {
            *(float *)(bytes + 0x70) = 0.8f;
        }
        unsigned previous = bytes[0x10b];
        bytes[0x10b] = 1; /* PerfProfile::useLowMem */
        LOG_SUCCESS("PERF: forced useLowMem=1 (previous=%u profile=%p)",
                    previous, profile);
        LOG_INFO("PERF: name=%s viewport=%dx%d hudImageRatio=%g previous=%g",
                 name ? name : "(null)", width, height,
                 (double)*(float *)(bytes + 0x70), (double)previous_ratio);
    } else {
        LOG_ERROR("PERF: ChoosePerfProfile returned without gPhonePerf");
    }
    return result;
}

static void *pink_material_hook_func(void *result, void *manager,
                                     const char *name) {
    static unsigned pink_count;
    unsigned call = ++pink_count;
    if (call <= 256 || call % 500 == 0) {
        LOG_ERROR("MATERIAL: pink fallback #%u name=%s manager=%p",
                  call, name ? name : "(null)", manager);
        diagnostics_flush();
    }
    return SO_CONTINUE(void *, pink_material_hook, result, manager, name);
}

/* TouchScreenBase::touchMoved assumes CameraManager's active camera exists.
 * During the early state transition Vita can deliver the initial stick/touch
 * event before that field is initialized, and the original virtual call then
 * dereferences NULL. Drop only that early event; once the camera is present,
 * preserve the original input path. */
static void touch_moved_hook_func(void *self, void *point, int32_t touch_id,
                                  int action) {
    void *camera_manager = camera_instance_slot ? *camera_instance_slot : NULL;
    void *camera = camera_manager
        ? *(void **)((uint8_t *)camera_manager + 0x1c) : NULL;
    if (!camera) {
        static unsigned skipped;
        unsigned call = ++skipped;
        if (call <= 16 || call % 1000 == 0) {
            LOG_ERROR("INPUT: skipped touchMoved #%u before camera init "
                      "manager=%p point=%p id=%d action=%d",
                      call, camera_manager, point, (int)touch_id, action);
            diagnostics_flush();
        }
        return;
    }
    kuKernelCpuUnrestrictedMemcpy((void *)touch_moved_hook.addr,
                                  touch_moved_hook.orig_instr,
                                  sizeof(touch_moved_hook.orig_instr));
    kuKernelFlushCaches((void *)touch_moved_hook.addr,
                        sizeof(touch_moved_hook.orig_instr));
    if (touch_moved_hook.thumb_addr) {
        ((void (*)(void *, void *, int32_t, int))touch_moved_hook.thumb_addr)(
            self, point, touch_id, action);
    } else {
        ((void (*)(void *, void *, int32_t, int))touch_moved_hook.addr)(
            self, point, touch_id, action);
    }
    kuKernelCpuUnrestrictedMemcpy((void *)touch_moved_hook.addr,
                                  touch_moved_hook.patch_instr,
                                  sizeof(touch_moved_hook.patch_instr));
    kuKernelFlushCaches((void *)touch_moved_hook.addr,
                        sizeof(touch_moved_hook.patch_instr));
}

static void (*strings_load_sheet_original)(void *, unsigned, unsigned);
static uint32_t *(*strings_create_rep)(unsigned, unsigned, const void *);
static void (*strings_delete_rep)(void *);
static const void *strings_empty_rep;

static void strings_load_sheet_hook(void *self, unsigned pack, unsigned sheet_id) {
    strings_load_sheet_original(self, pack, sheet_id);
    if (sheet_id != 3) return;
    const uint8_t *manager = self;
    const char *const *languages = *(const char *const *const *)(manager + 4);
    const char *const *languages_end = *(const char *const *const *)(manager + 8);
    if (!languages || languages_end < languages ||
        pack >= (unsigned)(languages_end - languages)) return;
    const char *language = languages[pack];
    if (!language || (strcmp(language, "english") && strcmp(language, "russian"))) return;
    uint8_t *sheets = *(uint8_t **)(manager + 0x10);
    uint8_t *sheet = sheets ? sheets + sheet_id * 0x14u : NULL;
    if (!sheet || !*sheet || strcmp(*(const char **)(sheet + 4), "global")) return;
    uintptr_t begin = *(uintptr_t *)(sheet + 8), end = *(uintptr_t *)(sheet + 12);
    if (!begin || end < begin || end - begin < 653u * 8u) {
        LOG_ERROR("INPUT LABELS: global string table profile mismatch");
        return;
    }
    unsigned updated = 0;
    for (unsigned index = 625; index <= 652; ++index) {
        char **value = (char **)(begin + index * 8u + 4u);
        char *data = *value;
        if (!data || !*data) continue;
        uint32_t *rep = (uint32_t *)data - 3;
        char text[2048];
        if (rep[2] != 0 || rep[0] >= sizeof(text) || rep[0] > rep[1]) {
            LOG_ERROR("INPUT LABELS: unexpected native string ownership index=%u", index);
            continue;
        }
        int result = controller_tutorial_text(text, sizeof(text), data, index,
                                             strcmp(language, "russian") == 0);
        if (result < 0) {
            LOG_ERROR("INPUT LABELS: translated string too long index=%u", index);
            continue;
        }
        if (!result) continue;
        unsigned length = (unsigned)strlen(text);
        if (length > rep[1]) {
            char allocator = 0;
            uint32_t *replacement = strings_create_rep(length, rep[1], &allocator);
            if (!replacement) continue;
            memcpy(replacement + 3, text, length + 1u);
            replacement[0] = length;
            replacement[2] = 0;
            *value = (char *)(replacement + 3);
            if (rep != strings_empty_rep) strings_delete_rep(rep);
        } else {
            memcpy(data, text, length + 1u);
            rep[0] = length;
        }
        ++updated;
    }
    LOG_INFO("INPUT LABELS: PlayStation tutorial names language=%s updated=%u", language, updated);
}

static int install_controller_labels(so_module *module) {
    uintptr_t load = so_symbol(module, "_ZN13StringManager13loadPackSheetEjj");
    const uint32_t prologue[2] = {0xe92d4ff0u, 0xe24ddd46u};
    uintptr_t original = immutable_continuation(module, load, prologue);
    strings_load_sheet_original = (void (*)(void *, unsigned, unsigned))original;
    strings_create_rep = (uint32_t *(*)(unsigned, unsigned, const void *))so_symbol(
        module, "_ZNSbIc9ci_traitsIcESaIcEE4_Rep9_S_createEjjRKS1_");
    strings_delete_rep = (void (*)(void *))so_symbol(module, "_ZdlPv");
    strings_empty_rep = (const void *)so_symbol(
        module, "_ZNSbIc9ci_traitsIcESaIcEE4_Rep20_S_empty_rep_storageE");
    if (!original || !strings_create_rep || !strings_delete_rep || !strings_empty_rep ||
        runtime_patch_install_trampoline(load, (uintptr_t)&strings_load_sheet_hook) < 0) {
        LOG_ERROR("PATCH: native tutorial string patch profile mismatch");
        return -1;
    }
    LOG_INFO("PATCH: PlayStation tutorial button labels enabled");
    return 0;
}

static int restore_player_cash_reader(so_module *module) {
    uintptr_t address = so_symbol(module, "_ZN6Player7getCashEv");
    const uint32_t modified[] = {
        0xe3a00506u, 0xe12fff1eu, 0xeb000024u, 0xe5940c10u, 0xe8bd8010u,
    };
    const uint32_t original[] = {0xe92d4010u, 0xe1a04000u};
    if (!address || (address & 3u) || address < module->text_base ||
        module->text_size < sizeof(modified) ||
        address - module->text_base > module->text_size - sizeof(modified) ||
        memcmp((const void *)address, modified, sizeof(modified)) != 0) {
        LOG_ERROR("PATCH: Player::getCash profile mismatch");
        return -1;
    }
    /* The supported APK replaces the prologue with a constant-return cheat. */
    if (kuKernelCpuUnrestrictedMemcpy((void *)address, original, sizeof(original)) < 0)
        return -1;
    kuKernelFlushCaches((void *)address, sizeof(original));
#ifdef HAVE_SHARK_LOG
    uint32_t player[0xc20u / sizeof(uint32_t)] = {0};
    const int balances[] = {123456, 123000, 0};
    player[0xb74u / sizeof(uint32_t)] = 7;
    for (unsigned i = 0; i < sizeof(balances) / sizeof(balances[0]); ++i) {
        /* Native decode reads the encoded cash mirror; use no live player/save. */
        player[0xb78u / sizeof(uint32_t)] = (uint32_t)balances[i] * 7u;
        player[0xc10u / sizeof(uint32_t)] = 25165824u;
        int actual = ((int (*)(void *))address)(player);
        if (actual != balances[i]) {
            LOG_ERROR("CASH TEST: FAIL expected=%d actual=%d", balances[i], actual);
            return -1;
        }
    }
    LOG_INFO("CASH TEST: PASS native getter reads 123456 -> 123000 -> 0");
#endif
    LOG_INFO("PATCH: Player::getCash restored native decoded balance");
    return 0;
}

static int enable_gamepad_touch_actions(so_module *module) {
    uintptr_t address = so_symbol(module, "_ZN13VirtualButton12processTouchEl");
    const uint32_t gate[] = {
        0xe5953068u, 0xe2433016u, 0xe3530001u,
        0x9a00001fu, 0xe5956024u, 0xe3160001u,
    };
    if (!address || (address & 3u) || address < module->text_base ||
        module->text_size < 0x40u + sizeof(gate) ||
        address - module->text_base > module->text_size - 0x40u - sizeof(gate) ||
        memcmp((const void *)(address + 0x40u), gate, sizeof(gate)) != 0) {
        LOG_ERROR("PATCH: native gamepad touch gate profile mismatch");
        return -1;
    }
    /* PowerA suppresses only grenade/nitro touch; retain their native handlers. */
    const uint32_t nop = 0xe1a00000u;
    void *branch = (void *)(address + 0x4cu);
    if (kuKernelCpuUnrestrictedMemcpy(branch, &nop, sizeof(nop)) < 0) return -1;
    kuKernelFlushCaches(branch, sizeof(nop));
    LOG_INFO("PATCH: native grenade/nitro touch enabled alongside gamepad");
    return 0;
}

int runtime_patch_game(so_module *module) {
    if (!module) return -1;

    task_game_module = module;
    if (restore_player_cash_reader(module) < 0) return -1;
    if (install_controller_labels(module) < 0) return -1;
    if (enable_gamepad_touch_actions(module) < 0) return -1;
    if (filesystem_patch(module) < 0) return -1;
    /* Managers is the 99% loading state. Keep the profiler at its native
     * subroutine boundaries so a slow asset loader is distinguishable from a
     * condition-variable stall. Each wrapper only observes elapsed time. */
#define INSTALL_LOAD_PROFILE(symbol, hook, function) do { \
        uintptr_t load_address = so_symbol(module, symbol); \
        if (load_address) { \
            hook = hook_addr(load_address, (uintptr_t)&function); \
            LOG_INFO("PATCH: load profiler enabled %s", symbol); \
        } else { \
            LOG_ERROR("PATCH: load profiler symbol missing %s", symbol); \
        } \
    } while (0)
    INSTALL_LOAD_PROFILE("_ZN7Mission4loadEPKc", load_mission_hook,
                         load_mission_hook_func);
    INSTALL_LOAD_PROFILE("_ZN12StoryManager20loadReputationLevelsEPKc",
                         load_reputation_hook, load_reputation_hook_func);
    INSTALL_LOAD_PROFILE("_ZN13ScriptManager11loadScriptsEv",
                         load_scripts_hook, load_scripts_hook_func);
    INSTALL_LOAD_PROFILE("_ZN13ScriptManager16loadDialogSoundsEv",
                         load_dialog_sounds_hook, load_dialog_sounds_hook_func);
    INSTALL_LOAD_PROFILE("_ZN13ScriptManager23loadInterruptConditionsEv",
                         load_interrupt_conditions_hook,
                         load_interrupt_conditions_hook_func);
    INSTALL_LOAD_PROFILE("_ZN13ScriptManager9loadPropsEv", load_props_hook,
                         load_props_hook_func);
    INSTALL_LOAD_PROFILE("_ZN17GameObjectManager14loadHighlightsEPKc",
                         load_highlights_hook, load_highlights_hook_func);
    INSTALL_LOAD_PROFILE("_ZN9Character13loadParticlesEv", load_particles_hook,
                         load_particles_hook_func);
    INSTALL_LOAD_PROFILE("_ZN15BehaviorManager14resolveScriptsEv",
                         resolve_behavior_scripts_hook,
                         resolve_behavior_scripts_hook_func);
    INSTALL_LOAD_PROFILE("_ZN15CControlManager4initEv", control_manager_init_hook,
                         control_manager_init_hook_func);
    INSTALL_LOAD_PROFILE("_ZN12TrainManager4initEv", train_manager_init_hook,
                         train_manager_init_hook_func);
    INSTALL_LOAD_PROFILE("_ZN11CHudManager6enableEv", hud_manager_enable_hook,
                         hud_manager_enable_hook_func);
    INSTALL_LOAD_PROFILE("_ZN6GSMain11LoadingMenu6updateEPS_",
                         loading_menu_update_hook,
                         loading_menu_update_hook_func);
    INSTALL_LOAD_PROFILE("_ZN6GSMain17playMainMenuMusicEv",
                         play_main_menu_music_hook,
                         play_main_menu_music_hook_func);
    INSTALL_LOAD_PROFILE("_ZN11Application16startSoundThreadEv",
                         application_start_sound_thread_hook,
                         application_start_sound_thread_hook_func);
    INSTALL_LOAD_PROFILE("_ZN12SoundManager15playLowFpsMusicEPKcb",
                         sound_play_low_fps_music_hook,
                         sound_play_low_fps_music_hook_func);
    INSTALL_LOAD_PROFILE("_ZN12SoundManager4initEv", sound_init_hook,
                         sound_init_hook_func);
    INSTALL_LOAD_PROFILE("_ZN3vox15VoxSoundPackXML7LoadXMLEPKc",
                         sound_pack_xml_hook, sound_pack_xml_hook_func);
    INSTALL_LOAD_PROFILE("_ZN3vox8VoxUtils24LoadDataSourceFromFileExEPKciNS_21VoxSourceLoadingFlagsEi",
                         sound_data_source_hook, sound_data_source_hook_func);
#ifdef HAVE_SHARK_LOG
    sound_application_slot = (void **)so_symbol(module, "_ZN11Application13s_applicationE");
    sound_get_device = (void *(*)(void))so_symbol(module, "_Z9GetDevicev");
    uintptr_t filesystem_vtable = so_symbol(module, "_ZTV17CCustomFileSystem");
    sound_filesystem_vptr = filesystem_vtable ? filesystem_vtable + 8 : 0;
    INSTALL_LOAD_PROFILE("_ZN3vox25CustomFileSystemInterface8OpenFileEPcNS_17VoxFileAccessModeE",
                         sound_file_open_hook, sound_file_open_hook_func);
    INSTALL_LOAD_PROFILE("_ZN3glf10FileStream4Impl4OpenEPKcNS_3ios8openflagE",
                         sound_file_stream_open_hook,
                         sound_file_stream_open_hook_func);
    INSTALL_LOAD_PROFILE("_ZN3glf10FileStream4OpenEPKcNS_3ios8openflagE",
                         sound_stream_open_hook, sound_stream_open_hook_func);
    INSTALL_LOAD_PROFILE("_ZN3vox12DecoderMSWav15CreateNewCursorEPNS_21StreamCursorInterfaceE",
                         sound_decoder_cursor_hook,
                         sound_decoder_cursor_hook_func);
#endif
    INSTALL_LOAD_PROFILE("_ZN12SoundManager15stopLowFpsMusicEv",
                         sound_stop_low_fps_music_hook,
                         sound_stop_low_fps_music_hook_func);
#undef INSTALL_LOAD_PROFILE
    loading_state_machine_get_instance =
        (void *(*)(void))so_symbol(module, "_ZN12StateMachine11getInstanceEv");
    loading_state_machine_get_state =
        (void *(*)(void *))so_symbol(module, "_ZN12StateMachine8getStateEv");
    loading_state_names = (const char **)so_symbol(module, "gLoadStates");
    loading_percent_weights = (int *)so_symbol(module, "LoadingPercents");
    if (!loading_state_machine_get_instance ||
        !loading_state_machine_get_state || !loading_state_names ||
        !loading_percent_weights) {
        LOG_ERROR("PATCH: native loading analyzer unavailable instance=%p state=%p names=%p weights=%p",
                  (void *)loading_state_machine_get_instance,
                  (void *)loading_state_machine_get_state,
                  loading_state_names, loading_percent_weights);
    } else {
        LOG_INFO("PATCH: native loading analyzer enabled (NativeGetMainLoadingProgress path)");
    }
    uintptr_t cpu_task_vtable = so_symbol(
        module, "_ZTVN6glitch4task8CCpuTaskE");
    uintptr_t graphics_task_vtable = so_symbol(
        module, "_ZTVN6glitch4task16CCpuGraphicsTaskE");
    cpu_task_vptr = cpu_task_vtable ? cpu_task_vtable + 8 : 0;
    graphics_task_vptr = graphics_task_vtable
        ? graphics_task_vtable + 8 : 0;
    LOG_INFO("PATCH: task types cpu_vptr=0x%08x graphics_vptr=0x%08x",
             (unsigned)cpu_task_vptr, (unsigned)graphics_task_vptr);

    phone_perf_slot = (void **)so_symbol(module, "gPhonePerf");
    profile_viewport_width = (int (*)(void))so_symbol(
        module, "_ZN11Application16GetViewportWidthEv");
    profile_viewport_height = (int (*)(void))so_symbol(
        module, "_ZN11Application17GetViewportHeightEv");
    uintptr_t choose_perf_profile = so_symbol(
        module, "_Z17ChoosePerfProfilei");
    if (!phone_perf_slot || !choose_perf_profile ||
        !profile_viewport_width || !profile_viewport_height) {
        LOG_ERROR("PATCH: profile symbols missing choose=%p slot=%p width=%p height=%p",
                  (void *)choose_perf_profile, phone_perf_slot,
                  (void *)profile_viewport_width, (void *)profile_viewport_height);
        return -1;
    }
    choose_perf_profile_hook = hook_addr(
        choose_perf_profile, (uintptr_t)&choose_perf_profile_hook_func);
    LOG_INFO("PATCH: engine low-memory resource tier enabled");

    uintptr_t texture_update_data = so_symbol(module,
        "_ZNK6glitch5video15CCommonGLDriverINS0_21CProgrammableGLDriverINS0_30CProgrammableShaderHandlerBaseINS0_18CGLSLShaderHandlerEEEEENS0_6detail33CProgrammableGLFunctionPointerSetEE8CTexture10updateDataEb");
    const uint32_t texture_prologue[2] = {0xe92d4ff0u, 0xe1a06000u};
    /* Verified PC-independent instructions execute in an immutable RX copy. */
    uintptr_t texture_original = immutable_continuation(module, texture_update_data, texture_prologue);
    if (!texture_original) {
        LOG_ERROR("PATCH: native texture mip continuation unavailable");
        return -1;
    }
    texture_update_data_original = (int (*)(void *, int))texture_original;
    if (runtime_patch_install_trampoline(texture_update_data,
            (uintptr_t)&texture_update_data_hook_func) < 0) return -1;
    LOG_INFO("PATCH: compressed texture cap respects native source mip count");
    uintptr_t dds_header = so_symbol(module,
        "_ZNK6glitch5video15CImageLoaderDDS17loadTextureHeaderEPNS_2io9IReadFileERNS0_12STextureDescE");
    uintptr_t dds_reader = module->text_base + 0x763d28;
    const uint32_t dds_header_prologue[2] = {0xe92d45f0u, 0xe1a05001u};
    const uint32_t dds_reader_prologue[2] = {0xe92d4070u, 0xe1a04000u};
    if (dds_header != module->text_base + 0x7640bc) {
        LOG_ERROR("PATCH: native DDS loader profile mismatch");
        return -1;
    }
    dds_header_original = (int (*)(const void *, void *, void *))
        immutable_continuation(module, dds_header, dds_header_prologue);
    dds_read_header_original = (int (*)(void *, void *))
        immutable_continuation(module, dds_reader, dds_reader_prologue);
    if (!dds_header_original || !dds_read_header_original ||
        runtime_patch_install_trampoline(dds_reader,
            (uintptr_t)&dds_read_header_hook_func) < 0 ||
        runtime_patch_install_trampoline(dds_header,
            (uintptr_t)&dds_header_hook_func) < 0) return -1;
    LOG_INFO("PATCH: native DDS mip selection enabled before payload reads");
#ifdef HAVE_SHARK_LOG
    uintptr_t hud_batch = so_symbol(module, "_ZN7ASprite8Renderer3EndEv");
    hud_batch_returns[0] = module->text_base + 0x396f2c;
    hud_batch_returns[1] = module->text_base + 0x3973a4;
    hud_optimized = (const unsigned char *)so_symbol(module, "gUseDraw2dOptim");
    uintptr_t hud_element = so_symbol(module, "_ZNK10HudElement6draw2dEv");
    uintptr_t map_draw = so_symbol(module, "_ZNK7MiniMap6draw2dEv");
    const uint32_t hud_prologue[2] = {0xe92d4070u, 0xe24dd028u};
    if (!hud_optimized || *(const uint32_t *)(hud_batch_returns[0] - 4) != 0xeb09480cu ||
        *(const uint32_t *)(hud_batch_returns[1] - 4) != 0xeb0946eeu) {
        LOG_ERROR("PATCH: native HUD batch caller profile mismatch");
        return -1;
    }
    const uint32_t map_prologue[2] = {0xe92d4ff0u, 0xed2d8b08u};
    const uint32_t element_prologue[2] = {0xe92d4010u, 0xe5903024u};
    hud_batch_original = (void (*)(const void *))immutable_continuation(module, hud_batch, hud_prologue);
    hud_element_original = (void (*)(const void *))immutable_continuation(module, hud_element, element_prologue);
    map_draw_original = (void (*)(const void *))immutable_continuation(module, map_draw, map_prologue);
    if (!hud_batch_original || !hud_element_original || !map_draw_original ||
        runtime_patch_install_trampoline(hud_batch, (uintptr_t)&hud_batch_hook) < 0 ||
        runtime_patch_install_trampoline(hud_element, (uintptr_t)&hud_element_hook) < 0 ||
        runtime_patch_install_trampoline(map_draw, (uintptr_t)&map_draw_hook) < 0) {
        LOG_ERROR("PATCH: native HUD scope profile mismatch");
        return -1;
    }
    LOG_INFO("PATCH: both native HUD flush paths, elements and map submissions captured");
#endif


    uintptr_t console_vprintln = so_symbol(
        module, "_ZN3glf7Console8VPrintlnEPKcSt9__va_list");
    if (console_vprintln) {
        (void)hook_addr(console_vprintln,
                        (uintptr_t)&console_vprintln_hook_func);
        LOG_INFO("PATCH: game console output redirected to loader.log");
    }

    uintptr_t pink_material = so_symbol(
        module,
        "_ZN6glitch5video24CMaterialRendererManager35createPinkWireFrameMaterialRendererEPKc");
    if (pink_material) {
        pink_material_hook = hook_addr(
            pink_material, (uintptr_t)&pink_material_hook_func);
        LOG_INFO("PATCH: pink fallback material tracing enabled");
    }

    /* Gameloft Tracking / Analytics:
     * When tracking is triggered, it attempts network connections or blocks
     * waiting for an HTTP/curl handshake that never finishes without network.
     * Stub TrackingManager, TrackingConnection, UrlConnection, and curl_easy_perform. */
    uintptr_t send_package = so_symbol(module, "_ZN4glot15TrackingManager11SendPackageEv");
    if (send_package) {
        runtime_patch_install_trampoline(send_package, (uintptr_t)&stub_ret0);
        LOG_INFO("PATCH: TrackingManager::SendPackage stubbed");
    }

    uintptr_t request_pkg = so_symbol(module, "_ZN4glot15TrackingManager12requestPKGIdEv");
    if (request_pkg) {
        runtime_patch_install_trampoline(request_pkg, (uintptr_t)&stub_ret0);
        LOG_INFO("PATCH: TrackingManager::requestPKGId stubbed");
    }

    uintptr_t send_data = so_symbol(module, "_ZN4glot18TrackingConnection8sendDataEbPKc");
    if (send_data) {
        runtime_patch_install_trampoline(send_data, (uintptr_t)&stub_ret0);
        LOG_INFO("PATCH: TrackingConnection::sendData stubbed");
    }

    uintptr_t conn_update = so_symbol(module, "_ZN4glot18TrackingConnection6UpdateEv");
    if (conn_update) {
        runtime_patch_install_trampoline(conn_update, (uintptr_t)&stub_ret0);
        LOG_INFO("PATCH: TrackingConnection::Update stubbed");
    }

    uintptr_t url_start = so_symbol(module, "_ZN4glwt13UrlConnection12StartRequestEv");
    if (url_start) {
        runtime_patch_install_trampoline(url_start, (uintptr_t)&stub_ret0);
        LOG_INFO("PATCH: UrlConnection::StartRequest stubbed");
    }

    uintptr_t url_run = so_symbol(module, "_ZN4glwt13UrlConnection10RunRequestEv");
    if (url_run) {
        runtime_patch_install_trampoline(url_run, (uintptr_t)&stub_ret0);
        LOG_INFO("PATCH: UrlConnection::RunRequest stubbed");
    }

    uintptr_t curl_perform = so_symbol(module, "curl_easy_perform");
    if (curl_perform) {
        runtime_patch_install_trampoline(curl_perform, (uintptr_t)&stub_curl_perform);
        LOG_INFO("PATCH: curl_easy_perform stubbed (CURLE_COULDNT_CONNECT)");
    }

    /* Main Menu & Player Render Guards */
    uintptr_t mm_ctor = so_symbol(module, "_ZN10GSMainMenu4ctorEPK12StateMachine");
    if (mm_ctor) {
        gs_main_menu_ctor_hook = hook_addr(mm_ctor, (uintptr_t)&gs_main_menu_ctor_hook_func);
        LOG_INFO("PATCH: GSMainMenu::ctor hooked");
    }

    uintptr_t mm_draw = so_symbol(module, "_ZN10GSMainMenu4drawEPK12StateMachine");
    if (mm_draw) {
        gs_main_menu_draw_hook = hook_addr(mm_draw, (uintptr_t)&gs_main_menu_draw_hook_func);
        LOG_INFO("PATCH: GSMainMenu::draw hooked");
    }

    uintptr_t mm_update = so_symbol(module, "_ZN10GSMainMenu6updateEP12StateMachine");
    if (mm_update) {
        gs_main_menu_update_hook = hook_addr(mm_update, (uintptr_t)&gs_main_menu_update_hook_func);
        LOG_INFO("PATCH: GSMainMenu::update hooked");
    }

    uintptr_t draw_player = so_symbol(module, "_Z10DrawPlayerRN7gameswf11RenderStateEPv");
    if (draw_player) {
        draw_player_hook = hook_addr(draw_player, (uintptr_t)&draw_player_hook_func);
        LOG_INFO("PATCH: DrawPlayer hooked with null guard");
    }

    uintptr_t camera_instance = so_symbol(
        module, "_ZN13CameraManager10s_instanceE");
    camera_instance_slot = camera_instance ? (void **)camera_instance : NULL;
    uintptr_t touch_moved = so_symbol(
        module, "_ZN15TouchScreenBase10touchMovedERKN3glf5PointIsEEli");
    if (touch_moved && camera_instance_slot) {
        touch_moved_hook = hook_addr(
            touch_moved, (uintptr_t)&touch_moved_hook_func);
        LOG_INFO("PATCH: TouchScreenBase::touchMoved camera-init guard enabled");
    } else {
        LOG_ERROR("PATCH: touchMoved guard unavailable touch=%p camera_slot=%p",
                  (void *)touch_moved, camera_instance_slot);
    }

    uintptr_t task_set = so_symbol(
        module, "_ZN6glitch4task15CCpuTaskHandler7setTaskEPNS0_5ITaskE");
    if (task_set) {
        (void)hook_addr(task_set, (uintptr_t)&cpu_task_set_hook_func);
        LOG_INFO("PATCH: CCpuTaskHandler::setTask lifecycle tracing enabled");
    } else {
        LOG_ERROR("PATCH: CCpuTaskHandler::setTask symbol missing");
    }

    if (native_condition_registry_init() < 0) {
        LOG_ERROR("PATCH: condition semaphore path unavailable; bounded polling fallback active");
    }

    uintptr_t condition_wait = so_symbol(
        module, "_ZN6glitch6thread10CCondition4waitEj");
    if (!condition_wait || runtime_patch_install_trampoline(
            condition_wait, (uintptr_t)&condition_wait_hook_func) < 0) {
        LOG_ERROR("PATCH: CCondition::wait bounded hook installation failed");
        return -1;
    }
    LOG_INFO("PATCH: CCondition::wait signal-aware semaphore with 10ms cap enabled");

    uintptr_t condition_signal = so_symbol(
        module, "_ZN6glitch6thread10CCondition6signalEv");
    if (condition_signal && runtime_patch_install_trampoline(
            condition_signal, (uintptr_t)&condition_signal_hook_func) >= 0) {
        LOG_INFO("PATCH: CCondition::signal semaphore notification enabled");
    } else {
        LOG_ERROR("PATCH: CCondition::signal hook unavailable; wait cap remains active");
    }

    uintptr_t condition_broadcast = so_symbol(
        module, "_ZN6glitch6thread10CCondition9broadcastEv");
    if (condition_broadcast && runtime_patch_install_trampoline(
            condition_broadcast,
            (uintptr_t)&condition_broadcast_hook_func) >= 0) {
        LOG_INFO("PATCH: CCondition::broadcast semaphore notification enabled");
    } else {
        LOG_ERROR("PATCH: CCondition::broadcast hook unavailable; wait cap remains active");
    }

    uintptr_t condition_destroy1 = so_symbol(
        module, "_ZN6glitch6thread10CConditionD1Ev");
    if (condition_destroy1 && runtime_patch_install_trampoline(
            condition_destroy1, (uintptr_t)&condition_destroy_hook_func) < 0) {
        LOG_ERROR("PATCH: CCondition D1 cleanup hook failed");
    }
    uintptr_t condition_destroy2 = so_symbol(
        module, "_ZN6glitch6thread10CConditionD2Ev");
    if (condition_destroy2 && condition_destroy2 != condition_destroy1 &&
        runtime_patch_install_trampoline(
            condition_destroy2, (uintptr_t)&condition_destroy_hook_func) < 0) {
        LOG_ERROR("PATCH: CCondition D2 cleanup hook failed");
    }

    uintptr_t task_wait = so_symbol(
        module, "_ZN6glitch4task5ITask4waitEj");
    if (!task_wait || runtime_patch_install_trampoline(
            task_wait, (uintptr_t)&task_wait_hook_func) < 0) {
        LOG_ERROR("PATCH: ITask::wait predicate hook installation failed");
        return -1;
    }
    LOG_INFO("PATCH: ITask::wait completion predicate enabled");

    task_done_address = so_symbol(module, "_ZN6glitch4task5ITask4doneEv");
    uintptr_t task_run = so_symbol(module, "_ZN6glitch4task8CCpuTask3runEv");
    if (task_run && task_done_address) {
        if (runtime_patch_install_trampoline(
                task_run, (uintptr_t)&cpu_task_run_hook_func) < 0) {
            LOG_ERROR("PATCH: CCpuTask::run guard installation failed");
            return -1;
        }
        LOG_INFO("PATCH: CCpuTask::run stale-done guard enabled");
    } else {
        LOG_ERROR("PATCH: CPU task lifecycle symbols missing run=%p done=%p",
                  (void *)task_run, (void *)task_done_address);
        return -1;
    }

    uintptr_t city_grid_process = so_symbol(module,
        "_ZN6glitch5scene21CRegularGridStreamingItE7processEPvj");
    uintptr_t city_lod_process = so_symbol(module,
        "_ZN6glitch5scene13CLODStreamingINS0_18CStreamedBatchMeshI14SSegmentExtPtr23ExtraDataHandlingPolicyNS0_25SProactiveMemoryAllocatorEEES3_S3_28GroupExtraDataHandlingPolicy26LodExtraDataHandlingPolicyE7processEPvj");
    const uint32_t city_grid_prologue[2] = {0xe92d4ff0u, 0xed2d8b02u};
    const uint32_t city_lod_prologue[2] = {0xe92d4ff0u, 0xe24dd0bcu};
    city_grid_process_original = (void (*)(void *, void *, unsigned))
        immutable_continuation(module, city_grid_process, city_grid_prologue);
    city_lod_process_original = (void (*)(void *, void *, unsigned))
        immutable_continuation(module, city_lod_process, city_lod_prologue);
    if (!city_grid_process_original || !city_lod_process_original ||
        runtime_patch_install_trampoline(
            city_grid_process, (uintptr_t)&city_grid_process_hook_func) < 0 ||
        runtime_patch_install_trampoline(
            city_lod_process, (uintptr_t)&city_lod_process_hook_func) < 0) {
        LOG_ERROR("PATCH: city streaming profiler native profile mismatch");
        return -1;
    }
    LOG_INFO("PATCH: city grid/LOD streaming profiler enabled");

    uintptr_t graphics_push = so_symbol(module, "_ZN6glitch4task16CCpuGraphicsTask4pushEv");
    collada_get_material = (const void *(*)(const void *, int))so_symbol(module,
        "_ZNK6glitch7collada16CColladaDatabase11getMaterialEi");
    if (graphics_push != module->text_base + 0x81da20 ||
        (uintptr_t)collada_get_material != module->text_base + 0x7b2e44 ||
        *(const uint32_t *)(module->text_base + 0x3fe1ec) != 0xeb107e0bu) {
        LOG_ERROR("PATCH: material prefetch native profile mismatch");
        return -1;
    }
    const uint32_t push_prologue[2] = {0xe92d4010u, 0xe1a04000u};
    graphics_task_push_original = (void (*)(void *))
        immutable_continuation(module, graphics_push, push_prologue);
    material_callback_run = module->text_base + 0x413e48;
    material_enqueue_return = module->text_base + 0x3fe1f0;
    if (!graphics_task_push_original || runtime_patch_install_trampoline(
            graphics_push, (uintptr_t)&graphics_task_push_hook_func) < 0) return -1;
    LOG_INFO("PATCH: material DDS ranges prefetched before main-thread graphics enqueue");

    task_dispatch_address = (void (*)(void *, void *, int))so_symbol(
        module,
        "_ZN6glitch4task12CTaskManager12dispatchTaskEPNS0_12ITaskHandlerEb");
    task_keep_running_address = (int (*)(void *, void *))so_symbol(
        module,
        "_ZN6glitch4task12CTaskManager11keepRunningEPNS0_12ITaskHandlerE");
    task_has_pending_address = (int (*)(void *))so_symbol(
        module,
        "_ZN6glitch4task12CTaskManager14hasPendingTaskEv");
    uintptr_t handler_run = so_symbol(
        module,
        "_ZN6glitch4task15CCpuTaskHandler3runERNS0_12CTaskManagerEb");
    if (!task_dispatch_address || !task_keep_running_address ||
        !task_has_pending_address || !handler_run) {
        LOG_ERROR("PATCH: CPU handler guard symbols missing run=%p "
                  "dispatch=%p keep=%p pending=%p",
                  (void *)handler_run, (void *)task_dispatch_address,
                  (void *)task_keep_running_address,
                  (void *)task_has_pending_address);
        return -1;
    }
    if (runtime_patch_install_trampoline(
            handler_run, (uintptr_t)&cpu_task_handler_run_hook_func) < 0) {
        LOG_ERROR("PATCH: CCpuTaskHandler::run guard installation failed");
        return -1;
    }
    LOG_INFO("PATCH: CCpuTaskHandler::run virtual-call guard enabled");

    kuKernelFlushCaches((void *)module->text_base, module->text_size);
    LOG_SUCCESS("M2 game runtime patches installed");
    return 0;
}
