/* Minimal, version-independent Miami diagnostics and ARM kuser relocation. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "utils/logger.h"
#include <sys/stat.h>
#include "reimpl/io.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/dialog.h"
#include "utils/filecache.h"

extern so_module so_mod;
extern void *__atomic_cmpxchg;

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX 0x0C20D050
#define KUSER_PAGE 0x9A000000u

static pthread_mutex_t trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static SceUID trace_fd = -1;

void port_trace(const char *format, ...) {
    char body[768];
    char line[864];
    va_list args;

    va_start(args, format);
    vsnprintf(body, sizeof(body), format, args);
    va_end(args);

    unsigned long long milliseconds =
        (unsigned long long)(sceKernelGetProcessTimeWide() / 1000);
    int length = snprintf(line, sizeof(line), "[%8llu.%03llu] %s\n",
                          milliseconds / 1000, milliseconds % 1000, body);
    if (length <= 0)
        return;
    if (length > (int)sizeof(line))
        length = sizeof(line);

    pthread_mutex_lock(&trace_mutex);
    if (trace_fd < 0)
        trace_fd = sceIoOpen(DATA_PATH "loader.log",
                             SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (trace_fd >= 0)
        sceIoWrite(trace_fd, line, (SceSize)length);
    pthread_mutex_unlock(&trace_mutex);
}

void port_trace_flush(void) {
    pthread_mutex_lock(&trace_mutex);
    if (trace_fd >= 0)
        sceIoSync("ux0:", 0);
    pthread_mutex_unlock(&trace_mutex);
}

static void app_abort_handler(KuKernelAbortContext *context) {
    port_trace("ABORT: type=%u pc=0x%08X lr=0x%08X sp=0x%08X",
               context->abortType, context->pc, context->lr, context->sp);
    port_trace("ABORT: FSR=0x%08X FAR=0x%08X SPSR=0x%08X",
               context->FSR, context->FAR, context->SPSR);
    port_trace("ABORT: r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X",
               context->r0, context->r1, context->r2, context->r3);
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0)
        port_trace("ABORT: thread=%s stack=%p size=0x%X", info.name,
                   info.stack, (unsigned)info.stackSize);
    port_trace_flush();
    kuKernelReleaseAbortHandler();
}

int port_register_abort_handler(void) {
    return kuKernelRegisterAbortHandler(app_abort_handler, NULL, NULL);
}

void __kuser_memory_barrier(void) {
    __sync_synchronize();
}

static void patch_kuser_helpers(void) {
    SceKernelAllocMemBlockKernelOpt options;
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.attr = 1;
    options.field_C = KUSER_PAGE;

    int block = kuKernelAllocMemBlock("miami-kuser",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &options);
    if (block < 0)
        fatal_error("Cannot allocate Miami kuser helper page: 0x%08X", block);
    kuKernelMemProtect((void *)KUSER_PAGE, 0x1000,
        KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

    hook_addr(KUSER_PAGE + 0xFA0, (uintptr_t)__kuser_memory_barrier);
    hook_addr(KUSER_PAGE + 0xFC0, (uintptr_t)__atomic_cmpxchg);

    unsigned patched_barriers = 0;
    unsigned patched_cmpxchg = 0;
    for (uintptr_t address = so_mod.text_base;
         address < so_mod.text_base + so_mod.text_size; address += 4) {
        uint32_t instruction = *(uint32_t *)address;
        uint32_t replacement;
        if (instruction == 0xFFFF0FA0) {
            replacement = KUSER_PAGE + 0xFA0;
            ++patched_barriers;
        } else if (instruction == 0xFFFF0FC0) {
            replacement = KUSER_PAGE + 0xFC0;
            ++patched_cmpxchg;
        } else {
            continue;
        }
        kuKernelCpuUnrestrictedMemcpy((void *)address, &replacement,
                                      sizeof(replacement));
    }
    port_trace("patch: kuser barrier=%u cmpxchg=%u", patched_barriers,
               patched_cmpxchg);
}

/*
 * This Miami binary contains a private copy of Android/bionic dlmalloc and
 * calls it directly from MemMgr (therefore import-table replacements are not
 * enough).  Its MORECORE assumptions do not hold in the Vita address space:
 * after a few constructor allocations it starts returning NULL even for a
 * 24-byte HUD object.  Redirect the complete public allocator family before
 * running any .init_array entry, so allocation, resizing, usable-size queries
 * and release all use the same newlib heap.
 */
static void patch_bionic_allocator(void) {
    struct allocator_symbol {
        const char *name;
        uintptr_t replacement;
    } symbols[] = {
        { "malloc", (uintptr_t)&malloc },
        { "calloc", (uintptr_t)&calloc },
        { "realloc", (uintptr_t)&realloc },
        { "free", (uintptr_t)&free },
        { "memalign", (uintptr_t)&memalign },
        { "valloc", (uintptr_t)&valloc },
        { "malloc_usable_size", (uintptr_t)&malloc_usable_size },
    };

    unsigned patched = 0;
    for (unsigned index = 0;
         index < sizeof(symbols) / sizeof(symbols[0]); ++index) {
        uintptr_t address = (uintptr_t)so_symbol(&so_mod, symbols[index].name);
        if (!address) {
            port_trace("patch: allocator symbol missing: %s",
                       symbols[index].name);
            continue;
        }
        hook_addr(address, symbols[index].replacement);
        port_trace("patch: allocator %s %p -> %p", symbols[index].name,
                   (void *)address, (void *)symbols[index].replacement);
        ++patched;
    }
    port_trace("patch: allocator replacements=%u", patched);
}

static void (*file_ctor)(void *), (*file_close)(void *), (*file_dtor)(void *);
static bool (*file_open)(void *, const char *, int);

static bool cached_file_exists(const char *path) {
    int cached = io_cached_file_exists(path);
    if (cached >= 0) return cached != 0;
    /* Exact FileStream::Exist fallback (0x369dc4): retain the native archive
     * and filesystem paths for anything outside the authoritative snapshot. */
    unsigned char object[0x574] __attribute__((aligned(8)));
    file_ctor(object);
    bool opened = file_open(object, path, 1);
    if (opened) file_close(object);
    file_dtor(object);
    return opened;
}

static bool (*glf_exists_original)(const void *, const char *);
static unsigned (*glf_resolve_path)(const char *, unsigned, char *, unsigned);
static void *(*glf_get_globals)(void);

static bool cached_glf_file_exists(const void *self, const char *path) {
    int cached = io_cached_file_exists(path);
    if (cached < 0 && path && strnlen(path, 1024) < 1024 && glf_get_globals()) {
        /* Native ResolvePath has strcpy branches that ignore its capacity. */
        char resolved[2048] = {0};
        glf_resolve_path(path, 0x801, resolved, 1024);
        cached = io_cached_file_exists(resolved);
    }
    if (cached == 1) {
        static atomic_uint hits = ATOMIC_VAR_INIT(0);
        if (atomic_fetch_add_explicit(&hits, 1, memory_order_relaxed) < 4)
            l_note("[IO] CGlf existence cache hit: %s", path);
        return true;
    }
    bool exists = glf_exists_original(self, path);
    static atomic_uint fallbacks = ATOMIC_VAR_INIT(0);
    if (atomic_fetch_add_explicit(&fallbacks, 1, memory_order_relaxed) < 4)
        l_note("[IO] CGlf native existence fallback returned %d: %s", exists, path ? path : "(null)");
    return exists;
}

static void patch_glf_file_exists_cache(void) {
    uintptr_t entry = so_symbol(&so_mod, "_ZNK6glitch2io14CGlfFileSystem9existFileEPKc");
    glf_resolve_path = (void *)so_symbol(&so_mod, "_ZN3glf2fs11ResolvePathEPKcNS_3ios8openflagEPcj");
    glf_get_globals = (void *)so_symbol(&so_mod, "_ZN3glf10GetGlobalsEv");
    uintptr_t address = entry & ~(uintptr_t)1;
    const uint16_t prologue[4] = {0xb5f0, 0x4647, 0xb480, 0x4f3c};
    if (!(entry & 1) || (address & 3) || !glf_resolve_path || !glf_get_globals ||
        memcmp((void *)address, prologue, sizeof(prologue))) {
        port_trace("patch: CGlf existence cache profile mismatch");
        return;
    }
    /* Relocate only the pinned prologue's literal load. Resume before the
     * remaining PC-relative instructions at their original addresses. */
    uint32_t code[5] = {0x4647b5f0, 0x4f02b480, 0xf000f8df,
                        (uint32_t)((address + 8) | 1), *(uint32_t *)(address + 0xf8)};
    uintptr_t trampoline = so_mod.patch_head;
    if ((trampoline & 3) || trampoline < so_mod.patch_base ||
        trampoline - so_mod.patch_base > so_mod.patch_size ||
        sizeof(code) > so_mod.patch_size - (trampoline - so_mod.patch_base)) {
        port_trace("patch: CGlf existence cache has no trampoline space");
        return;
    }
    so_mod.patch_head += sizeof(code);
    memcpy((void *)trampoline, code, sizeof(code));
    kuKernelFlushCaches((void *)trampoline, sizeof(code));
    glf_exists_original = (void *)(trampoline | 1);
    hook_addr(entry, (uintptr_t)cached_glf_file_exists);
    port_trace("patch: CGlfFileSystem::existFile uses asset index; archive fallback preserved");
}

static void patch_file_exists_cache(void) {
    uintptr_t exists = so_symbol(&so_mod, "_ZN10FileStream5ExistEPKc");
    file_ctor = (void (*)(void *))so_symbol(&so_mod, "_ZN10FileStreamC1Ev");
    file_open = (bool (*)(void *, const char *, int))so_symbol(&so_mod, "_ZN10FileStream4OpenEPKci");
    file_close = (void (*)(void *))so_symbol(&so_mod, "_ZN10FileStream5CloseEv");
    file_dtor = (void (*)(void *))so_symbol(&so_mod, "_ZN10FileStreamD1Ev");
    if (!exists || !file_ctor || !file_open || !file_close || !file_dtor) {
        port_trace("patch: file existence cache symbols missing");
        return;
    }
    hook_addr(exists, (uintptr_t)cached_file_exists);
    port_trace("patch: native FileStream::Exist uses asset index");
    patch_glf_file_exists_cache();
}

static const char *russian_font_name(const void *self) {
    switch (*(const int *)((const char *)self + 4)) {
        case 0: return "font_russian.ttf";
        case 5: return "font_japanese.ttf";
        case 6: return "font_korean.ttf";
        case 7: return "font_chinese.ttf";
        default: return "font_latin.ttf";
    }
}

static void patch_russian_localization(void) {
    static const char *required[] = {
        "about.russian", "ch1.russian", "ch2.russian", "ch3.russian",
        "ch4.russian", "ch5.russian", "chr.russian", "chx.russian",
        "global.russian", "init.russian", "menu.russian", "tunes.russian",
        "init.russian.default.glyph", "menu.russian.button.glyph",
        "menu.russian.labelmedium.glyph", "menu.russian.labelsmall.glyph",
        "menu.russian.labeltiny.glyph", "menu.russian.title.glyph",
        "global.russian.carname.glyph", "global.russian.dialog.glyph",
        "global.russian.labelbig.glyph", "global.russian.labelmedium.glyph",
        "global.russian.labelsmall.glyph", "global.russian.labeltiny.glyph",
        "global.russian.tutorialcontent.glyph",
        "global.russian.tutorialtitle.glyph", "font_russian.ttf",
    };
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        char path[256];
        snprintf(path, sizeof(path), GAME_DATA_PATH "%s", required[i]);
        uint32_t size = 0;
        uint16_t mode = 0;
        int cached = filecache_lookup(path, &size, &mode);
        bool present = cached == 1 && S_ISREG(mode) && size > 0;
        if (cached < 0) {
            SceIoStat st;
            present = sceIoGetstat(path, &st) >= 0 &&
                SCE_S_ISREG(st.st_mode) && st.st_size > 0;
        }
        if (!present) {
            port_trace("patch: Russian pack absent/incomplete: %s", required[i]);
            return;
        }
    }

    const char **languages = (const char **)so_symbol(&so_mod,
        "_ZN13StringManager9LANGUAGESE");
    uintptr_t get_font = so_symbol(&so_mod, "_ZNK13StringManager11getFontNameEv");
    if (!languages || !get_font) {
        port_trace("patch: Russian localization symbols missing");
        return;
    }
    /* Keep the eight native IDs: adding a ninth overruns fixed language arrays. */
    hook_addr(get_font, (uintptr_t)russian_font_name);
    languages[0] = "russian";
    port_trace("patch: Russian text/font enabled in language slot 0");
}

void so_patch(void) {
    port_trace("patch: libGangster2 text=%p size=0x%X",
               (void *)so_mod.text_base, (unsigned)so_mod.text_size);
    patch_kuser_helpers();
    patch_bionic_allocator();
    patch_file_exists_cache();
    patch_russian_localization();
    port_trace_flush();
}
