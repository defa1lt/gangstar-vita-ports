#include "diagnostics.h"

#include "game_profile.h"

#include <vitaGL.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#ifdef HAVE_SHARK_LOG
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>
#endif

#include <stdarg.h>
#include <stddef.h>
#include <string.h>

static SceUID log_fd = -1;
static SceUInt64 log_start_us;

/* Small sceIoWrite calls are exceptionally expensive on Vita storage.  The
 * import audit emits roughly 1500 timestamped lines before the first frame;
 * writing every line separately used to add 15-20 seconds to every launch.
 * Keep the timestamps, but batch the physical writes.  Explicit flush points
 * and the watchdog still make recent diagnostics durable around crashes. */
#define DIAGNOSTICS_BUFFER_SIZE (64 * 1024)
static char log_buffer[DIAGNOSTICS_BUFFER_SIZE];
static size_t log_buffer_used;
static volatile int log_lock;

static void diagnostics_lock(void) {
    while (__sync_lock_test_and_set(&log_lock, 1)) {
        sceKernelDelayThread(50);
    }
}

static void diagnostics_unlock(void) {
    __sync_lock_release(&log_lock);
}

static void diagnostics_write_buffer_locked(void) {
    if (log_fd < 0 || !log_buffer_used) return;

    size_t written = 0;
    while (written < log_buffer_used) {
        int result = sceIoWrite(log_fd, log_buffer + written,
                                (SceSize)(log_buffer_used - written));
        if (result <= 0) break;
        written += (size_t)result;
    }

    if (written < log_buffer_used) {
        size_t remaining = log_buffer_used - written;
        sceClibMemmove(log_buffer, log_buffer + written, remaining);
        log_buffer_used = remaining;
    } else {
        log_buffer_used = 0;
    }
}

int diagnostics_init(void) {
    log_start_us = sceKernelGetProcessTimeWide();
    log_buffer_used = 0;
    log_lock = 0;
    (void)sceIoMkdir(RIO_DATA_PATH, 0777);
    log_fd = sceIoOpen(RIO_LOG_PATH,
                       SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC,
                       0666);
    return log_fd < 0 ? -1 : 0;
}

void diagnostics_close(void) {
    diagnostics_lock();
    if (log_fd >= 0) {
        diagnostics_write_buffer_locked();
        (void)sceIoSyncByFd(log_fd, 0);
        sceIoClose(log_fd);
        log_fd = -1;
    }
    diagnostics_unlock();
}

void diagnostics_flush(void) {
    diagnostics_lock();
    if (log_fd >= 0) {
        diagnostics_write_buffer_locked();
        (void)sceIoSyncByFd(log_fd, 0);
    }
    diagnostics_unlock();
}

void diagnostics_log(const char *level, const char *format, ...) {
    char message[768];
    char line[832];
    va_list arguments;

    va_start(arguments, format);
    sceClibVsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    SceUInt64 now_us = sceKernelGetProcessTimeWide();
    SceUInt64 elapsed_us = now_us >= log_start_us
                         ? now_us - log_start_us
                         : 0;
    unsigned long long seconds = (unsigned long long)(elapsed_us / 1000000);
    unsigned long long milliseconds =
        (unsigned long long)((elapsed_us / 1000) % 1000);

    /* Monotonic time since diagnostics_init is more useful for diagnosing
     * stalls and thread races than Vita's user-configurable wall clock. */
    sceClibSnprintf(line, sizeof(line), "[+%06llu.%03llu] [%s] %s\n",
                    seconds, milliseconds, level, message);
    /* Preserve console output for failures without serializing every normal
     * import trace through stdout.  The complete timestamped trace remains
     * available in loader.log. */
    if (level && (strcmp(level, "ERROR") == 0 ||
                  strcmp(level, "FATAL") == 0 ||
                  strcmp(level, "CRASH") == 0)) {
        sceClibPrintf("%s", line);
    }

    if (log_fd >= 0) {
        size_t length = sceClibStrnlen(line, sizeof(line));
        diagnostics_lock();
        if (log_fd < 0) {
            diagnostics_unlock();
            return;
        }
        if (length > sizeof(log_buffer) - log_buffer_used) {
            diagnostics_write_buffer_locked();
        }
        if (length <= sizeof(log_buffer) - log_buffer_used) {
            sceClibMemcpy(log_buffer + log_buffer_used, line, length);
            log_buffer_used += length;
        } else {
            (void)sceIoWrite(log_fd, line, (SceSize)length);
        }
        diagnostics_unlock();
    }
}

/* Link wrappers keep allocator behavior intact and include time spent in the
 * existing forced-GC retry path. Counters wrap modulo 2^32; interval deltas
 * remain valid without requiring 64-bit atomics on ARMv7. */
enum RenderCounter {
    POOL_CALLS, POOL_BYTES, POOL_US, POOL_ACTIVE,
    MAPPED_CALLS, MAPPED_BYTES, MAPPED_US, MAPPED_ACTIVE,
    MAPPED_SLOW, MAPPED_FAILED, RENDER_COUNTER_COUNT
};
static unsigned render_counters[RENDER_COUNTER_COUNT];
static unsigned render_previous[RENDER_COUNTER_COUNT];
static uint64_t render_report_started;
static uint64_t render_step_us;
static uint64_t render_swap_us;
static uint64_t render_max_step_us;
static unsigned render_frames;

static void render_add(enum RenderCounter counter, unsigned value) {
    __atomic_fetch_add(&render_counters[counter], value, __ATOMIC_RELAXED);
}

static void render_snapshot(unsigned *sample) {
    for (unsigned i = 0; i < RENDER_COUNTER_COUNT; ++i)
        sample[i] = __atomic_load_n(&render_counters[i], __ATOMIC_RELAXED);
}

void __wrap_vgl_file_log(const char *format, ...) {
    char message[768];
    va_list args;
    va_start(args, format);
    sceClibVsnprintf(message, sizeof(message), format, args);
    va_end(args);
    size_t length = sceClibStrnlen(message, sizeof(message));
    while (length && (message[length - 1] == '\n' || message[length - 1] == '\r'))
        message[--length] = '\0';
    diagnostics_log("VGL", "%s", message);
}

uint8_t *__real_vgl_reserve_data_pool(uint32_t size);
uint8_t *__wrap_vgl_reserve_data_pool(uint32_t size) {
    uint64_t started = sceKernelGetProcessTimeWide();
    render_add(POOL_CALLS, 1);
    render_add(POOL_BYTES, size);
    render_add(POOL_ACTIVE, 1);
    uint8_t *result = __real_vgl_reserve_data_pool(size);
    render_add(POOL_US, (unsigned)(sceKernelGetProcessTimeWide() - started));
    render_add(POOL_ACTIVE, (unsigned)-1);
    return result;
}

void *__real_gpu_alloc_mapped_aligned_for_cpu(size_t alignment, size_t size);
void *__real_gpu_alloc_mapped_aligned_for_gpu(size_t alignment, size_t size);

static void *render_mapped_alloc(size_t alignment, size_t size,
                                void *(*allocate)(size_t, size_t)) {
    uint64_t started = sceKernelGetProcessTimeWide();
    render_add(MAPPED_CALLS, 1);
    render_add(MAPPED_BYTES, (unsigned)size);
    render_add(MAPPED_ACTIVE, 1);
    void *result = allocate(alignment, size);
    uint64_t elapsed = sceKernelGetProcessTimeWide() - started;
    render_add(MAPPED_US, (unsigned)elapsed);
    if (elapsed >= 500000) render_add(MAPPED_SLOW, 1);
    if (!result) render_add(MAPPED_FAILED, 1);
    render_add(MAPPED_ACTIVE, (unsigned)-1);
    return result;
}

void *__wrap_gpu_alloc_mapped_aligned_for_cpu(size_t alignment, size_t size) {
    return render_mapped_alloc(alignment, size,
                               __real_gpu_alloc_mapped_aligned_for_cpu);
}

void *__wrap_gpu_alloc_mapped_aligned_for_gpu(size_t alignment, size_t size) {
    return render_mapped_alloc(alignment, size,
                               __real_gpu_alloc_mapped_aligned_for_gpu);
}

void diagnostics_render_start(void) {
    render_snapshot(render_previous);
    render_report_started = sceKernelGetProcessTimeWide();
    render_frames = 0;
    render_step_us = render_swap_us = render_max_step_us = 0;
    LOG_INFO("PROFILE: " RIO_VERSION " render diagnostics; allocator times include GC; "
             "pool bytes are requested temporary data; mapped counters cover "
             "cross-object calls including pool spills, excluding internal "
             "texture/USSE calls; recovery messages cover those too; "
             "timings overlap and must not be added");
}

void diagnostics_render_frame(unsigned frame, uint64_t step_us, uint64_t swap_us) {
    ++render_frames;
    render_step_us += step_us;
    render_swap_us += swap_us;
    if (step_us > render_max_step_us) render_max_step_us = step_us;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - render_report_started < 2000000 && step_us < 1000000 && swap_us < 1000000)
        return;

    unsigned sample[RENDER_COUNTER_COUNT], delta[RENDER_COUNTER_COUNT];
    render_snapshot(sample);
    for (unsigned i = 0; i < RENDER_COUNTER_COUNT; ++i) {
        delta[i] = sample[i] - render_previous[i];
        render_previous[i] = sample[i];
    }
    diagnostics_log("PROFILE",
                    "frame=%u frames=%u window=%llu ms step=%llu ms "
                    "max_step=%llu ms swap=%llu ms "
                    "pool(calls=%u requested=%u KB elapsed=%u ms) "
                    "mapped(calls=%u requested=%u KB elapsed=%u ms "
                    "slow=%u failed=%u) RAM=%u KB VRAM=%u KB",
                    frame, render_frames,
                    (unsigned long long)((now - render_report_started) / 1000),
                    (unsigned long long)(render_step_us / 1000),
                    (unsigned long long)(render_max_step_us / 1000),
                    (unsigned long long)(render_swap_us / 1000),
                    delta[POOL_CALLS], delta[POOL_BYTES] / 1024,
                    delta[POOL_US] / 1000, delta[MAPPED_CALLS],
                    delta[MAPPED_BYTES] / 1024, delta[MAPPED_US] / 1000,
                    delta[MAPPED_SLOW], delta[MAPPED_FAILED],
                    (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024),
                    (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024));
#ifdef HAVE_SHARK_LOG
    diagnostics_gpu_progress(8);
#endif
    render_report_started = now;
    render_frames = 0;
    render_step_us = render_swap_us = render_max_step_us = 0;
}

void diagnostics_render_progress(void) {
    unsigned sample[RENDER_COUNTER_COUNT];
    render_snapshot(sample);
    diagnostics_log("PROFILE",
                    "active(pool=%u mapped=%u) totals(pool_calls=%u "
                    "pool_requested=%u KB mapped_calls=%u "
                    "mapped_requested=%u KB mapped_elapsed=%u ms "
                    "mapped_slow=%u mapped_failed=%u); pending calls "
                    "contribute bytes now and elapsed time after return",
                    sample[POOL_ACTIVE], sample[MAPPED_ACTIVE],
                    sample[POOL_CALLS], sample[POOL_BYTES] / 1024,
                    sample[MAPPED_CALLS], sample[MAPPED_BYTES] / 1024,
                    sample[MAPPED_US] / 1000, sample[MAPPED_SLOW],
                    sample[MAPPED_FAILED]);
}

#ifdef HAVE_SHARK_LOG
/* Keep GPU submission evidence in memory; the existing watchdog copies a
 * bounded tail. Recording never waits, allocates or changes GPU ordering. */
enum GpuTraceKind { GPU_DRAW, GPU_BEGIN, GPU_END, GPU_QUEUE,
                    GPU_VERTEX_PROGRAM, GPU_FRAGMENT_PROGRAM, GPU_STREAM };
typedef struct {
    unsigned busy, sequence, kind, returned, context, thread, caller, args[5];
} GpuTrace;
#define GPU_TRACE_COUNT 128u
static GpuTrace gpu_trace[GPU_TRACE_COUNT];
static unsigned gpu_trace_sequence;
static __thread uintptr_t gpu_scene_caller;

static void trace_gpu(unsigned kind, unsigned returned, const void *context,
                      uintptr_t a, uintptr_t b, uintptr_t c,
                      uintptr_t d, uintptr_t e) {
    unsigned sequence = __atomic_add_fetch(&gpu_trace_sequence, 1, __ATOMIC_RELAXED);
    GpuTrace *entry = &gpu_trace[sequence % GPU_TRACE_COUNT];
    if (__atomic_exchange_n(&entry->busy, 1, __ATOMIC_ACQUIRE)) return;
    entry->sequence = sequence;
    entry->kind = kind;
    entry->returned = returned;
    entry->context = (unsigned)(uintptr_t)context;
    entry->thread = (kind == GPU_BEGIN || kind == GPU_END) ? (unsigned)sceKernelGetThreadId() : 0;
    entry->caller = (kind == GPU_BEGIN || kind == GPU_END) ? (unsigned)gpu_scene_caller : 0;
    entry->args[0] = (unsigned)a; entry->args[1] = (unsigned)b;
    entry->args[2] = (unsigned)c; entry->args[3] = (unsigned)d;
    entry->args[4] = (unsigned)e;
    __atomic_store_n(&entry->busy, 0, __ATOMIC_RELEASE);
}

void diagnostics_gpu_progress(unsigned limit) {
    unsigned end = __atomic_load_n(&gpu_trace_sequence, __ATOMIC_RELAXED);
    if (limit > GPU_TRACE_COUNT) limit = GPU_TRACE_COUNT;
    unsigned count = end < limit ? end : limit;
    static const char *const names[] = {
        "draw", "begin", "end", "queue", "vertex", "fragment", "stream"
    };
    for (unsigned sequence = end - count + 1; count; ++sequence, --count) {
        GpuTrace *entry = &gpu_trace[sequence % GPU_TRACE_COUNT];
        if (__atomic_exchange_n(&entry->busy, 1, __ATOMIC_ACQUIRE)) continue;
        GpuTrace copy = {0};
        copy.sequence = entry->sequence; copy.kind = entry->kind;
        copy.returned = entry->returned; copy.context = entry->context;
        copy.thread = entry->thread; copy.caller = entry->caller;
        for (unsigned i = 0; i < 5; ++i) copy.args[i] = entry->args[i];
        __atomic_store_n(&entry->busy, 0, __ATOMIC_RELEASE);
        if (copy.sequence != sequence || copy.kind >= sizeof(names) / sizeof(names[0])) continue;
        diagnostics_log("GPU", "seq=%u %s %s ctx=0x%08x tid=0x%08x caller=0x%08x args=%08x/%08x/%08x/%08x/%08x",
                        sequence, names[copy.kind], copy.returned ? "returned" : "enter",
                        copy.context, copy.thread, copy.caller, copy.args[0], copy.args[1], copy.args[2],
                        copy.args[3], copy.args[4]);
    }
}

int __real_sceGxmDraw(SceGxmContext *, SceGxmPrimitiveType, SceGxmIndexFormat,
                      const void *, unsigned);
int __wrap_sceGxmDraw(SceGxmContext *context, SceGxmPrimitiveType primitive,
                      SceGxmIndexFormat format, const void *indices, unsigned count) {
    trace_gpu(GPU_DRAW, 0, context, primitive, format, (uintptr_t)indices, count, 0);
    int result = __real_sceGxmDraw(context, primitive, format, indices, count);
    trace_gpu(GPU_DRAW, 1, context, primitive, format, (uintptr_t)indices, count, (unsigned)result);
    return result;
}

int __real_sceGxmBeginScene(SceGxmContext *, unsigned, const SceGxmRenderTarget *,
    const SceGxmValidRegion *, SceGxmSyncObject *, SceGxmSyncObject *,
    const SceGxmColorSurface *, const SceGxmDepthStencilSurface *);
int __wrap_sceGxmBeginScene(SceGxmContext *context, unsigned flags,
    const SceGxmRenderTarget *target, const SceGxmValidRegion *region,
    SceGxmSyncObject *vertex_sync, SceGxmSyncObject *fragment_sync,
    const SceGxmColorSurface *color, const SceGxmDepthStencilSurface *depth) {
    uintptr_t previous = gpu_scene_caller;
    gpu_scene_caller = (uintptr_t)__builtin_return_address(0);
    trace_gpu(GPU_BEGIN, 0, context, (uintptr_t)target, (uintptr_t)color,
              (uintptr_t)depth, flags, 0);
    int result = __real_sceGxmBeginScene(context, flags, target, region,
                                        vertex_sync, fragment_sync, color, depth);
    trace_gpu(GPU_BEGIN, 1, context, (uintptr_t)target, (uintptr_t)color,
              (uintptr_t)depth, flags, (unsigned)result);
    gpu_scene_caller = previous;
    return result;
}

int __real_sceGxmEndScene(SceGxmContext *, const SceGxmNotification *, const SceGxmNotification *);
int __wrap_sceGxmEndScene(SceGxmContext *context, const SceGxmNotification *vertex,
                         const SceGxmNotification *fragment) {
    uintptr_t previous = gpu_scene_caller;
    gpu_scene_caller = (uintptr_t)__builtin_return_address(0);
    trace_gpu(GPU_END, 0, context, (uintptr_t)vertex, (uintptr_t)fragment, 0, 0, 0);
    int result = __real_sceGxmEndScene(context, vertex, fragment);
    trace_gpu(GPU_END, 1, context, (uintptr_t)vertex, (uintptr_t)fragment, 0, 0, (unsigned)result);
    gpu_scene_caller = previous;
    return result;
}

int __real_sceGxmDisplayQueueAddEntry(SceGxmSyncObject *, SceGxmSyncObject *, const void *);
int __wrap_sceGxmDisplayQueueAddEntry(SceGxmSyncObject *old_buffer,
                                    SceGxmSyncObject *new_buffer, const void *callback) {
    trace_gpu(GPU_QUEUE, 0, NULL, (uintptr_t)old_buffer, (uintptr_t)new_buffer,
              (uintptr_t)callback, 0, 0);
    int result = __real_sceGxmDisplayQueueAddEntry(old_buffer, new_buffer, callback);
    trace_gpu(GPU_QUEUE, 1, NULL, (uintptr_t)old_buffer, (uintptr_t)new_buffer,
              (uintptr_t)callback, 0, (unsigned)result);
    return result;
}

void __real_sceGxmSetVertexProgram(SceGxmContext *, const SceGxmVertexProgram *);
void __wrap_sceGxmSetVertexProgram(SceGxmContext *context, const SceGxmVertexProgram *program) {
    __real_sceGxmSetVertexProgram(context, program);
    trace_gpu(GPU_VERTEX_PROGRAM, 1, context, (uintptr_t)program, 0, 0, 0, 0);
}

void __real_sceGxmSetFragmentProgram(SceGxmContext *, const SceGxmFragmentProgram *);
void __wrap_sceGxmSetFragmentProgram(SceGxmContext *context, const SceGxmFragmentProgram *program) {
    __real_sceGxmSetFragmentProgram(context, program);
    trace_gpu(GPU_FRAGMENT_PROGRAM, 1, context, (uintptr_t)program, 0, 0, 0, 0);
}

int __real_sceGxmSetVertexStream(SceGxmContext *, unsigned, const void *);
int __wrap_sceGxmSetVertexStream(SceGxmContext *context, unsigned stream, const void *data) {
    int result = __real_sceGxmSetVertexStream(context, stream, data);
    trace_gpu(GPU_STREAM, 1, context, stream, (uintptr_t)data, 0, 0, (unsigned)result);
    return result;
}
#endif

#ifdef HAVE_SHARK_LOG
static tai_hook_ref_t compiler_lock_ref;
static SceUID compiler_lock_hook = -1;
static unsigned compiler_lock_calls, compiler_lock_failures;
static int compiler_lock_error;
static uintptr_t compiler_lock_work;

static int compiler_lock_hook_func(SceKernelLwMutexWork *work, int count, unsigned *timeout) {
    int result = TAI_CONTINUE(int, compiler_lock_ref, work, count, timeout);
    __sync_add_and_fetch(&compiler_lock_calls, 1);
    if (result != 0) {
        __sync_add_and_fetch(&compiler_lock_failures, 1);
        __atomic_store_n(&compiler_lock_error, result, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_lock_work, (uintptr_t)work, __ATOMIC_RELAXED);
    }
    return result;
}

static SceUID compiler_module_info(SceKernelModuleInfo *info, const char *phase) {
    SceUID modules[256];
    SceSize count = 256;
    if (sceKernelGetModuleList(0xff, modules, &count) < 0) return -1;
    for (SceSize i = 0; i < count && i < 256; ++i) {
        memset(info, 0, sizeof(*info));
        info->size = sizeof(*info);
        if (sceKernelGetModuleInfo(modules[i], info) < 0 ||
            strcmp(info->module_name, "SceShaccCg")) continue;
        /* Offsets belong to the two privately inspected compiler binaries. */
        if (info->segments[0].memsz != 0x2fe954 ||
            info->segments[1].memsz != 0x328) {
            LOG_INFO("COMPILER STATE: phase=%s unsupported module layout text=%u data=%u",
                     phase, info->segments[0].memsz, info->segments[1].memsz);
            return -1;
        }
        return modules[i];
    }
    LOG_INFO("COMPILER STATE: phase=%s module unavailable", phase);
    return -1;
}

static void compiler_state_snapshot(const char *phase) {
    SceKernelModuleInfo info;
    if (compiler_module_info(&info, phase) < 0) return;
    const uint8_t *data = info.segments[1].vaddr;
    uint32_t lock[8], tls[8], keys[4], allocators[2], self;
    memcpy(lock, data + 0x2e0, sizeof(lock));
    memcpy(allocators, data + 0x294, sizeof(allocators));
    memcpy(&self, data + 0x308, sizeof(self));
    const unsigned offsets[] = {0x220, 0x234, 0x2d0, 0x318};
    for (unsigned k = 0; k < 4; ++k) memcpy(&keys[k], data + offsets[k], 4);
    for (unsigned k = 0; k < 8; ++k) {
        const void *slot = sceKernelGetTLSAddr(69 + (int)k);
        tls[k] = 0;
        if (slot) memcpy(&tls[k], slot, 4);
    }
    LOG_INFO("COMPILER STATE: phase=%s data=%p keys=%u/%u/%u/%u alloc=0x%x/0x%x lock_self=0x%x lock=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x",
             phase, data, keys[0], keys[1], keys[2], keys[3], allocators[0], allocators[1], self,
             lock[0], lock[1], lock[2], lock[3], lock[4], lock[5], lock[6], lock[7]);
    LOG_INFO("COMPILER TLS: phase=%s thread=0x%x slots69-76=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x",
             phase, (unsigned)sceKernelGetThreadId(), tls[0], tls[1], tls[2], tls[3], tls[4], tls[5], tls[6], tls[7]);
}

static tai_hook_ref_t compiler_create_ref;
static SceUID compiler_create_hook = -1;
static unsigned compiler_create_calls, compiler_create_failures;
static int compiler_create_error, compiler_create_count;
static unsigned compiler_create_attr;
static uintptr_t compiler_create_work, compiler_create_options;

static int compiler_create_hook_func(SceKernelLwMutexWork *work, const char *name,
                                     unsigned attr, int count,
                                     const SceKernelLwMutexOptParam *options) {
    int result = TAI_CONTINUE(int, compiler_create_ref, work, name, attr, count, options);
    __sync_add_and_fetch(&compiler_create_calls, 1);
    if (result != 0) {
        __sync_add_and_fetch(&compiler_create_failures, 1);
        __atomic_store_n(&compiler_create_error, result, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_work, (uintptr_t)work, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_attr, attr, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_count, count, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_options, (uintptr_t)options, __ATOMIC_RELAXED);
    }
    return result;
}

void diagnostics_compiler_trace_begin(void) {
    if (compiler_lock_hook < 0)
        compiler_lock_hook = taiHookFunctionImport(&compiler_lock_ref, "SceShaccCg",
            TAI_ANY_LIBRARY, 0x46e7be7b, compiler_lock_hook_func);
    LOG_INFO("COMPILER LOCK: import hook result=0x%x", compiler_lock_hook);
    SceKernelModuleInfo info;
    if (compiler_module_info(&info, "trace begin") >= 0 && compiler_create_hook < 0)
        compiler_create_hook = taiHookFunctionImport(&compiler_create_ref, "SceShaccCg",
            TAI_ANY_LIBRARY, 0xda6ec8ef, compiler_create_hook_func);
    LOG_INFO("COMPILER CREATE: import hook result=0x%x", compiler_create_hook);
}

void diagnostics_compiler_trace_end(void) {
    int create_result = compiler_create_hook >= 0
        ? taiHookRelease(compiler_create_hook, compiler_create_ref) : 0;
    LOG_INFO("COMPILER CREATE: release result=0x%x", create_result);
    if (create_result >= 0) compiler_create_hook = -1;
    int result = compiler_lock_hook >= 0
        ? taiHookRelease(compiler_lock_hook, compiler_lock_ref) : 0;
    LOG_INFO("COMPILER LOCK: release result=0x%x", result);
    if (result >= 0) compiler_lock_hook = -1;
}

void diagnostics_compiler_checkpoint(const char *phase, int completed) {
    if (completed) {
        LOG_INFO("COMPILER CREATE: phase=%s calls=%u failures=%u last_error=0x%x work=0x%x attr=0x%x count=%d options=0x%x",
                 phase, __atomic_load_n(&compiler_create_calls, __ATOMIC_RELAXED),
                 __atomic_load_n(&compiler_create_failures, __ATOMIC_RELAXED),
                 __atomic_load_n(&compiler_create_error, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&compiler_create_work, __ATOMIC_RELAXED),
                 __atomic_load_n(&compiler_create_attr, __ATOMIC_RELAXED),
                 __atomic_load_n(&compiler_create_count, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&compiler_create_options, __ATOMIC_RELAXED));
        LOG_INFO("COMPILER LOCK: phase=%s calls=%u failures=%u last_error=0x%x work=0x%x",
                 phase, __atomic_load_n(&compiler_lock_calls, __ATOMIC_RELAXED),
                 __atomic_load_n(&compiler_lock_failures, __ATOMIC_RELAXED),
                 __atomic_load_n(&compiler_lock_error, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&compiler_lock_work, __ATOMIC_RELAXED));
    } else {
        compiler_state_snapshot(phase);
        __atomic_store_n(&compiler_create_calls, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_failures, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_error, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_work, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_attr, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_create_options, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_lock_calls, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_lock_failures, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_lock_error, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compiler_lock_work, 0, __ATOMIC_RELAXED);
    }
}
#endif
