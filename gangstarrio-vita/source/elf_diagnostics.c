#include "elf_diagnostics.h"

#include "compat_resolver.h"
#include "diagnostics.h"
#include "game_profile.h"
#include "java_bridge.h"
#include "runtime_patch.h"

#include <falso_jni/FalsoJNI.h>
#include <malloc.h>
#include <reimpl/controls.h>
#include <reimpl/intro_video.h>
#include <reimpl/pthr.h>
#include <utils/glutil.h>

#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/motion.h>
#include <psp2/vshbridge.h>

#include <elf.h>
#include <so_util.h>

#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RIO_LOAD_ADDRESS ((uintptr_t)0x98000000)

static so_module rio_module;

#ifdef HAVE_SHARK_LOG
extern void mc2_memory_profile_record(void *, size_t, uintptr_t);
extern void mc2_memory_profile_forget(void *);
extern void *mc2_memory_profile_reallocate(void *, size_t, uintptr_t);
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_memalign(size_t, size_t);
extern void *__real_valloc(size_t);
extern char *__real_strdup(const char *);
extern void __real_free(void *);

void *__wrap_malloc(size_t size) {
    void *result = __real_malloc(size);
    mc2_memory_profile_record(result, size, (uintptr_t)__builtin_return_address(0));
    return result;
}

void *__wrap_calloc(size_t count, size_t size) {
    void *result = __real_calloc(count, size);
    size_t total = size && count > SIZE_MAX / size ? SIZE_MAX : count * size;
    mc2_memory_profile_record(result, total, (uintptr_t)__builtin_return_address(0));
    return result;
}

void *__wrap_realloc(void *pointer, size_t size) {
    return mc2_memory_profile_reallocate(pointer, size,
                                         (uintptr_t)__builtin_return_address(0));
}

void *__wrap_memalign(size_t alignment, size_t size) {
    void *result = __real_memalign(alignment, size);
    mc2_memory_profile_record(result, size, (uintptr_t)__builtin_return_address(0));
    return result;
}

void *__wrap_valloc(size_t size) {
    void *result = __real_valloc(size);
    mc2_memory_profile_record(result, size, (uintptr_t)__builtin_return_address(0));
    return result;
}

char *__wrap_strdup(const char *value) {
    char *result = __real_strdup(value);
    mc2_memory_profile_record(result, strlen(value) + 1,
                             (uintptr_t)__builtin_return_address(0));
    return result;
}

void __wrap_free(void *pointer) {
    mc2_memory_profile_forget(pointer);
    __real_free(pointer);
}
#endif

typedef jint (*RioJniOnLoad)(JavaVM *, void *);
typedef void (*RioVoidCall)(JNIEnv *, jclass);
typedef void (*RioBooleanCall)(JNIEnv *, jclass, jboolean);
typedef void (*RioResizeCall)(JNIEnv *, jclass, jint, jint);
typedef void (*RioKeyCall)(JNIEnv *, jclass, jint, jboolean);
typedef void (*RioTouchCall)(JNIEnv *, jclass, jint, jint, jint, jint);
typedef void (*RioAnalogCall)(JNIEnv *, jclass, jfloat, jfloat);
typedef void (*RioConnectedCall)(JNIEnv *, jclass, jint);

static RioKeyCall rio_key_event;
static RioTouchCall rio_touch_event;
static RioAnalogCall rio_right_analog;
static RioConnectedCall rio_set_connected;
static void **rio_application;
static jclass rio_gl2_class;
static int rio_input_ready;
static int rio_controller_connected;
static int rio_start_gate = 1;
static unsigned rio_dpad_directions;
static unsigned rio_stick_directions;
static void **rio_player;
static void *(*rio_get_vehicle)(const void *);
static int (*rio_get_spawn_type)(const void *);
static void (*rio_player_use_nitros)(void *);
static void (*rio_player_move)(void *, float, float);
static void (*rio_add_cash)(void *, int, int);
static void (*rio_level_up)(void *, int);
static const uint8_t *rio_ui_flags[7];
static int (*rio_is_in_game)(void);
static int (*rio_is_landscape_reversed)(const void *);
static int32_t *rio_accelerometer_lean_center;
static int rio_motion_cal_pending = 1;
static int rio_cross_nitro_latched;
static int rio_circle_vehicle_latched;
static void *rio_moving_player;
static unsigned rio_stop_steps;
static unsigned rio_cheat_step, rio_cheat_swallowed;
static uint64_t rio_cheat_step_ms, rio_cheat_deadline_ms;
static uintptr_t rio_moto_vptr;
static uintptr_t rio_acceleration_vptr;
static void *(*rio_event_manager)(void);
static void (*rio_raise_event)(void *, const void *);
typedef struct {
    uintptr_t vptr;
    uint32_t type;
    float x, y, z;
} RioAccelerationEvent;
_Static_assert(sizeof(RioAccelerationEvent) == 20, "native EvAcceleration layout");
_Static_assert(offsetof(RioAccelerationEvent, x) == 8 &&
               offsetof(RioAccelerationEvent, y) == 12 &&
               offsetof(RioAccelerationEvent, z) == 16, "native acceleration fields");

static JNIEnv *rio_AndroidOS_GetEnv(void) {
    return &jni;
}

static void rio_AndroidReleaseThreadEnv(void) {
}

static int module_loaded(const char *name) {
    int work[2] = {0, 0};
    return _vshKernelSearchModuleByName(name, work) >= 0;
}

static int relocation_supported(unsigned int type) {
    switch (type) {
        case R_ARM_ABS32:
        case R_ARM_RELATIVE:
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
            return 1;
        default:
            return 0;
    }
}

static Elf32_Rel *relocation_at(so_module *module, int index) {
    if (index < module->num_reldyn) {
        return &module->reldyn[index];
    }
    return &module->relplt[index - module->num_reldyn];
}

ElfDiagnosticStatus elf_diagnostics_run(void) {
    int load_result;
    int relocation_result;
    int relocation_count;
    int import_count = 0;
    int jni_export_count = 0;
    int unsupported_count = 0;
    int resolved_count = 0;
    int missing_count = 0;
    int bound_relocations = 0;
    unsigned char *seen_symbols;

    LOG_INFO("M1: checking kubridge");
    if (!module_loaded("kubridge")) {
        LOG_ERROR("kubridge.skprx is not loaded");
        return ELF_DIAGNOSTIC_KUBRIDGE_MISSING;
    }

    LOG_INFO("M1: mapping ELF at 0x%08x", (unsigned int)RIO_LOAD_ADDRESS);
    load_result = so_file_load(&rio_module, RIO_GAME_SO_PATH, RIO_LOAD_ADDRESS);
    if (load_result < 0) {
        LOG_ERROR("so_file_load failed: 0x%08x", load_result);
        return ELF_DIAGNOSTIC_LOAD_FAILED;
    }

    if (rio_module.dynsym == NULL || rio_module.dynstr == NULL ||
        rio_module.reldyn == NULL || rio_module.relplt == NULL ||
        rio_module.num_dynsym <= 0 || rio_module.num_reldyn < 0 ||
        rio_module.num_relplt < 0) {
        LOG_ERROR("mapped ELF has invalid dynamic tables");
        return ELF_DIAGNOSTIC_INVALID_TABLES;
    }

    LOG_SUCCESS("ELF mapped: text=0x%08x size=%u data_segments=%d",
                (unsigned int)rio_module.text_base,
                (unsigned int)rio_module.text_size,
                rio_module.n_data);
    for (int d = 0; d < rio_module.n_data; ++d) {
        LOG_INFO("  data[%d]: base=0x%08x size=%u",
                 d, (unsigned int)rio_module.data_base[d], (unsigned int)rio_module.data_size[d]);
    }
    LOG_INFO("dynamic tables: symbols=%d rel.dyn=%d rel.plt=%d init_array=%d",
             rio_module.num_dynsym,
             rio_module.num_reldyn,
             rio_module.num_relplt,
             rio_module.num_init_array);

    seen_symbols = calloc((size_t)rio_module.num_dynsym, sizeof(*seen_symbols));
    if (seen_symbols == NULL) {
        LOG_ERROR("cannot allocate import scan table");
        return ELF_DIAGNOSTIC_OUT_OF_MEMORY;
    }

    relocation_count = rio_module.num_reldyn + rio_module.num_relplt;
    for (int index = 0; index < relocation_count; ++index) {
        Elf32_Rel *relocation = relocation_at(&rio_module, index);
        unsigned int type = ELF32_R_TYPE(relocation->r_info);
        unsigned int symbol_index = ELF32_R_SYM(relocation->r_info);

        if (!relocation_supported(type)) {
            LOG_ERROR("unsupported relocation type %u at index %d", type, index);
            ++unsupported_count;
        }

        if (symbol_index >= (unsigned int)rio_module.num_dynsym) {
            LOG_ERROR("relocation %d has invalid symbol index %u",
                      index, symbol_index);
            free(seen_symbols);
            return ELF_DIAGNOSTIC_INVALID_TABLES;
        }

        Elf32_Sym *symbol = &rio_module.dynsym[symbol_index];
        if (symbol->st_shndx == SHN_UNDEF && symbol->st_name != 0 &&
            !seen_symbols[symbol_index]) {
            const char *name = rio_module.dynstr + symbol->st_name;
            seen_symbols[symbol_index] = 1;
            ++import_count;
            diagnostics_log("IMPORT", "%s", name);
        }
    }
    free(seen_symbols);

    for (int index = 0; index < rio_module.num_dynsym; ++index) {
        Elf32_Sym *symbol = &rio_module.dynsym[index];
        if (symbol->st_shndx != SHN_UNDEF && symbol->st_name != 0) {
            const char *name = rio_module.dynstr + symbol->st_name;
            if (strcmp(name, "JNI_OnLoad") == 0 ||
                strncmp(name, "Java_", 5) == 0) {
                ++jni_export_count;
                diagnostics_log("EXPORT", "%s", name);
            }
        }
    }

    LOG_INFO("ELF audit: relocations=%d imports=%d JNI_exports=%d unsupported=%d",
             relocation_count, import_count, jni_export_count,
             unsupported_count);
    if (relocation_count != RIO_GAME_RELOCATION_COUNT ||
        import_count != RIO_GAME_IMPORT_COUNT ||
        jni_export_count != RIO_GAME_JNI_EXPORT_COUNT) {
        LOG_ERROR("ELF profile mismatch: expected relocations=%d imports=%d JNI_exports=%d",
                  RIO_GAME_RELOCATION_COUNT, RIO_GAME_IMPORT_COUNT,
                  RIO_GAME_JNI_EXPORT_COUNT);
        return ELF_DIAGNOSTIC_INVALID_TABLES;
    }
    if (unsupported_count != 0) {
        return ELF_DIAGNOSTIC_UNSUPPORTED_RELOCATION;
    }

    relocation_result = so_relocate(&rio_module);
    if (relocation_result < 0) {
        LOG_ERROR("so_relocate failed: 0x%08x", relocation_result);
        return ELF_DIAGNOSTIC_RELOCATE_FAILED;
    }

    LOG_SUCCESS("M1 complete: internal relocations applied");
    if (compat_validate_imports(&rio_module, &resolved_count, &missing_count) < 0) {
        LOG_ERROR("M2 import validation failed: resolved=%d missing=%d",
                  resolved_count, missing_count);
        return ELF_DIAGNOSTIC_UNRESOLVED_IMPORTS;
    }
    LOG_SUCCESS("M2 import validation complete: resolved=%d missing=%d",
                resolved_count, missing_count);
    if (compat_bind_imports(&rio_module, &bound_relocations) < 0) {
        LOG_ERROR("M2 import binding failed");
        return ELF_DIAGNOSTIC_BIND_FAILED;
    }
    if (bound_relocations != RIO_GAME_IMPORT_RELOCATION_COUNT) {
        LOG_ERROR("M2 bound relocation mismatch: %d (expected %d)",
                  bound_relocations, RIO_GAME_IMPORT_RELOCATION_COUNT);
        return ELF_DIAGNOSTIC_BIND_FAILED;
    }
    LOG_SUCCESS("M2 import binding complete: relocations=%d",
                bound_relocations);
    if (runtime_patch_android_kuser(&rio_module) < 0) {
        return ELF_DIAGNOSTIC_PATCH_FAILED;
    }
    if (runtime_patch_game(&rio_module) < 0) {
        return ELF_DIAGNOSTIC_PATCH_FAILED;
    }
    LOG_SUCCESS("M2 complete: executable image is ready for runtime bring-up");
    diagnostics_flush();
    return ELF_DIAGNOSTIC_OK;
}

const char *elf_diagnostic_status_string(ElfDiagnosticStatus status) {
    switch (status) {
        case ELF_DIAGNOSTIC_OK:
            return "ok";
        case ELF_DIAGNOSTIC_KUBRIDGE_MISSING:
            return "kubridge missing";
        case ELF_DIAGNOSTIC_LOAD_FAILED:
            return "ELF load failed";
        case ELF_DIAGNOSTIC_INVALID_TABLES:
            return "invalid dynamic tables";
        case ELF_DIAGNOSTIC_OUT_OF_MEMORY:
            return "out of memory";
        case ELF_DIAGNOSTIC_UNSUPPORTED_RELOCATION:
            return "unsupported relocation";
        case ELF_DIAGNOSTIC_RELOCATE_FAILED:
            return "relocation failed";
        case ELF_DIAGNOSTIC_UNRESOLVED_IMPORTS:
            return "unresolved imports";
        case ELF_DIAGNOSTIC_BIND_FAILED:
            return "import binding failed";
        case ELF_DIAGNOSTIC_PATCH_FAILED:
            return "runtime patch failed";
        default:
            return "unknown";
    }
}

static uintptr_t runtime_symbol(const char *name) {
    uintptr_t address = so_symbol(&rio_module, name);
    if (address == 0) {
        LOG_ERROR("M3 required symbol missing: %s", name);
    } else {
        LOG_INFO("M3 symbol: %s -> 0x%08x", name, (unsigned int)address);
    }
    return address;
}

static void *input_player(void) {
    for (unsigned i = 0; i < sizeof rio_ui_flags / sizeof rio_ui_flags[0]; ++i)
        if (*rio_ui_flags[i]) return NULL;
    return *rio_player;
}

static int handle_cheat_key(int keycode, int pressed) {
    unsigned bit = keycode == AKEYCODE_BUTTON_X ? 1u :
                   keycode == AKEYCODE_BUTTON_Y ? 2u : 0;
    if (!pressed && (rio_cheat_swallowed & bit)) {
        rio_cheat_swallowed &= ~bit;
        return 1;
    }
    void *player = input_player();
    if (!player || rio_get_vehicle(player)) {
        rio_cheat_step = 0;
        rio_cheat_deadline_ms = 0;
        return 0;
    }
    if (!pressed) return 0;
    uint64_t now = sceKernelGetSystemTimeWide() / 1000;
    if (bit && rio_cheat_deadline_ms && now < rio_cheat_deadline_ms) {
        rio_cheat_deadline_ms = 0;
        rio_cheat_swallowed |= bit;
        if (bit == 1u) {
            rio_add_cash(player, 50000, 1);
            LOG_SUCCESS("INPUT CHEAT: added 50000 cash");
        } else {
            rio_level_up(player, 1);
            LOG_SUCCESS("INPUT CHEAT: raised level by one");
        }
        return 1;
    }
    if (keycode == AKEYCODE_BUTTON_L1) {
        rio_cheat_step = 1;
        rio_cheat_step_ms = now;
        rio_cheat_deadline_ms = 0;
    } else if (keycode == AKEYCODE_BUTTON_R1) {
        rio_cheat_step = rio_cheat_step == 1 && now - rio_cheat_step_ms < 2000 ? 2 : 0;
        rio_cheat_step_ms = now;
    } else if (keycode == AKEYCODE_DPAD_UP && rio_cheat_step == 2) {
        if (now - rio_cheat_step_ms < 2000) {
            rio_cheat_deadline_ms = now + 5000;
            LOG_INFO("INPUT CHEAT: armed for Square or Triangle");
        }
        rio_cheat_step = 0;
    }
    return 0;
}

static void update_directions(unsigned dpad, unsigned stick) {
    unsigned previous = rio_dpad_directions | rio_stick_directions;
    unsigned current = dpad | stick;
    rio_dpad_directions = dpad;
    rio_stick_directions = stick;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned bit = 1u << i;
        if ((previous ^ current) & bit) {
            rio_key_event(&jni, rio_gl2_class, AKEYCODE_DPAD_UP + i,
                          (current & bit) ? JNI_TRUE : JNI_FALSE);
        }
    }
}

static int player_uses_road_vehicle(void *player) {
    void *vehicle = player ? rio_get_vehicle(player) : NULL;
    if (!vehicle) return 0;
    int type = rio_get_spawn_type(vehicle);
    return type == 4 || type == 5;
}

void elf_runtime_handle_key(int keycode, int pressed) {
    if (!rio_input_ready || !rio_key_event) return;
    /* Native GameObjectType 4/5 are Car/Moto; aircraft need their gas key. */
    if (keycode == AKEYCODE_DPAD_CENTER) {
        if (rio_cross_nitro_latched) {
            if (!pressed) rio_cross_nitro_latched = 0;
            return;
        }
        if (pressed) {
            void *player = input_player();
            if (player_uses_road_vehicle(player)) {
                rio_player_use_nitros(player);
                rio_cross_nitro_latched = 1;
                return;
            }
        }
    }
    /* Circle maps to Android B, whose native vehicle handler is brake. */
    if (keycode == AKEYCODE_BACK) keycode = 123;
    if (keycode == 123) {
        if (rio_circle_vehicle_latched) {
            if (!pressed) rio_circle_vehicle_latched = 0;
            return;
        }
        if (pressed) {
            void *player = input_player();
            if (player_uses_road_vehicle(player)) {
                rio_circle_vehicle_latched = 1;
                return;
            }
        }
    }
    if (handle_cheat_key(keycode, pressed)) return;
    if (keycode >= AKEYCODE_DPAD_UP && keycode <= AKEYCODE_DPAD_RIGHT) {
        unsigned bit = 1u << (keycode - AKEYCODE_DPAD_UP);
        unsigned dpad = pressed ? rio_dpad_directions | bit :
                                  rio_dpad_directions & ~bit;
        update_directions(dpad, rio_stick_directions);
        return;
    }
    rio_key_event(&jni, rio_gl2_class, keycode,
                  pressed ? JNI_TRUE : JNI_FALSE);
}

void elf_runtime_handle_touch(int id, int x, int y, int action) {
    if (!rio_input_ready || !rio_touch_event) {
        return;
    }
    rio_touch_event(&jni, rio_gl2_class, action, x, y, id);
}

void elf_runtime_handle_analog(int which, float x, float y) {
    if (!rio_input_ready || !rio_key_event) {
        return;
    }
    if (which == CONTROLS_STICK_LEFT) {
        void *player = input_player();
        if (player && !rio_get_vehicle(player)) {
            update_directions(rio_dpad_directions, 0);
            if (rio_moving_player != player) rio_stop_steps = 0;
            rio_moving_player = player;
            float magnitude = sqrtf(x * x + y * y);
            if (magnitude > 0) {
                rio_player_move(player, atan2f(-x, -y), fminf(magnitude, 1.0f));
                rio_stop_steps = 5;
            } else if (rio_stop_steps) {
                if (!rio_dpad_directions) rio_player_move(player, 0, 0);
                --rio_stop_steps;
            }
            return;
        }
        if (rio_moving_player != *rio_player) {
            rio_moving_player = NULL;
            rio_stop_steps = 0;
        }
        rio_cheat_step = 0;
        rio_cheat_deadline_ms = 0;
        unsigned directions = 0;
        /* Rio's left PowerA JNI export is empty; movement uses D-pad keys. */
        if (y < -0.35f) directions |= 1u;
        if (y > 0.35f) directions |= 2u;
        if (x < -0.35f) directions |= 4u;
        if (x > 0.35f) directions |= 8u;
        update_directions(rio_dpad_directions, directions);
    } else if (which == CONTROLS_STICK_RIGHT && rio_right_analog) {
        rio_right_analog(&jni, rio_gl2_class, x * 2.5f, y * 2.5f);
    }
}

static void poll_motorcycle_motion(void) {
    void *player = input_player();
    if (!player) return;
    void *vehicle = rio_get_vehicle(player);
    if (!vehicle || *(const uintptr_t *)vehicle != rio_moto_vptr) return;
    SceMotionState motion;
    int result = sceMotionGetState(&motion);
    if (result < 0) {
        static int error_logged;
        if (!error_logged++) LOG_ERROR("INPUT MOTION: read failed 0x%x", result);
        return;
    }
    float pitch = -motion.acceleration.y;
    float z = -motion.acceleration.z;
    if (!isfinite(pitch) || !isfinite(z)) return;
    /* Vita reports g already; rotate landscape axes and leave steering neutral. */
    /* Android disables sensor events in PowerA mode; raise the same native event. */
    RioAccelerationEvent event = {rio_acceleration_vptr, 6, pitch, 0, z};
    rio_raise_event(rio_event_manager(), &event);
    static unsigned samples;
    static float previous_pitch;
    if (samples < 24 && (samples < 3 || fabsf(pitch - previous_pitch) >= 0.1f)) {
        LOG_INFO("INPUT MOTION: motorcycle timestamp=%u raw=%g/%g/%g native=%g/0/%g",
                 motion.timestamp, motion.acceleration.x, motion.acceleration.y,
                 motion.acceleration.z, pitch, z);
        previous_pitch = pitch;
        ++samples;
    }
}

static void calibrate_motion_on_world_entry(void) {
    if (*rio_ui_flags[4] || *rio_ui_flags[6])
        rio_motion_cal_pending = 1;
    if (!rio_motion_cal_pending || !rio_is_in_game ||
        !rio_is_landscape_reversed || !rio_accelerometer_lean_center ||
        !rio_application || !*rio_application || !input_player() ||
        !rio_is_in_game()) return;

    SceMotionState motion;
    if (sceMotionGetState(&motion) < 0) return;
    float x = -motion.acceleration.y;
    float z = -motion.acceleration.z;
    if (!isfinite(x) || !isfinite(z) || fabsf(z) < 0.001f) return;

    float angle = (float)atan2((double)x, (double)z);
    if (!isfinite(angle)) return;
    int reversed = rio_is_landscape_reversed(*rio_application);
    if (reversed) angle = -angle;
    int center = (int)((angle * 180.0f) / 3.14f);
    *rio_accelerometer_lean_center = center;
    rio_motion_cal_pending = 0;
    LOG_SUCCESS("INPUT MOTION: world-entry calibration center=%d angle=%g reversed=%d sample=%g/0/%g",
                center, angle, reversed, x, z);
}

static void ensure_controller_connected(void) {
    if (rio_controller_connected || !rio_set_connected || !rio_application)
        return;
    void *application = *rio_application;
    if (!application)
        return;
    rio_set_connected(&jni, rio_gl2_class, 1);
    rio_controller_connected = 1;
    LOG_SUCCESS("INPUT: PowerA mode enabled application=%p", application);
}

static volatile unsigned current_frame = 0;
static volatile uint64_t last_step_start = 0;
static volatile SceUID main_thread_uid = 0;

extern void mc2_memory_profile_report(void);

static int watchdog_thread_func(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    unsigned sec = 0;
    unsigned user_min = UINT32_MAX;
    unsigned cdram_min = UINT32_MAX;
    unsigned phycont_min = UINT32_MAX;
    size_t heap_min = SIZE_MAX;
    size_t vgl_ram_min = SIZE_MAX;
    size_t vgl_vram_min = SIZE_MAX;
    size_t vgl_slow_min = SIZE_MAX;
    unsigned samples = 0;
    for (;;) {
        sceKernelDelayThread(250 * 1000);
        uint64_t now = sceKernelGetProcessTimeWide();
        uint64_t in_step_us = last_step_start ? (now - last_step_start) : 0;
        SceKernelFreeMemorySizeInfo memory;
        memset(&memory, 0, sizeof(memory));
        memory.size = sizeof(memory);
        int memory_result = sceKernelGetFreeMemorySize(&memory);
        if (memory_result >= 0) {
            if ((unsigned)memory.size_user < user_min) user_min = memory.size_user;
            if ((unsigned)memory.size_cdram < cdram_min) cdram_min = memory.size_cdram;
            if ((unsigned)memory.size_phycont < phycont_min) phycont_min = memory.size_phycont;
        }
        struct mallinfo heap = mallinfo();
        size_t vgl_ram_free = vglMemFree(VGL_MEM_RAM);
        size_t vgl_vram_free = vglMemFree(VGL_MEM_VRAM);
        size_t vgl_slow_free = vglMemFree(VGL_MEM_SLOW);
        if (heap.fordblks < heap_min) heap_min = heap.fordblks;
        if (vgl_ram_free < vgl_ram_min) vgl_ram_min = vgl_ram_free;
        if (vgl_vram_free < vgl_vram_min) vgl_vram_min = vgl_vram_free;
        if (vgl_slow_free < vgl_slow_min) vgl_slow_min = vgl_slow_free;
        if (++samples >= 8) {
            sec += 2;
#ifdef HAVE_SHARK_LOG
            if (sec % 30 == 0) {
                mc2_memory_profile_report();
                unsigned created, destroyed;
                fjni_reference_stats(&created, &destroyed);
                LOG_INFO("JNI REFS: created=%u destroyed=%u live=%u",
                         created, destroyed, created - destroyed);
            }
#endif
            if (memory_result >= 0) {
                LOG_INFO("WATCHDOG: t=%us current_frame=%u in_step=%llu ms "
                         "sys_user=%u KB min=%u KB sys_cdram=%u KB min=%u KB "
                         "sys_phycont=%u KB min=%u KB "
                         "newlib_heap(arena=%u used=%u free=%u min_free=%u KB) "
                         "vgl_ram=%u/%u KB min=%u KB vgl_vram=%u/%u KB min=%u KB "
                         "vgl_slow=%u/%u KB min=%u KB",
                         sec, current_frame, (unsigned long long)(in_step_us / 1000),
                         (unsigned)memory.size_user / 1024, user_min / 1024,
                         (unsigned)memory.size_cdram / 1024, cdram_min / 1024,
                         (unsigned)memory.size_phycont / 1024, phycont_min / 1024,
                         (unsigned)(heap.arena / 1024), (unsigned)(heap.uordblks / 1024),
                         (unsigned)(heap.fordblks / 1024), (unsigned)(heap_min / 1024),
                         (unsigned)(vgl_ram_free / 1024),
                         (unsigned)(vglMemTotal(VGL_MEM_RAM) / 1024),
                         (unsigned)(vgl_ram_min / 1024),
                         (unsigned)(vgl_vram_free / 1024),
                         (unsigned)(vglMemTotal(VGL_MEM_VRAM) / 1024),
                         (unsigned)(vgl_vram_min / 1024),
                         (unsigned)(vgl_slow_free / 1024),
                         (unsigned)(vglMemTotal(VGL_MEM_SLOW) / 1024),
                         (unsigned)(vgl_slow_min / 1024));
                user_min = (unsigned)memory.size_user;
                cdram_min = (unsigned)memory.size_cdram;
                phycont_min = (unsigned)memory.size_phycont;
                heap_min = heap.fordblks;
                vgl_ram_min = vgl_ram_free;
                vgl_vram_min = vgl_vram_free;
                vgl_slow_min = vgl_slow_free;
            } else {
                LOG_ERROR("WATCHDOG: sceKernelGetFreeMemorySize failed err=0x%08x",
                          (unsigned)memory_result);
            }
            samples = 0;
        }
        if (in_step_us >= 3000 * 1000) {
            SceKernelThreadInfo main_info;
            memset(&main_info, 0, sizeof(main_info));
            main_info.size = sizeof(main_info);
            if (main_thread_uid > 0 && sceKernelGetThreadInfo(main_thread_uid, &main_info) >= 0) {
                diagnostics_log("WARN",
                                "WATCHDOG MAIN THREAD: name=\"%s\" status=0x%08X waitType=0x%X waitId=0x%08X cpu=%d entry=%p stack=%p-0x%X",
                                main_info.name, (unsigned)main_info.status,
                                (unsigned)main_info.waitType, (unsigned)main_info.waitId,
                                main_info.currentCpuId, main_info.entry,
                                main_info.stack, (unsigned)main_info.stackSize);
            }
            diagnostics_render_progress();
#ifdef HAVE_SHARK_LOG
            diagnostics_gpu_progress(128);
#endif
            runtime_patch_dump_state();
            pthr_dump_object_stats();
        } else if (sec % 30 == 0) {
            pthr_dump_object_stats();
        }
        if (in_step_us >= 3000 * 1000 || sec % 10 == 0)
            diagnostics_flush();
    }
    return 0;
}

int elf_runtime_run(void) {
    main_thread_uid = sceKernelGetThreadId();
#define RIO_SYMBOL(name, type) ((type)runtime_symbol( \
    "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_" name))
    RioJniOnLoad on_load = (RioJniOnLoad)runtime_symbol("JNI_OnLoad");
    RioVoidCall installer_init = (RioVoidCall)runtime_symbol(
        "Java_com_gameloft_android_ANMP_GloftG4HM_installer_"
        "GameInstaller_initNative");
    RioVoidCall installer_public_key = (RioVoidCall)runtime_symbol(
        "Java_com_gameloft_android_ANMP_GloftG4HM_installer_"
        "GameInstaller_getPublicKey");
    RioVoidCall installer_start = (RioVoidCall)runtime_symbol(
        "Java_com_gameloft_android_ANMP_GloftG4HM_installer_"
        "GameInstaller_nativeStart");
    RioVoidCall init = RIO_SYMBOL("init", RioVoidCall);
    RioSetPathsCall set_paths = RIO_SYMBOL("setPaths", RioSetPathsCall);
    RioVoidCall init_gl = RIO_SYMBOL("initGL", RioVoidCall);
    RioVoidCall get_view_settings =
        RIO_SYMBOL("getViewSettings", RioVoidCall);
    RioBooleanCall state_changed =
        RIO_SYMBOL("stateChanged", RioBooleanCall);
    RioResizeCall resize = RIO_SYMBOL("resize", RioResizeCall);
    RioVoidCall step = RIO_SYMBOL("step", RioVoidCall);

    rio_key_event = RIO_SYMBOL("keyEvent", RioKeyCall);
    rio_touch_event = RIO_SYMBOL("touchEvent", RioTouchCall);
    rio_right_analog = RIO_SYMBOL("nativeSetPowerARightJoystick", RioAnalogCall);
    rio_set_connected =
        RIO_SYMBOL("nativeSetPowerAConnected", RioConnectedCall);
    rio_application = (void **)runtime_symbol(
        "_ZN11Application13s_applicationE");
    rio_player = (void **)runtime_symbol("_ZN6Player8s_playerE");
    rio_get_vehicle = (void *(*)(const void *))runtime_symbol("_ZNK6Player10getVehicleEv");
    rio_get_spawn_type = (int (*)(const void *))runtime_symbol("_ZN11LevelObject12getSpawnTypeEv");
    rio_player_use_nitros = (void (*)(void *))runtime_symbol("_ZN6Player9useNitrosEv");
    rio_is_in_game = (int (*)(void))runtime_symbol("_ZN9GS3DStuff8isInGameEv");
    rio_is_landscape_reversed = (int (*)(const void *))runtime_symbol(
        "_ZNK11Application19isLandscapeReversedEv");
    rio_accelerometer_lean_center = (int32_t *)runtime_symbol(
        "_ZN7Vehicle23accelerometerLeanCenterE");
    rio_player_move = (void (*)(void *, float, float))runtime_symbol("_ZN6Player4moveEff");
    rio_add_cash = (void (*)(void *, int, int))runtime_symbol("_ZN6Player7addCashEii");
    rio_level_up = (void (*)(void *, int))runtime_symbol("_ZN6Player12ForceLevelUpEi");
    uintptr_t moto_table = runtime_symbol("_ZTV4Moto");
    rio_moto_vptr = moto_table ? moto_table + 2 * sizeof(uintptr_t) : 0;
    uintptr_t acceleration_table = runtime_symbol("_ZTV14EvAcceleration");
    rio_acceleration_vptr = acceleration_table ? acceleration_table + 2 * sizeof(uintptr_t) : 0;
    rio_event_manager = (void *(*)(void))runtime_symbol("_ZN12EventManager11getInstanceEv");
    rio_raise_event = (void (*)(void *, const void *))runtime_symbol("_ZN12EventManager5raiseERK6IEvent");
    const char *ui_names[] = {
        "_ZN12StateMachine16s_isInInGameMenuE",
        "_ZN12StateMachine14s_isInShopMenuE",
        "_ZN12StateMachine20s_isInWeaponShopMenuE",
        "_ZN12StateMachine19s_isInCarRentalMenuE",
        "_ZN12StateMachine17s_isInLoadingMenuE",
        "_ZN12StateMachine18s_isInSplashScreenE",
        "_ZN12StateMachine14s_isInMainMenuE"
    };
    int input_symbols_ready = rio_key_event && rio_touch_event && rio_right_analog &&
        rio_player && rio_get_vehicle && rio_get_spawn_type && rio_player_use_nitros && rio_player_move &&
        rio_add_cash && rio_level_up && rio_moto_vptr && rio_acceleration_vptr &&
        rio_event_manager && rio_raise_event && rio_is_in_game &&
        rio_is_landscape_reversed && rio_accelerometer_lean_center && rio_application;
    for (unsigned i = 0; i < sizeof rio_ui_flags / sizeof rio_ui_flags[0]; ++i) {
        rio_ui_flags[i] = (const uint8_t *)runtime_symbol(ui_names[i]);
        if (!rio_ui_flags[i]) input_symbols_ready = 0;
    }
#undef RIO_SYMBOL

    if (!on_load || !installer_init || !installer_public_key ||
        !installer_start ||
        !init || !set_paths || !init_gl ||
        !get_view_settings || !state_changed || !resize || !step || !input_symbols_ready) {
        LOG_ERROR("M3 stopped: incomplete native lifecycle");
        diagnostics_close();
        sceKernelDelayThread(5 * 1000 * 1000);
        return 1;
    }

    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    so_flush_caches(&rio_module);
    LOG_INFO("M3: executing %d ELF constructors", rio_module.num_init_array);
    for (int index = 0; index < rio_module.num_init_array; ++index) {
        uintptr_t address = (uintptr_t)rio_module.init_array[index];
        if (address == 0 || address == UINTPTR_MAX) {
            continue;
        }
        LOG_INFO("M3 constructor %d/%d begin: 0x%08x", index + 1,
                 rio_module.num_init_array, (unsigned int)address);
        rio_module.init_array[index]();
        LOG_INFO("M3 constructor %d/%d complete", index + 1,
                 rio_module.num_init_array);
    }
    LOG_SUCCESS("M3: ELF constructors complete");
    diagnostics_flush();

    LOG_INFO("M3: preloading Vita shader compiler");
    gl_preload();
    LOG_SUCCESS("M3: Vita shader compiler ready");

    jni_init();
#ifdef HAVE_SHARK_LOG
    extern int fjni_run_reference_tests(JNIEnv *);
    if (fjni_run_reference_tests(&jni) != 0) {
        LOG_ERROR("M3 stopped: JNI reference lifetime tests failed");
        diagnostics_flush();
        return 1;
    }
#endif
    java_init();
    controls_init();
    LOG_SUCCESS("M3: FalsoJNI and Vita controls initialized");

    LOG_INFO("M3: calling JNI_OnLoad");
    jint jni_version = on_load(&jvm, NULL);
    LOG_SUCCESS("M3: JNI_OnLoad returned 0x%08x", (unsigned int)jni_version);

    /* Bind JavaVM pointers for AndroidOS and InAppBilling */
    JavaVM **android_os_jvm = (JavaVM **)runtime_symbol("AndroidOS_JavaVM");
    if (android_os_jvm) {
        *android_os_jvm = &jvm;
        LOG_INFO("M3 symbol: AndroidOS_JavaVM -> %p", (void *)android_os_jvm);
    }

    JavaVM **in_app_billing_jvm = (JavaVM **)runtime_symbol("_ZN12InAppBilling7mJavaVM");
    if (in_app_billing_jvm) {
        *in_app_billing_jvm = &jvm;
        LOG_INFO("M3 symbol: _ZN12InAppBilling7mJavaVM -> %p", (void *)in_app_billing_jvm);
    }

    /* Pre-populate AndroidOS global path buffers so getters return clean paths without JNI roundtrips */
    char *sd_path = (char *)runtime_symbol("_ZN9AndroidOS7mSDPathE");
    if (sd_path) {
        strncpy(sd_path, "ux0:data/gangstarrio", 256);
    }
    char *save_path = (char *)runtime_symbol("_ZN9AndroidOS9mSavePathE");
    if (save_path) {
        strncpy(save_path, "ux0:data/gangstarrio/files", 256);
    }
    char *user_agent = (char *)runtime_symbol("_ZN9AndroidOS10mUserAgentE");
    if (user_agent) {
        strncpy(user_agent, "GangstarRio/1.2.2b (PlayStation Vita)", 256);
    }

    /* Hook AndroidOS_GetEnv to directly return persistent &jni */
    uintptr_t android_os_get_env = runtime_symbol("AndroidOS_GetEnv");
    if (android_os_get_env) {
        runtime_patch_install_trampoline(android_os_get_env, (uintptr_t)&rio_AndroidOS_GetEnv);
        LOG_SUCCESS("M3: hooked AndroidOS_GetEnv -> %p", (void *)rio_AndroidOS_GetEnv);
    }

    uintptr_t android_release_thread_env = runtime_symbol("AndroidReleaseThreadEnv");
    if (android_release_thread_env) {
        runtime_patch_install_trampoline(android_release_thread_env, (uintptr_t)&rio_AndroidReleaseThreadEnv);
    }

    jclass installer_class = jni->FindClass(&jni,
        "com/gameloft/android/ANMP/GloftG4HM/installer/GameInstaller");
    rio_gl2_class = jni->FindClass(&jni,
        "com/gameloft/android/ANMP/GloftG4HM/GL2JNILib");
    if (!installer_class || !rio_gl2_class) {
        LOG_ERROR("M3 stopped: unable to create Java class references");
        diagnostics_flush();
        return 1;
    }
    java_configure_set_paths(set_paths, rio_gl2_class);

    LOG_INFO("M3: restoring GameInstaller native lifecycle");
    LOG_INFO("M3: calling GameInstaller.initNative");
    diagnostics_flush();
    installer_init(&jni, installer_class);
    LOG_SUCCESS("M3: GameInstaller.initNative returned");

    LOG_INFO("M3: calling GameInstaller.getPublicKey");
    diagnostics_flush();
    installer_public_key(&jni, installer_class);
    LOG_SUCCESS("M3: GameInstaller.getPublicKey returned");
    int **lock_pointer1 = (int **)runtime_symbol("lockPointer1");
    if (!lock_pointer1 || !*lock_pointer1) {
        LOG_ERROR("M3 stopped: GameInstaller did not create lockPointer1");
        diagnostics_flush();
        return 1;
    }

    /* Android's GDRMPolicy.nativeAllow() allocates this one-word gate after
     * Google Play licensing succeeds. The Vita wrapper has already verified
     * the exact supported clean library, so provide the same value locally. */
    int **lock_pointer3 = (int **)runtime_symbol("lockPointer3");
    if (!lock_pointer3) {
        diagnostics_flush();
        return 1;
    }
    *lock_pointer3 = &rio_start_gate;
    LOG_SUCCESS("M3: native start gate ready");

    LOG_INFO("M3: calling GameInstaller.nativeStart");
    diagnostics_flush();
    installer_start(&jni, installer_class);
    LOG_SUCCESS("M3: GameInstaller.nativeStart returned");
    int **lock_pointer4 = (int **)runtime_symbol("lockPointer4");
    if (!lock_pointer4 || !*lock_pointer4) {
        LOG_ERROR("M3 stopped: GameInstaller did not create lockPointer4");
        diagnostics_flush();
        return 1;
    }
    LOG_SUCCESS("M3: GameInstaller lifecycle ready");
    diagnostics_flush();

    LOG_INFO("M3: calling init");
    init(&jni, rio_gl2_class);
    LOG_SUCCESS("M3: init returned");

    LOG_INFO("M3: initializing VitaGL");
    diagnostics_flush();
    gl_init();
    LOG_SUCCESS("M3: VitaGL initialized");
    diagnostics_flush();

    /* SceAvPlayer allocates a controller thread that conflicts with this
     * game's OBB startup path on retail Vita. Keep the original artwork on
     * screen without starting a second video subsystem. */
    LOG_INFO("M3: displaying Gameloft loading screen");
    diagnostics_flush();
    mc2_show_loading_screen();
    gl_wait_for_startup_worker();
    LOG_SUCCESS("M3: loading screen displayed");
    diagnostics_flush();

    LOG_INFO("M3: calling initGL");
    init_gl(&jni, rio_gl2_class);
    LOG_SUCCESS("M3: initGL returned");

    LOG_INFO("M3: calling getViewSettings");
    get_view_settings(&jni, rio_gl2_class);
    LOG_SUCCESS("M3: getViewSettings returned");
    if (!java_paths_ready()) {
        LOG_ERROR("M3 stopped: getViewSettings did not call Java setupPaths");
        diagnostics_flush();
        return 1;
    }

    state_changed(&jni, rio_gl2_class, JNI_TRUE);
    LOG_SUCCESS("M3: stateChanged(true) returned");
    resize(&jni, rio_gl2_class, 960, 544);
    LOG_SUCCESS("M3: resize(960,544) returned");

    ensure_controller_connected();
    if (!rio_controller_connected)
        LOG_INFO("INPUT: PowerA mode pending until Application exists");
    rio_input_ready = 1;
    LOG_SUCCESS("M3: entering game loop with Vita input");
    diagnostics_flush();

    SceUID watchdog_thid = sceKernelCreateThread("rio_watchdog",
                                                 (SceKernelThreadEntry)watchdog_thread_func,
                                                 0x10000100, 0x10000, 0, 0, NULL);
    if (watchdog_thid >= 0) {
        sceKernelStartThread(watchdog_thid, 0, NULL);
    }

    diagnostics_render_start();
    unsigned frame = 0;
    for (;;) {
        uint64_t started = sceKernelGetProcessTimeWide();
        jni->PushLocalFrame(&jni, 32);
        ensure_controller_connected();
        controls_poll();
        calibrate_motion_on_world_entry();
        poll_motorcycle_motion();
        current_frame = frame;
        last_step_start = started;

        if (frame <= 5 || frame % 100 == 0) {
            LOG_INFO("M3: frame %u step begin", frame);
        }

        step(&jni, rio_gl2_class);
        jni->PopLocalFrame(&jni, NULL);

        uint64_t step_duration = sceKernelGetProcessTimeWide() - started;
        if (frame <= 5 || frame % 100 == 0) {
            LOG_INFO("M3: frame %u step end (took %llu ms), swapping",
                     frame, (unsigned long long)(step_duration / 1000));
        }

        uint64_t swap_started = sceKernelGetProcessTimeWide();
        gl_swap();
        uint64_t swap_duration = sceKernelGetProcessTimeWide() - swap_started;
        diagnostics_render_frame(frame, step_duration, swap_duration);
        ++frame;
        current_frame = frame;

        if (frame <= 8 || frame % 600 == 0) {
            LOG_INFO("M3 frame %u complete", frame);
        }

        uint64_t elapsed = sceKernelGetProcessTimeWide() - started;
        if (elapsed < 16667) {
            sceKernelDelayThread((unsigned int)(16667 - elapsed));
        }
    }
}
