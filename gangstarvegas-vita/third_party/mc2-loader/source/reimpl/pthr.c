/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <psp2/kernel/error.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define PTHR_INITIAL_OBJECT_CAPACITY 128

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

typedef enum {
    INITIALIZED_MUTEX,
    INITIALIZED_COND,
} initialized_object_kind;

typedef struct initialized_object {
    void *wrapper;
    void *real;
    initialized_object_kind kind;
    struct initialized_object *hash_next;
    unsigned references;
    unsigned serial;
    int retired;
} initialized_object_t;

static initialized_object_t **initialized_objects;
static size_t initialized_object_count;
static size_t initialized_object_capacity;
static pthread_mutex_t initialized_objects_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lazy_init_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned initialized_object_serial;
static unsigned initialized_object_created[2];
static unsigned initialized_object_destroyed[2];
static unsigned initialized_object_retired[2];

/* Rio creates close to two thousand lazy Bionic mutex wrappers while loading
 * the city.  The old compatibility layer linearly scanned the entire list on
 * every lock/unlock, making worker synchronization progressively slower as
 * loading advanced.  Keep the ownership vector for statistics and lifetime
 * management, but use buckets for the hot address lookup. */
#define PTHR_OBJECT_BUCKETS 4096
static initialized_object_t *initialized_object_buckets[PTHR_OBJECT_BUCKETS];

static size_t objectBucket(const void *wrapper,
                           initialized_object_kind kind) {
    uintptr_t value = (uintptr_t)wrapper;
    value ^= value >> 13;
    value ^= value >> 7;
    value ^= (uintptr_t)kind * 0x9e3779b9u;
    return (size_t)value & (PTHR_OBJECT_BUCKETS - 1);
}

#define PTHR_MUTEX_OWNER_SLOTS 64

typedef struct mutex_owner_slot {
    void *wrapper;
    initialized_object_t *object;
    unsigned depth;
    struct mutex_owner_slot *next;
} mutex_owner_slot;

/* Successful locks retain the original mutex state until the same thread
 * unlocks it. Rio stream tasks can destroy and reuse their four-byte wrapper
 * while another worker is returning from a lock or condition operation. */
typedef struct {
    mutex_owner_slot slots[PTHR_MUTEX_OWNER_SLOTS];
    mutex_owner_slot *overflow;
} mutex_owner_state;

static __thread mutex_owner_state mutex_owners;

extern void port_trace(const char *format, ...);

static initialized_object_t *find_object_locked(
        const void *wrapper, initialized_object_kind kind) {
    size_t bucket = objectBucket(wrapper, kind);
    for (initialized_object_t *object = initialized_object_buckets[bucket];
         object; object = object->hash_next) {
        if (!object->retired && object->wrapper == wrapper &&
            object->kind == kind) {
            return object;
        }
    }
    return NULL;
}

static initialized_object_t *acquireObject(
        const void *wrapper, initialized_object_kind kind) {
    pthread_mutex_lock(&initialized_objects_lock);
    initialized_object_t *object = find_object_locked(wrapper, kind);
    if (object) {
        ++object->references;
    }
    pthread_mutex_unlock(&initialized_objects_lock);
    return object;
}

int isObjectInitialized(const void *wrapper) {
    pthread_mutex_lock(&initialized_objects_lock);
    int initialized = find_object_locked(wrapper, INITIALIZED_MUTEX) != NULL ||
                      find_object_locked(wrapper, INITIALIZED_COND) != NULL;
    pthread_mutex_unlock(&initialized_objects_lock);
    return initialized;
}

static int rememberObject(void *wrapper, void *real,
                          initialized_object_kind kind) {
    initialized_object_t *object = calloc(1, sizeof(*object));
    if (!object) return 0;
    object->wrapper = wrapper;
    object->real = real;
    object->kind = kind;

    pthread_mutex_lock(&initialized_objects_lock);

    if (initialized_object_count == initialized_object_capacity) {
        size_t new_capacity = initialized_object_capacity
            ? initialized_object_capacity * 2
            : PTHR_INITIAL_OBJECT_CAPACITY;
        if (new_capacity < initialized_object_capacity ||
            new_capacity > SIZE_MAX / sizeof(*initialized_objects)) {
            pthread_mutex_unlock(&initialized_objects_lock);
            free(object);
            return 0;
        }

        initialized_object_t **new_objects = realloc(
            initialized_objects, new_capacity * sizeof(*new_objects));
        if (!new_objects) {
            pthread_mutex_unlock(&initialized_objects_lock);
            free(object);
            return 0;
        }
        initialized_objects = new_objects;
        initialized_object_capacity = new_capacity;
    }

    object->serial = ++initialized_object_serial;
    initialized_objects[initialized_object_count] = object;
    ++initialized_object_count;
    size_t bucket = objectBucket(wrapper, kind);
    object->hash_next = initialized_object_buckets[bucket];
    initialized_object_buckets[bucket] = object;
    ++initialized_object_created[kind];
    pthread_mutex_unlock(&initialized_objects_lock);
    return 1;
}

static void destroyRetiredObject(initialized_object_t *object) {
    free(object->real);
    pthread_mutex_lock(&initialized_objects_lock);
    ++initialized_object_destroyed[object->kind];
    unsigned created = initialized_object_created[object->kind];
    unsigned destroyed = initialized_object_destroyed[object->kind];
    pthread_mutex_unlock(&initialized_objects_lock);
    if (destroyed <= 16 || destroyed % 128 == 0) {
        port_trace("PTHREAD: destroyed %s serial=%u live=%u total=%u",
                   object->kind == INITIALIZED_MUTEX ? "mutex" : "cond",
                   object->serial, created - destroyed, destroyed);
    }
    free(object);
}

static void releaseObject(initialized_object_t *object) {
    if (!object) return;
    int destroy = 0;
    pthread_mutex_lock(&initialized_objects_lock);
    if (object->references > 0) --object->references;
    if (object->retired && object->references == 0) destroy = 1;
    pthread_mutex_unlock(&initialized_objects_lock);
    if (destroy) destroyRetiredObject(object);
}

static initialized_object_t *retireObject(
        const void *wrapper, initialized_object_kind kind) {
    initialized_object_t *object = NULL;
    pthread_mutex_lock(&initialized_objects_lock);
    object = find_object_locked(wrapper, kind);
    if (object) {
        size_t bucket = objectBucket(wrapper, kind);
        initialized_object_t **link = &initialized_object_buckets[bucket];
        while (*link && *link != object) link = &(*link)->hash_next;
        if (*link == object) *link = object->hash_next;
        object->hash_next = NULL;

        for (size_t i = 0; i < initialized_object_count; ++i) {
            if (initialized_objects[i] == object) {
                initialized_objects[i] =
                    initialized_objects[--initialized_object_count];
                break;
            }
        }
        object->retired = 1;
        ++initialized_object_retired[kind];
        /* Hand the caller a stable reference. It releases this after
         * clearing the four-byte Bionic wrapper; the final release then
         * performs the real host destroy when no operation owns it. */
        ++object->references;
    }
    pthread_mutex_unlock(&initialized_objects_lock);
    return object;
}

static unsigned objectReferenceCount(initialized_object_t *object) {
    pthread_mutex_lock(&initialized_objects_lock);
    unsigned references = object->references;
    pthread_mutex_unlock(&initialized_objects_lock);
    return references;
}

void pthr_dump_object_stats(void) {
    unsigned active_mutexes = 0;
    unsigned active_conds = 0;
    pthread_mutex_lock(&initialized_objects_lock);
    for (size_t i = 0; i < initialized_object_count; ++i) {
        if (initialized_objects[i]->kind == INITIALIZED_MUTEX) {
            ++active_mutexes;
        } else {
            ++active_conds;
        }
    }
    unsigned mutex_created = initialized_object_created[INITIALIZED_MUTEX];
    unsigned mutex_destroyed = initialized_object_destroyed[INITIALIZED_MUTEX];
    unsigned mutex_retired = initialized_object_retired[INITIALIZED_MUTEX];
    unsigned cond_created = initialized_object_created[INITIALIZED_COND];
    unsigned cond_destroyed = initialized_object_destroyed[INITIALIZED_COND];
    unsigned cond_retired = initialized_object_retired[INITIALIZED_COND];
    pthread_mutex_unlock(&initialized_objects_lock);

    port_trace("PTHREAD: stats mutex active=%u live=%u created=%u retired=%u destroyed=%u "
               "cond active=%u live=%u created=%u retired=%u destroyed=%u",
               active_mutexes, mutex_created - mutex_destroyed,
               mutex_created, mutex_retired, mutex_destroyed,
               active_conds, cond_created - cond_destroyed,
               cond_created, cond_retired, cond_destroyed);
}

typedef struct pthr_waiter {
    struct pthr_waiter *next;
    SceUID sema;
    SceUID thread;
    int notified;
} pthr_waiter;

typedef struct {
    unsigned guard;
    SceUID owner;
    unsigned depth;
    int kind;
    pthr_waiter *first;
    pthr_waiter *last;
} pthr_mutex;

typedef struct {
    unsigned guard;
    clockid_t clock;
    pthr_waiter *first;
    pthr_waiter *last;
} pthr_cond;

/* These guards protect only queue changes and ownership handoff. Threads
 * waiting for a mutex or condition sleep on their own semaphore; idle Bionic
 * wrappers therefore consume no kernel UIDs. */
static void queueLock(unsigned *guard) {
    while (__atomic_exchange_n(guard, 1, __ATOMIC_ACQUIRE))
        sceKernelDelayThread(1);
}

static void queueUnlock(unsigned *guard) {
    __atomic_store_n(guard, 0, __ATOMIC_RELEASE);
}

static void appendWaiter(pthr_waiter **first, pthr_waiter **last,
                         pthr_waiter *waiter) {
    waiter->next = NULL;
    if (*last) (*last)->next = waiter;
    else *first = waiter;
    *last = waiter;
}

static void removeWaiter(pthr_waiter **first, pthr_waiter **last,
                         pthr_waiter *waiter) {
    pthr_waiter *previous = NULL;
    for (pthr_waiter *entry = *first; entry; entry = entry->next) {
        if (entry == waiter) {
            if (previous) previous->next = entry->next;
            else *first = entry->next;
            if (*last == entry) *last = previous;
            return;
        }
        previous = entry;
    }
}

static int kernelWaitError(int result) {
    if (result == (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT) return ETIMEDOUT;
    return result < 0 ? EINVAL : 0;
}

static int initWaiter(pthr_waiter *waiter) {
    memset(waiter, 0, sizeof(*waiter));
    waiter->thread = sceKernelGetThreadId();
    waiter->sema = sceKernelCreateSema("BionicPthreadWait", 0, 0, 1, NULL);
    return waiter->sema < 0 ? EAGAIN : 0;
}

static int waitForNotification(pthr_waiter *waiter, clockid_t clock,
                               const struct timespec *deadline) {
    if (!deadline)
        return kernelWaitError(sceKernelWaitSema(waiter->sema, 1, NULL));
    for (;;) {
        struct timespec now;
        if (clock_gettime(clock, &now) != 0) return errno;
        int64_t seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
        int64_t nanos = (int64_t)deadline->tv_nsec - now.tv_nsec;
        if (nanos < 0) {
            --seconds;
            nanos += 1000000000;
        }
        if (seconds < 0 || (seconds == 0 && nanos == 0))
            return ETIMEDOUT;
        /* Kernel waits use a 32-bit relative microsecond timeout. Round up
         * sub-microsecond deadlines and repeat only for longer absolute waits. */
        uint64_t usec = (uint64_t)seconds * 1000000u +
                        ((uint64_t)nanos + 999u) / 1000u;
        SceUInt timeout = usec > UINT32_MAX ? UINT32_MAX : (SceUInt)usec;
        int result = sceKernelWaitSema(waiter->sema, 1, &timeout);
        if (result != (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT)
            return kernelWaitError(result);
    }
}

static int takeMutexLocked(pthr_mutex *mutex, SceUID thread, int try_only) {
    if (!mutex->owner) {
        mutex->owner = thread;
        mutex->depth = 1;
        return 0;
    }
    if (mutex->owner == thread) {
        if (mutex->kind == PTHREAD_MUTEX_RECURSIVE) {
            if (mutex->depth == UINT_MAX) return EAGAIN;
            ++mutex->depth;
            return 0;
        }
        if (!try_only && mutex->kind == PTHREAD_MUTEX_ERRORCHECK)
            return EDEADLK;
    }
    return EBUSY;
}

static int lockMutex(pthr_mutex *mutex, int try_only, pthr_waiter *existing) {
    SceUID thread = sceKernelGetThreadId();
    queueLock(&mutex->guard);
    int result = takeMutexLocked(mutex, thread, try_only);
    queueUnlock(&mutex->guard);
    if (result != EBUSY || try_only) return result;

    pthr_waiter local;
    pthr_waiter *waiter = existing ? existing : &local;
    if (existing) {
        /* A condition signal can win after the kernel timeout. Its semaphore
         * token must be consumed before reusing this waiter for mutex handoff. */
        sceKernelPollSema(waiter->sema, 1);
        waiter->notified = 0;
    } else {
        result = initWaiter(waiter);
        if (result != 0) return result;
    }
    queueLock(&mutex->guard);
    result = takeMutexLocked(mutex, thread, 0);
    if (result == EBUSY)
        appendWaiter(&mutex->first, &mutex->last, waiter);
    queueUnlock(&mutex->guard);
    if (result == EBUSY) {
        result = waitForNotification(waiter, CLOCK_REALTIME, NULL);
        queueLock(&mutex->guard);
        if (waiter->notified) result = 0;
        else removeWaiter(&mutex->first, &mutex->last, waiter);
        queueUnlock(&mutex->guard);
    }
    if (!existing) {
        int deleted = sceKernelDeleteSema(waiter->sema);
        /* An acquired mutex must remain reported as acquired even if deleting
         * an already detached waiter fails; callers need to know they own it. */
        if (deleted < 0)
            port_trace("PTHREAD: waiter delete failed uid=0x%x ret=0x%x", waiter->sema, deleted);
    }
    return result;
}

static int unlockMutex(pthr_mutex *mutex) {
    queueLock(&mutex->guard);
    if (mutex->owner != sceKernelGetThreadId()) {
        queueUnlock(&mutex->guard);
        return EPERM;
    }
    if (mutex->depth > 1) {
        --mutex->depth;
    } else if (mutex->first) {
        pthr_waiter *waiter = mutex->first;
        int result = sceKernelSignalSema(waiter->sema, 1);
        if (result < 0) {
            queueUnlock(&mutex->guard);
            return kernelWaitError(result);
        }
        removeWaiter(&mutex->first, &mutex->last, waiter);
        mutex->owner = waiter->thread;
        mutex->depth = 1;
        waiter->notified = 1;
    } else {
        mutex->owner = 0;
        mutex->depth = 0;
    }
    queueUnlock(&mutex->guard);
    return 0;
}

static int notifyCondition(pthr_cond *cond, int broadcast) {
    int result = 0;
    queueLock(&cond->guard);
    while (cond->first) {
        pthr_waiter *waiter = cond->first;
        result = kernelWaitError(sceKernelSignalSema(waiter->sema, 1));
        if (result != 0) break;
        removeWaiter(&cond->first, &cond->last, waiter);
        waiter->notified = 1;
        if (!broadcast) break;
    }
    queueUnlock(&cond->guard);
    return result;
}

static int ownerSlotIsInline(const mutex_owner_state *owners,
                             const mutex_owner_slot *slot) {
    uintptr_t address = (uintptr_t)slot;
    return address >= (uintptr_t)owners->slots &&
           address < (uintptr_t)(owners->slots + PTHR_MUTEX_OWNER_SLOTS);
}

static mutex_owner_slot *nextMutexOwner(mutex_owner_state *owners,
                                        mutex_owner_slot *slot) {
    if (!ownerSlotIsInline(owners, slot)) return slot->next;
    if (slot + 1 < owners->slots + PTHR_MUTEX_OWNER_SLOTS) return slot + 1;
    return owners->overflow;
}

static initialized_object_t *ownedMutex(const void *wrapper) {
    mutex_owner_state *owners = &mutex_owners;
    void *current_real = wrapper
        ? ((const pthread_mutex_t_bionic *)wrapper)->real_ptr
        : NULL;
    initialized_object_t *fallback = NULL;
    for (mutex_owner_slot *slot = owners->slots; slot;
         slot = nextMutexOwner(owners, slot)) {
        if (!slot->depth || slot->wrapper != wrapper) continue;
        if (current_real && slot->object->real == current_real)
            return slot->object;
        fallback = slot->object;
    }
    return fallback;
}

static mutex_owner_slot *reserveMutexOwner(
        mutex_owner_state *owners, void *wrapper, initialized_object_t *object) {
    mutex_owner_slot *empty = NULL;
    for (mutex_owner_slot *slot = owners->slots; slot;
         slot = nextMutexOwner(owners, slot)) {
        if (slot->depth && slot->wrapper == wrapper && slot->object == object)
            return slot;
        if (!slot->depth && !empty) empty = slot;
    }
    if (!empty) {
        empty = calloc(1, sizeof(*empty));
        if (!empty) return NULL;
        empty->next = owners->overflow;
        owners->overflow = empty;
    }
    empty->wrapper = wrapper;
    empty->object = object;
    return empty;
}

static void discardMutexOwner(mutex_owner_state *owners,
                              mutex_owner_slot *slot) {
    if (ownerSlotIsInline(owners, slot)) {
        slot->wrapper = NULL;
        slot->object = NULL;
        return;
    }
    mutex_owner_slot **link = &owners->overflow;
    while (*link && *link != slot) link = &(*link)->next;
    if (*link) *link = slot->next;
    free(slot);
}

static void releaseMutexOwner(void *wrapper, initialized_object_t *object) {
    mutex_owner_state *owners = &mutex_owners;
    for (mutex_owner_slot *slot = owners->slots; slot;
         slot = nextMutexOwner(owners, slot)) {
        if (!slot->depth || slot->wrapper != wrapper ||
            slot->object != object) continue;
        if (--slot->depth == 0) discardMutexOwner(owners, slot);
        releaseObject(object);
        return;
    }
}

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    int ret = 0;
    pthread_mutex_lock(&lazy_init_lock);
    if (attr->magic != 0x42424242) {
        pthread_attr_t *real_attr = malloc(sizeof(*real_attr));
        if (!real_attr) {
            pthread_mutex_unlock(&lazy_init_lock);
            return ENOMEM;
        }
        ret = pthread_attr_init(real_attr);
        if (ret == 0) {
            attr->real_ptr = real_attr;
            attr->magic = 0x42424242;
        } else {
            free(real_attr);
        }
    }
    pthread_mutex_unlock(&lazy_init_lock);
    return ret;
}

// null check for `mutex` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _mutex_t_static_init(pthread_mutex_t_bionic *mutex,
                                    const pthread_mutexattr_t *attr) {
    int ret = 0, kind = PTHREAD_MUTEX_NORMAL;
    pthread_mutex_lock(&lazy_init_lock);
    initialized_object_t *registered = acquireObject(mutex, INITIALIZED_MUTEX);
    if (registered && mutex->real_ptr == registered->real) {
        releaseObject(registered);
        pthread_mutex_unlock(&lazy_init_lock);
        return 0;
    }
    if (registered) {
        releaseObject(registered);
        initialized_object_t *stale = retireObject(mutex, INITIALIZED_MUTEX);
        releaseObject(stale);
    }
    if (attr) {
        int shared;
        ret = pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &kind);
        if (ret == 0) ret = pthread_mutexattr_getpshared(attr, &shared);
        if (ret == 0 && shared != PTHREAD_PROCESS_PRIVATE) ret = ENOTSUP;
    } else if (*(int *)mutex == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) {
        kind = PTHREAD_MUTEX_RECURSIVE;
    } else if (*(int *)mutex == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) {
        kind = PTHREAD_MUTEX_ERRORCHECK;
    }
    if (ret == 0 && kind != PTHREAD_MUTEX_NORMAL &&
        kind != PTHREAD_MUTEX_RECURSIVE && kind != PTHREAD_MUTEX_ERRORCHECK)
        ret = EINVAL;
    if (ret == 0) {
        pthr_mutex *real_mutex = calloc(1, sizeof(*real_mutex));
        if (!real_mutex) ret = ENOMEM;
        else {
            real_mutex->kind = kind;
            if (rememberObject(mutex, real_mutex, INITIALIZED_MUTEX))
                mutex->real_ptr = real_mutex;
            else {
                free(real_mutex);
                ret = ENOMEM;
            }
        }
    }
    pthread_mutex_unlock(&lazy_init_lock);
    return ret;
}

// null check for `cond` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic *cond,
                                   const pthread_condattr_t *attr) {
    int ret = 0;
    pthread_mutex_lock(&lazy_init_lock);
    initialized_object_t *registered = acquireObject(cond, INITIALIZED_COND);
    if (registered && cond->real_ptr == registered->real) {
        releaseObject(registered);
        pthread_mutex_unlock(&lazy_init_lock);
        return 0;
    }
    if (registered) {
        releaseObject(registered);
        initialized_object_t *stale = retireObject(cond, INITIALIZED_COND);
        releaseObject(stale);
    }
    clockid_t clock = CLOCK_REALTIME;
    if (attr) {
        int shared;
        ret = pthread_condattr_getpshared(attr, &shared);
        if (ret == 0 && shared != PTHREAD_PROCESS_PRIVATE) ret = ENOTSUP;
        if (ret == 0) ret = pthread_condattr_getclock(attr, &clock);
    }
    if (ret == 0 && clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
        ret = EINVAL;
    if (ret == 0) {
        pthr_cond *real_cond = calloc(1, sizeof(*real_cond));
        if (!real_cond) ret = ENOMEM;
        else {
            real_cond->clock = clock;
            if (rememberObject(cond, real_cond, INITIALIZED_COND))
                cond->real_ptr = real_cond;
            else {
                free(real_cond);
                ret = ENOMEM;
            }
        }
    }
    pthread_mutex_unlock(&lazy_init_lock);
    return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        ret = _attr_t_static_init((pthread_attr_t_bionic *) attr);
        if (ret != 0)
            return ret;
        pthread_attr_setstacksize(attr->real_ptr, 512 * 1024);
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }
    port_trace("PTHREAD: created thread=%p start=%p ret=%d", thread ? (void *)*thread : NULL, start, ret);

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    pthread_mutex_lock(&lazy_init_lock);
    initialized_object_t *object = retireObject(mutex, INITIALIZED_MUTEX);
    if (!object) {
        mutex->real_ptr = NULL;
        pthread_mutex_unlock(&lazy_init_lock);
        return 0;
    }
    mutex->real_ptr = NULL;
    pthread_mutex_unlock(&lazy_init_lock);

    unsigned references = objectReferenceCount(object);
    unsigned retired;
    pthread_mutex_lock(&initialized_objects_lock);
    retired = initialized_object_retired[INITIALIZED_MUTEX];
    pthread_mutex_unlock(&initialized_objects_lock);
    if (retired <= 16 || retired % 128 == 0) {
        port_trace("PTHREAD: retire mutex wrapper=%p real=%p serial=%u refs=%u total=%u",
                   mutex, object->real, object->serial,
                   references ? references - 1 : 0, retired);
    }
    releaseObject(object);
    return 0;
}

static int acquireMutex(pthread_mutex_t_bionic *mutex, int try_only) {
    if (!mutex) return EINVAL;
    int ret = _mutex_t_static_init(mutex, NULL);
    if (ret != 0) return ret;
    initialized_object_t *object = acquireObject(mutex, INITIALIZED_MUTEX);
    if (!object) return EINVAL;
    mutex_owner_state *owners = &mutex_owners;
    mutex_owner_slot *slot = reserveMutexOwner(owners, mutex, object);
    if (!slot) {
        ret = ENOMEM;
        if (try_only) {
            pthr_mutex *real_mutex = object->real;
            queueLock(&real_mutex->guard);
            if (real_mutex->owner) ret = EBUSY;
            queueUnlock(&real_mutex->guard);
        }
        releaseObject(object);
        return ret;
    }
    ret = lockMutex(object->real, try_only, NULL);
    if (ret == 0) {
        ++slot->depth;
    } else {
        if (!slot->depth) discardMutexOwner(owners, slot);
        releaseObject(object);
    }
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex) {
    return acquireMutex(mutex, 0);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex) {
    return acquireMutex(mutex, 1);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    initialized_object_t *object = ownedMutex(mutex);
    int transient = 0;
    if (!object) {
        object = acquireObject(mutex, INITIALIZED_MUTEX);
        transient = 1;
    }
    if (!object) return EINVAL;
    int ret = unlockMutex(object->real);
    if (ret == 0 && !transient) {
        releaseMutexOwner(mutex, object);
    } else if (transient) {
        releaseObject(object);
    }
    return ret;
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    return _cond_t_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    pthread_mutex_lock(&lazy_init_lock);
    initialized_object_t *object = retireObject(cond, INITIALIZED_COND);
    if (!object) {
        cond->real_ptr = NULL;
        pthread_mutex_unlock(&lazy_init_lock);
        return 0;
    }
    cond->real_ptr = NULL;
    pthread_mutex_unlock(&lazy_init_lock);
    unsigned references = objectReferenceCount(object);
    unsigned retired;
    pthread_mutex_lock(&initialized_objects_lock);
    retired = initialized_object_retired[INITIALIZED_COND];
    pthread_mutex_unlock(&initialized_objects_lock);
    if (retired <= 16 || retired % 128 == 0) {
        port_trace("PTHREAD: retire cond wrapper=%p real=%p serial=%u refs=%u total=%u",
                   cond, object->real, object->serial,
                   references ? references - 1 : 0, retired);
    }
    releaseObject(object);
    return 0;
}

static int notifyCond(pthread_cond_t_bionic *cond, int broadcast) {
    if (!cond) return EINVAL;
    int ret = _cond_t_static_init(cond, NULL);
    if (ret != 0) return ret;
    initialized_object_t *object = acquireObject(cond, INITIALIZED_COND);
    if (!object) return EINVAL;
    ret = notifyCondition(object->real, broadcast);
    releaseObject(object);
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond) {
    return notifyCond(cond, 0);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond) {
    return notifyCond(cond, 1);
}

static int waitCondition(pthread_cond_t_bionic *cond,
                         pthread_mutex_t_bionic *mutex,
                         const struct timespec *deadline) {
    if (!cond || !mutex) return EINVAL;
    if (deadline && (deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000))
        return EINVAL;
    int ret = _cond_t_static_init(cond, NULL);
    if (ret != 0) return ret;
    initialized_object_t *cond_object = acquireObject(cond, INITIALIZED_COND);
    initialized_object_t *mutex_object = ownedMutex(mutex);
    if (!cond_object || !mutex_object) {
        releaseObject(cond_object);
        return EPERM;
    }
    pthr_waiter waiter;
    ret = initWaiter(&waiter);
    if (ret != 0) {
        releaseObject(cond_object);
        return ret;
    }
    pthr_cond *real_cond = cond_object->real;
    queueLock(&real_cond->guard);
    appendWaiter(&real_cond->first, &real_cond->last, &waiter);
    /* Keeping the condition queue guarded until unlock completes closes the
     * signal-before-sleep race. The existing mutex ownership reference stays
     * alive throughout wait, including destroy/reuse of its Bionic wrapper. */
    ret = unlockMutex(mutex_object->real);
    if (ret != 0) {
        removeWaiter(&real_cond->first, &real_cond->last, &waiter);
        queueUnlock(&real_cond->guard);
        sceKernelDeleteSema(waiter.sema);
        releaseObject(cond_object);
        return ret;
    }
    queueUnlock(&real_cond->guard);
    ret = waitForNotification(&waiter, real_cond->clock, deadline);
    queueLock(&real_cond->guard);
    if (waiter.notified) ret = 0;
    else removeWaiter(&real_cond->first, &real_cond->last, &waiter);
    queueUnlock(&real_cond->guard);
    /* Reuse the condition semaphore if reacquiring needs to block. No new
     * kernel object is required after the caller has released its mutex. */
    int locked = lockMutex(mutex_object->real, 0, &waiter);
    int deleted = sceKernelDeleteSema(waiter.sema);
    if (deleted < 0)
        port_trace("PTHREAD: waiter delete failed uid=0x%x ret=0x%x", waiter.sema, deleted);
    if (locked != 0) releaseMutexOwner(mutex, mutex_object);
    releaseObject(cond_object);
    return locked != 0 ? locked : ret;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond,
                                   pthread_mutex_t_bionic *mutex,
                                   struct timespec *abstime) {
    if (!abstime) return EINVAL;
    return waitCondition(cond, mutex, abstime);
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond,
                              pthread_mutex_t_bionic *mutex) {
    return waitCondition(cond, mutex, NULL);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    pthread_mutex_lock(&lazy_init_lock);
    if (attr->magic != 0x42424242) {
        pthread_mutex_unlock(&lazy_init_lock);
        return 0;
    }

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->real_ptr = NULL;
    attr->magic = 0x0;
    pthread_mutex_unlock(&lazy_init_lock);

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    int ret = _attr_t_static_init(attr);
    if (ret != 0) return ret;
    state = !state; // pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC...
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    int ret = _attr_t_static_init(attr);
    if (ret != 0) return ret;
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return EINVAL;

    int expected = 0;
    if (__atomic_compare_exchange_n(once_control, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        (*init_routine)();
        __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) != 2)
            sceKernelDelayThread(1000);
    }
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    if (!uid || !sval) return -1;
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    if (!abstime) return -1;
    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    if (_timeout - now <= 0) return -1;
    unsigned long long remaining = (unsigned long long) (_timeout - now);
    uint timeout_real = remaining > UINT32_MAX ? UINT32_MAX : (uint) remaining;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0)
        return -1;
    return 0;
}

int sem_trywait_soloader (int * uid) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}
