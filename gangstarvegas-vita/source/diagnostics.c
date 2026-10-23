#include "diagnostics.h"
#include "game_profile.h"
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

static FILE *log_file;
static char log_buffer[64*1024];
static uint64_t last_flush_us;
static int watch_stopped=1;
static SceUID watched_thread;
static unsigned wait_sequence;
static const char *wait_operation;
static const void *wait_subject, *wait_caller;
static pthread_mutex_t log_lock=PTHREAD_MUTEX_INITIALIZER;
int diagnostics_init(void) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(GAME_DATA_PATH, 0777);
    log_file = fopen(GAME_LOG_PATH, "w");
    if (log_file && setvbuf(log_file,log_buffer,_IOFBF,sizeof(log_buffer))) {
        fclose(log_file);
        log_file=NULL;
    }
    last_flush_us=sceKernelGetSystemTimeWide();
    return log_file ? 0 : -1;
}
void diagnostics_log(const char *level, const char *format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    unsigned long long now = sceKernelGetSystemTimeWide();
    unsigned long long ms = now / 1000;
    sceClibPrintf("[%llu] %s: %s\n", ms, level, text);
    pthread_mutex_lock(&log_lock);
    if (log_file) {
        fprintf(log_file, "[%llu] %s: %s\n", ms, level, text);
        // Preserve error and milestone logs immediately; batch routine tracing.
        if (!strcmp(level,"ERROR") || !strcmp(level,"SUCCESS") || !strcmp(level,"ANDROID") ||
            !strncmp(text,"ALOG/6 ",7) || !strncmp(text,"ALOG/7 ",7) ||
            strstr(text,"ERROR") || strstr(text,"FATAL") ||
            sceKernelGetSystemTimeWide()-last_flush_us>=250000) {
            fflush(log_file);
            last_flush_us=sceKernelGetSystemTimeWide();
        }
    }
    pthread_mutex_unlock(&log_lock);
}
void diagnostics_flush(void) {
    pthread_mutex_lock(&log_lock);
    if (log_file) fflush(log_file);
    last_flush_us=sceKernelGetSystemTimeWide();
    pthread_mutex_unlock(&log_lock);
}
void diagnostics_close(void) {
    diagnostics_watch_stop();
    pthread_mutex_lock(&log_lock);
    if (log_file) fclose(log_file);
    log_file = NULL;
    pthread_mutex_unlock(&log_lock);
}

void port_trace(const char *format, ...) {
    char text[1024]; va_list arguments; va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments); va_end(arguments);
    diagnostics_log("COMPAT", "%s", text);
}
void port_trace_flush(void) { diagnostics_flush(); }

static void *runtime_watch(void *unused) {
    (void)unused;
    const void *previous_subject=NULL;
    unsigned long long previous_clocks=0;
    int stack_captured=0;
    for (unsigned sample=0;sample<30;++sample) {
        sceKernelDelayThread(10*1000*1000);
        if (__atomic_load_n(&watch_stopped,__ATOMIC_RELAXED)) break;
        SceKernelThreadInfo info={.size=sizeof(info)};
        int result=sceKernelGetThreadInfo(__atomic_load_n(&watched_thread,__ATOMIC_SEQ_CST),&info);
        if (result<0) { LOG_ERROR("Runtime watch: thread info failed 0x%x",result); break; }
        LOG_INFO("Runtime watch #%u: status=0x%x wait=0x%x wait_id=0x%x clocks=%llu stack_free=%d",
                 sample+1,info.status,info.waitType,info.waitId,(unsigned long long)info.runClocks,
                 sceKernelGetThreadStackFreeSize(__atomic_load_n(&watched_thread,__ATOMIC_SEQ_CST)));
        SceKernelSemaInfo semaphore={.size=sizeof(semaphore)};
        if (info.waitId>0 && sceKernelGetSemaInfo(info.waitId,&semaphore)==0)
            LOG_INFO("Runtime wait semaphore: name=%.31s count=%d waiters=%d",
                     semaphore.name,semaphore.currentCount,semaphore.numWaitThreads);
        unsigned sequence=__atomic_load_n(&wait_sequence,__ATOMIC_SEQ_CST);
        const char *operation=__atomic_load_n(&wait_operation,__ATOMIC_SEQ_CST);
        const void *subject=__atomic_load_n(&wait_subject,__ATOMIC_SEQ_CST);
        const void *caller=__atomic_load_n(&wait_caller,__ATOMIC_SEQ_CST);
        if (!(sequence & 1) && sequence==__atomic_load_n(&wait_sequence,__ATOMIC_SEQ_CST) && operation) {
            LOG_INFO("Runtime pending pthread: operation=%s subject=%p caller=%p",operation,subject,caller);
            uintptr_t base=(uintptr_t)info.stack, address=(uintptr_t)subject;
            size_t size=info.stackSize>0 ? (size_t)info.stackSize : 0;
            if (!stack_captured && !strcmp(operation,"cond_wait") && subject==previous_subject &&
                previous_clocks==(unsigned long long)info.runClocks && info.waitId>0 &&
                size && base<=UINTPTR_MAX-size && address>=base && address-base<size) {
                unsigned char bytes[2048];
                uintptr_t start=address-base>128 ? address-128 : base;
                size_t length=base+size-start;
                if (length>sizeof(bytes)) length=sizeof(bytes);
                stack_captured=1;
                /* Raw stack bytes identify candidates; this is not an unwind. */
                memcpy(bytes,(const void *)start,length);
                FILE *snapshot=fopen(GAME_DATA_PATH "/wait-stack.bin","wb");
                size_t written=snapshot ? fwrite(bytes,1,length,snapshot) : 0;
                int closed=snapshot ? fclose(snapshot) : -1;
                LOG_INFO("Runtime stack snapshot: start=0x%08lx length=%u written=%u close=%d stack=%p size=%u",
                         (unsigned long)start,(unsigned)length,(unsigned)written,closed,info.stack,(unsigned)size);
            }
            previous_subject=subject;
            previous_clocks=(unsigned long long)info.runClocks;
        } else previous_subject=NULL;
    }
    __atomic_store_n(&watch_stopped,1,__ATOMIC_RELAXED);
    LOG_INFO("Runtime watch stopped");
    return NULL;
}
void diagnostics_watch_start(void) {
    __atomic_store_n(&watched_thread,sceKernelGetThreadId(),__ATOMIC_SEQ_CST);
    pthread_attr_t attributes;
    int result=pthread_attr_init(&attributes);
    if (result) { LOG_ERROR("Runtime watch attributes failed: %d",result); return; }
    result=pthread_attr_setstacksize(&attributes,64*1024);
    if (!result) result=pthread_attr_setdetachstate(&attributes,PTHREAD_CREATE_DETACHED);
    if (!result) {
        pthread_t thread;
        __atomic_store_n(&watch_stopped,0,__ATOMIC_RELAXED);
        result=pthread_create(&thread,&attributes,runtime_watch,NULL);
        if (result) __atomic_store_n(&watch_stopped,1,__ATOMIC_RELAXED);
    }
    pthread_attr_destroy(&attributes);
    if (result) LOG_ERROR("Runtime watch start failed: %d",result);
}
void diagnostics_watch_stop(void) { __atomic_store_n(&watch_stopped,1,__ATOMIC_RELAXED); }

int diagnostics_wait_enter(const char *operation, const void *subject, const void *caller) {
    if (sceKernelGetThreadId()!=__atomic_load_n(&watched_thread,__ATOMIC_SEQ_CST)) return 0;
    __atomic_add_fetch(&wait_sequence,1,__ATOMIC_SEQ_CST);
    __atomic_store_n(&wait_operation,operation,__ATOMIC_SEQ_CST);
    __atomic_store_n(&wait_subject,subject,__ATOMIC_SEQ_CST);
    __atomic_store_n(&wait_caller,caller,__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&wait_sequence,1,__ATOMIC_SEQ_CST);
    return 1;
}
void diagnostics_wait_leave(int tracked) {
    if (!tracked) return;
    __atomic_add_fetch(&wait_sequence,1,__ATOMIC_SEQ_CST);
    __atomic_store_n(&wait_operation,NULL,__ATOMIC_SEQ_CST);
    __atomic_add_fetch(&wait_sequence,1,__ATOMIC_SEQ_CST);
}
