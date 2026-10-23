#include "runtime.h"
#include "offline.h"
#include "diagnostics.h"
#include "game_profile.h"
#include "reimpl/pthr.h"
#include "reimpl/egl.h"
#include "utils/glutil.h"
#include <kubridge.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/times.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <psp2/io/devctl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <zlib.h>

extern FILE __sF_fake[3];
static void kernel_memory_barrier(void) __attribute__((naked,target("arm")));
static void kernel_memory_barrier(void) {
    __asm__ volatile("dmb ish\n\tbx lr");
}
static int kernel_compare_exchange(uint32_t old __attribute__((unused)),
                                   uint32_t replacement __attribute__((unused)),
                                   volatile uint32_t *target __attribute__((unused)))
    __attribute__((naked,target("arm")));
static int kernel_compare_exchange(uint32_t old __attribute__((unused)),
                                   uint32_t replacement __attribute__((unused)),
                                   volatile uint32_t *target __attribute__((unused))) {
    // Linux kuser ABI preserves r1/r2 and reports success in both r0 and C.
    __asm__ volatile("dmb ish\n"
                     "1: ldrex r3, [r2]\n"
                     "cmp r3, r0\n"
                     "bne 2f\n"
                     "strex r3, r1, [r2]\n"
                     "cmp r3, #0\n"
                     "bne 1b\n"
                     "mov r0, #0\n"
                     "b 3f\n"
                     "2: clrex\n"
                     "mov r0, #1\n"
                     "3: dmb ish\n"
                     "rsbs r0, r0, #0\n"
                     "bx lr");
}
uintptr_t vegas_kernel_helper(uintptr_t address) {
    if (address==0xffff0fa0u) return (uintptr_t)kernel_memory_barrier;
    if (address==0xffff0fc0u) return (uintptr_t)kernel_compare_exchange;
    return 0;
}
static uintptr_t policy_constructor_tail __attribute__((used));
static void *(*policy_destructor)(void *);
static void *(*policy_deleting_destructor)(void *);
static void (*policy_release)(void *);
static void *native_policy_selector(void *self __attribute__((unused)),
                                    void *manager __attribute__((unused)))
    __attribute__((naked,target("arm")));
static void *native_policy_selector(void *self __attribute__((unused)),
                                    void *manager __attribute__((unused))) {
    __asm__ volatile("push {r4, r5, r6, lr}\n"
                     "mov r4, r0\n"
                     "ldr ip, =policy_constructor_tail\n"
                     "ldr pc, [ip]");
}
static void *policy_selector(void *self, void *manager) {
    void *result=native_policy_selector(self,manager);
    uint32_t *policy=((void **)self)[2];
    if (!policy || policy[0]!=so_mod.text_base+GAME_TEXTURE_POLICY_POLICY_VTABLE) {
        LOG_ERROR("Custom texture policy constructor returned an invalid object");
        abort();
    }
    // The selector must outlive the last texture which references its policy.
    unsigned references=__atomic_add_fetch(&policy[1],1,__ATOMIC_RELAXED);
    LOG_INFO("Custom texture policy retained: selector=%p policy=%p references=%u",self,policy,references);
    return result;
}
static void release_selector_policy(void *self) {
    void *policy=((void **)self)[2];
    ((void **)self)[2]=NULL;
    if (policy) policy_release(policy);
}
static void *destroy_policy_selector(void *self) {
    release_selector_policy(self);
    return policy_destructor(self);
}
static void *delete_policy_selector(void *self) {
    release_selector_policy(self);
    return policy_deleting_destructor(self);
}
int vegas_bind_texture_policy(void) {
    uint32_t *entry=(void *)(so_mod.text_base+GAME_TEXTURE_POLICY_CONSTRUCTOR);
    uint32_t *vtable=(void *)(so_mod.text_base+GAME_TEXTURE_POLICY_SELECTOR_VTABLE);
    if (entry[0]!=GAME_TEXTURE_POLICY_CONSTRUCTOR_WORD0 ||
        entry[1]!=GAME_TEXTURE_POLICY_CONSTRUCTOR_WORD1 ||
        vtable[0]!=so_mod.text_base+GAME_TEXTURE_POLICY_DESTRUCTOR ||
        vtable[1]!=so_mod.text_base+GAME_TEXTURE_POLICY_DELETING_DESTRUCTOR) {
        LOG_ERROR("Custom texture policy profile mismatch"); return -1;
    }
    policy_constructor_tail=(uintptr_t)entry+8;
    policy_destructor=(void *)vtable[0];
    policy_deleting_destructor=(void *)vtable[1];
    policy_release=(void *)(so_mod.text_base+GAME_TEXTURE_POLICY_RELEASE);
    so_hook hook=hook_arm((uintptr_t)entry,(uintptr_t)policy_selector);
    uint32_t replacements[2]={(uintptr_t)destroy_policy_selector,(uintptr_t)delete_policy_selector};
    if (memcmp(entry,hook.patch_instr,sizeof(hook.patch_instr)) ||
        kuKernelCpuUnrestrictedMemcpy(vtable,replacements,sizeof(replacements))<0 ||
        memcmp(vtable,replacements,sizeof(replacements))) {
        LOG_ERROR("Custom texture policy binding failed"); return -1;
    }
    LOG_SUCCESS("Custom texture policy selector owns its policy");
    return 0;
}
extern uintptr_t __gnu_Unwind_Find_exidx(uintptr_t,int *);
static mode_t file_mask = 022;
static mode_t app_umask(mode_t mask) { return __atomic_exchange_n(&file_mask,mask & 0777,__ATOMIC_RELAXED); }
static void *app_dlsym(void *handle,const char *name) { (void)handle; return vegas_symbol(name); }
extern void *dlsym_soloader(void *, const char *);
extern int vegas_msync(void *, size_t, int);
extern int vegas_epoll_create(int);
extern int vegas_epoll_ctl(int, int, int, void *);
extern int vegas_epoll_wait(int, void *, int, int);
extern int vegas_close(int);
static void android_assert(const char *file,int line,const char *function,const char *message) __attribute__((noreturn));
static void android_assert(const char *file,int line,const char *function,const char *message) {
    LOG_ERROR("Android assertion: %s:%d %s: %s",file,line,function,message);
    diagnostics_flush(); abort();
}
extern ssize_t vegas_read(int, void *, size_t);
extern ssize_t vegas_write(int, const void *, size_t);

void *sceClibMemclr(void *destination, size_t length) { return sceClibMemset(destination, 0, length); }

static int classify(double value) {
    union { double floating; uint64_t bits; } number = {value};
    uint64_t exponent = number.bits >> 52 & 0x7ff;
    uint64_t fraction = number.bits & UINT64_C(0xfffffffffffff);
    if (exponent == 0x7ff) return fraction ? 2 : 1;
    if (!exponent) return fraction ? 8 : 16;
    return 4;
}
static int detach_state(const pthread_attr_t_bionic *attribute, int *state) {
    if (!attribute || !state || attribute->magic != 0x42424242 || !attribute->real_ptr) return EINVAL;
    int native;
    int result = pthread_attr_getdetachstate(attribute->real_ptr, &native);
    if (!result) *state = native == PTHREAD_CREATE_DETACHED;
    return result;
}
static int tid(void) { return sceKernelGetThreadId(); }
static unsigned app_uid(void) { return 10000; }
typedef struct { char *name, *password; unsigned uid, gid; char *home, *shell; } AndroidPasswd;
static AndroidPasswd *app_passwd(unsigned uid) {
    static AndroidPasswd entry = {"vita", "x", 10000, 10000, "ux0:data/gangstarvegas/files", ""};
    if (uid != entry.uid) { errno = ENOENT; return NULL; }
    return &entry;
}
static void android_syslog(int priority, const char *format, ...) {
    char buffer[768]; va_list arguments; va_start(arguments, format);
    vsnprintf(buffer, sizeof(buffer), format, arguments); va_end(arguments);
    diagnostics_log("ANDROID", "priority=%d %s", priority, buffer);
}
typedef struct {
    uint32_t type, block_size;
    uint64_t blocks, free, available, files, free_files;
    int32_t fsid[2];
    uint32_t name_length, fragment_size, flags, spare[4];
} AndroidStatfs;
_Static_assert(sizeof(AndroidStatfs) == 88 && offsetof(AndroidStatfs,available) == 24,
               "Bionic ARM statfs64 layout");
static int android_statfs(const char *path, AndroidStatfs *out) {
    if (!path || !out) { errno = EFAULT; return -1; }
    SceIoDevInfo storage = {0};
    int result = sceIoDevctl("ux0:", 0x3001, NULL, 0, &storage, sizeof(storage));
    if (result < 0) { errno = EIO; return -1; }
    memset(out, 0, sizeof(*out)); out->type = 0x4d44; out->block_size = out->fragment_size = 4096;
    out->blocks = storage.max_size / 4096; out->free = out->available = storage.free_size / 4096;
    out->name_length = 255; return 0;
}
typedef struct { const void *base; size_t length; } AndroidIovec;
static ssize_t vector_write(int fd, const AndroidIovec *vector, int count) {
    if (count < 0 || count > 1024) { errno = EINVAL; return -1; }
    ssize_t total = 0;
    for (int i = 0; i < count; ++i) {
        if (vector[i].length > INT32_MAX - (size_t)total) { errno = EINVAL; return total ? total : -1; }
        ssize_t n = vegas_write(fd, vector[i].base, vector[i].length);
        if (n < 0) return total ? total : -1;
        total += n;
        if ((size_t)n < vector[i].length) break;
    }
    return total;
}
static ssize_t copy_file(int out, int in, int32_t *offset, size_t count) {
    unsigned char buffer[32768]; size_t total = 0;
    while (total < count) {
        size_t chunk = count - total; if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        ssize_t n = offset ? pread(in, buffer, chunk, *offset) : vegas_read(in, buffer, chunk);
        if (n < 0) return total ? (ssize_t)total : -1;
        if (!n) break;
        ssize_t done = vegas_write(out, buffer, n);
        if (done < 0) return total ? (ssize_t)total : -1;
        total += done;
        if (offset) *offset += done;
        else if (done < n) lseek(in, done - n, SEEK_CUR);
        if (done < n) break;
    }
    return total;
}
typedef struct { const char *file; void *base; const char *symbol; void *address; } AndroidDlInfo;
static int address_info(const void *address, AndroidDlInfo *out) {
    if (!out) return 0;
    uintptr_t value = (uintptr_t)address;
    if (value < so_mod.text_base || value >= so_mod.text_base + so_mod.text_size) return 0;
    *out = (AndroidDlInfo){"ux0:data/gangstarvegas/libGangstar4.so", (void *)so_mod.text_base, NULL, NULL};
    uintptr_t nearest = 0;
    for (int i = 0; i < so_mod.num_dynsym; ++i) {
        Elf32_Sym *symbol = &so_mod.dynsym[i];
        uintptr_t candidate = so_mod.text_base + symbol->st_value;
        if (symbol->st_shndx && symbol->st_name && candidate <= value && candidate >= nearest) {
            nearest = candidate; out->symbol = so_mod.dynstr + symbol->st_name; out->address = (void *)candidate;
        }
    }
    return 1;
}
extern EGLDisplay bbr_eglGetCurrentDisplay(void);
extern EGLSurface bbr_eglGetCurrentSurface(EGLint);
static EGLDisplay current_display(void) { return bbr_eglGetCurrentDisplay(); }
static EGLSurface current_surface(EGLint which) { return bbr_eglGetCurrentSurface(which); }
static unsigned presentations;
unsigned vegas_presentations(void) { return __atomic_load_n(&presentations,__ATOMIC_RELAXED); }
static EGLBoolean present(EGLDisplay display, EGLSurface surface) {
    (void)display; (void)surface;
    gl_swap();
    unsigned count=__atomic_add_fetch(&presentations,1,__ATOMIC_RELAXED);
    if (count<=8 || count%300==0)
        LOG_INFO("Native presentation=%u thread=0x%x",count,sceKernelGetThreadId());
    return EGL_TRUE;
}
static int tracked_mutex_lock(pthread_mutex_t_bionic *mutex) {
    int tracked=diagnostics_wait_enter("mutex_lock",mutex,__builtin_return_address(0));
    int result=pthread_mutex_lock_soloader(mutex);
    diagnostics_wait_leave(tracked);
    return result;
}
static int tracked_condition_wait(pthread_cond_t_bionic *condition, pthread_mutex_t_bionic *mutex) {
    int tracked=diagnostics_wait_enter("cond_wait",condition,__builtin_return_address(0));
    int result=pthread_cond_wait_soloader(condition,mutex);
    diagnostics_wait_leave(tracked);
    return result;
}
static int tracked_condition_timedwait(pthread_cond_t_bionic *condition, pthread_mutex_t_bionic *mutex,
                                       struct timespec *deadline) {
    int tracked=diagnostics_wait_enter("cond_timedwait",condition,__builtin_return_address(0));
    int result=pthread_cond_timedwait_soloader(condition,mutex,deadline);
    diagnostics_wait_leave(tracked);
    return result;
}
static int trace_query(unsigned *counter) {
    unsigned count=__atomic_add_fetch(counter,1,__ATOMIC_RELAXED);
    return count<=128 || !(count & (count-1));
}
static void active_attribute(GLuint program, GLuint index, GLsizei capacity,
                             GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    static unsigned calls; int trace=trace_query(&calls);
    if (trace) LOG_INFO("GL attribute query enter: program=%u index=%u capacity=%d",program,index,capacity);
    glGetActiveAttrib_soloader(program,index,capacity,length,size,type,name);
    if (trace) LOG_INFO("GL attribute query returned: program=%u index=%u",program,index);
}
static void active_uniform(GLuint program, GLuint index, GLsizei capacity,
                           GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    static unsigned calls; int trace=trace_query(&calls);
    if (trace) LOG_INFO("GL uniform query enter: program=%u index=%u capacity=%d",program,index,capacity);
    glGetActiveUniform_soloader(program,index,capacity,length,size,type,name);
    if (trace) LOG_INFO("GL uniform query returned: program=%u index=%u",program,index);
}
static GLint attribute_location(GLuint program, const GLchar *name) {
    static unsigned calls; int trace=trace_query(&calls);
    if (trace) LOG_INFO("GL attribute location enter: program=%u",program);
    GLint result=glGetAttribLocation_soloader(program,name);
    if (trace) LOG_INFO("GL attribute location returned: program=%u location=%d",program,result);
    return result;
}
static GLint uniform_location(GLuint program, const GLchar *name) {
    static unsigned calls; int trace=trace_query(&calls);
    if (trace) LOG_INFO("GL uniform location enter: program=%u",program);
    GLint result=glGetUniformLocation_soloader(program,name);
    if (trace) LOG_INFO("GL uniform location returned: program=%u location=%d",program,result);
    return result;
}
static GLenum graphics_error(void) {
    static unsigned calls; unsigned count=__atomic_add_fetch(&calls,1,__ATOMIC_RELAXED);
    GLenum result=glGetError_soloader();
    if (count<=16 || !(count & (count-1))) LOG_INFO("GL error query #%u returned 0x%x",count,result);
    return result;
}
static struct { const char *name; uintptr_t address; } overrides[] = {
    {"pthread_mutex_lock",(uintptr_t)tracked_mutex_lock},
    {"pthread_cond_wait",(uintptr_t)tracked_condition_wait},
    {"pthread_cond_timedwait",(uintptr_t)tracked_condition_timedwait},
    {"glGetActiveAttrib",(uintptr_t)active_attribute}, {"glGetActiveUniform",(uintptr_t)active_uniform},
    {"glGetAttribLocation",(uintptr_t)attribute_location}, {"glGetUniformLocation",(uintptr_t)uniform_location},
    {"glGetError",(uintptr_t)graphics_error},
    {"__gnu_Unwind_Find_exidx",(uintptr_t)__gnu_Unwind_Find_exidx},
    {"__assert2", (uintptr_t)android_assert}, {"clearerr",(uintptr_t)clearerr},
    {"__sF", (uintptr_t)__sF_fake}, {"dlsym", (uintptr_t)app_dlsym},
    {"__fpclassifyd", (uintptr_t)classify}, {"asctime", (uintptr_t)asctime},
    {"dladdr", (uintptr_t)address_info}, {"eglGetCurrentDisplay", (uintptr_t)current_display},
    {"eglGetCurrentSurface", (uintptr_t)current_surface}, {"eglSwapBuffers", (uintptr_t)present}, {"epoll_create", (uintptr_t)vegas_epoll_create},
    {"epoll_ctl", (uintptr_t)vegas_epoll_ctl}, {"epoll_wait", (uintptr_t)vegas_epoll_wait},
    {"read", (uintptr_t)vegas_read}, {"write", (uintptr_t)vegas_write},
    {"close", (uintptr_t)vegas_close}, {"gettid", (uintptr_t)tid},
    {"getuid", (uintptr_t)app_uid}, {"geteuid", (uintptr_t)app_uid},
    {"getgid", (uintptr_t)app_uid}, {"getegid", (uintptr_t)app_uid}, {"getpwuid", (uintptr_t)app_passwd},
    {"modff", (uintptr_t)modff}, {"msync", (uintptr_t)vegas_msync}, {"pread", (uintptr_t)pread},
    {"pthread_attr_getdetachstate", (uintptr_t)detach_state},
    {"pthread_condattr_init", (uintptr_t)pthread_condattr_init_soloader},
    {"sendfile", (uintptr_t)copy_file}, {"statfs", (uintptr_t)android_statfs},
    {"strptime", (uintptr_t)strptime}, {"syslog", (uintptr_t)android_syslog},
    {"times", (uintptr_t)times}, {"umask", (uintptr_t)app_umask}, {"writev", (uintptr_t)vector_write},
    {"zError", (uintptr_t)zError}, {"zlibVersion", (uintptr_t)zlibVersion},
};
void *vegas_symbol(const char *name) {
    void *offline=vegas_offline_symbol(name);
    if (offline) return offline;
    for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); ++i)
        if (!strcmp(name, overrides[i].name)) return (void *)overrides[i].address;
    return dlsym_soloader(NULL, name);
}
