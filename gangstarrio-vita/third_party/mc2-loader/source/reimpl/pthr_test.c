#include "reimpl/pthr.h"
#include "reimpl/pthr_test.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <psp2/kernel/threadmgr.h>

extern void port_trace(const char *format, ...);

typedef struct {
    pthread_mutex_t_bionic mutex;
    pthread_cond_t_bionic condition;
    unsigned counter;
    unsigned ready;
    unsigned done;
    unsigned errors;
    int predicate;
    int wait_result;
} test_state;

static void record_error(test_state *state, int result) {
    if (result) __atomic_add_fetch(&state->errors, 1, __ATOMIC_RELAXED);
}

static int wait_count(unsigned *counter, unsigned target) {
    int64_t deadline = sceKernelGetSystemTimeWide() + 3000000;
    while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < target) {
        if (sceKernelGetSystemTimeWide() >= deadline) return ETIMEDOUT;
        sceKernelDelayThread(1000);
    }
    return 0;
}

static int start_thread(pthread_t *thread, void *(*start)(void *), void *data) {
    pthread_attr_t attr;
    int result = pthread_attr_init(&attr);
    if (result) return result;
    result = pthread_attr_setstacksize(&attr, 128 * 1024);
    if (!result) result = pthread_create(thread, &attr, start, data);
    pthread_attr_destroy(&attr);
    return result;
}

static void *count_thread(void *data) {
    test_state *state = data;
    for (unsigned i = 0; i < 2000; ++i) {
        int result = pthread_mutex_lock_soloader(&state->mutex);
        if (result) { record_error(state, result); break; }
        ++state->counter;
        record_error(state, pthread_mutex_unlock_soloader(&state->mutex));
    }
    __atomic_add_fetch(&state->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *condition_thread(void *data) {
    test_state *state = data;
    int result = pthread_mutex_lock_soloader(&state->mutex);
    record_error(state, result);
    if (!result) {
        __atomic_add_fetch(&state->ready, 1, __ATOMIC_RELEASE);
        while (!state->predicate) {
            struct timespec deadline;
            if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
                record_error(state, errno ? errno : EINVAL);
                break;
            }
            deadline.tv_sec += 2;
            result = pthread_cond_timedwait_soloader(
                &state->condition, &state->mutex, &deadline);
            if (result) { record_error(state, result); break; }
        }
        record_error(state, pthread_mutex_unlock_soloader(&state->mutex));
    }
    __atomic_add_fetch(&state->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *foreign_owner_thread(void *data) {
    test_state *state = data;
    if (pthread_mutex_unlock_soloader(&state->mutex) != EPERM)
        record_error(state, EINVAL);
    if (pthread_mutex_trylock_soloader(&state->mutex) != EBUSY)
        record_error(state, EINVAL);
    __atomic_store_n(&state->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *short_condition_thread(void *data) {
    test_state *state = data;
    int result = pthread_mutex_lock_soloader(&state->mutex);
    record_error(state, result);
    if (!result) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            record_error(state, errno ? errno : EINVAL);
        } else {
            deadline.tv_nsec += 20000000;
            if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
            __atomic_store_n(&state->ready, 1, __ATOMIC_RELEASE);
            state->wait_result = pthread_cond_timedwait_soloader(
                &state->condition, &state->mutex, &deadline);
            if (state->wait_result != 0 && state->wait_result != ETIMEDOUT)
                record_error(state, state->wait_result);
            __atomic_add_fetch(&state->counter, 1, __ATOMIC_RELEASE);
        }
        record_error(state, pthread_mutex_unlock_soloader(&state->mutex));
    }
    __atomic_store_n(&state->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

#define REQUIRE(expression) do { \
    int test_result = (expression); \
    if (test_result) { \
        port_trace("PTHREAD TEST: FAIL line=%u result=%d", \
                   (unsigned)__LINE__, test_result); \
        return test_result; \
    } \
} while (0)

static int test_mutex_types(void) {
    pthread_mutexattr_t attr;
    static test_state state;
    state = (test_state){0};
    REQUIRE(pthread_mutexattr_init(&attr));
    REQUIRE(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    REQUIRE(pthread_mutex_init_soloader(&state.mutex, &attr));
    REQUIRE(pthread_mutex_lock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_trylock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_unlock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_unlock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_destroy_soloader(&state.mutex));
    REQUIRE(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK));
    REQUIRE(pthread_mutex_init_soloader(&state.mutex, &attr));
    REQUIRE(pthread_mutex_lock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_lock_soloader(&state.mutex) == EDEADLK ? 0 : EINVAL);
    pthread_t thread;
    REQUIRE(start_thread(&thread, foreign_owner_thread, &state));
    REQUIRE(wait_count(&state.done, 1));
    REQUIRE(pthread_join(thread, NULL));
    REQUIRE(state.errors ? EINVAL : 0);
    REQUIRE(pthread_mutex_unlock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_destroy_soloader(&state.mutex));
    REQUIRE(pthread_mutexattr_destroy(&attr));

    /* Retiring a wrapper must preserve its locked object until final unlock. */
    REQUIRE(pthread_mutex_lock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_destroy_soloader(&state.mutex));
    REQUIRE(pthread_mutex_unlock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_lock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_unlock_soloader(&state.mutex));
    REQUIRE(pthread_mutex_destroy_soloader(&state.mutex));
    return 0;
}

static int test_threads(int condition, unsigned count) {
    static test_state state;
    state = (test_state){0};
    pthread_t threads[4];
    REQUIRE(pthread_mutex_init_soloader(&state.mutex, NULL));
    if (condition) REQUIRE(pthread_cond_init_soloader(&state.condition, NULL));
    for (unsigned i = 0; i < count; ++i)
        REQUIRE(start_thread(&threads[i], condition ? condition_thread : count_thread, &state));
    if (condition) {
        REQUIRE(wait_count(&state.ready, count));
        REQUIRE(pthread_mutex_lock_soloader(&state.mutex));
        state.predicate = 1;
        REQUIRE(count == 1 ? pthread_cond_signal_soloader(&state.condition)
                           : pthread_cond_broadcast_soloader(&state.condition));
        REQUIRE(pthread_mutex_unlock_soloader(&state.mutex));
    }
    REQUIRE(wait_count(&state.done, count));
    for (unsigned i = 0; i < count; ++i) REQUIRE(pthread_join(threads[i], NULL));
    REQUIRE(state.errors ? EINVAL : 0);
    if (!condition) REQUIRE(state.counter == count * 2000 ? 0 : EINVAL);
    if (condition) REQUIRE(pthread_cond_destroy_soloader(&state.condition));
    REQUIRE(pthread_mutex_destroy_soloader(&state.mutex));
    return 0;
}

static int test_many_owned_mutexes(void) {
    static pthread_mutex_t_bionic mutexes[96];
    for (unsigned i = 0; i < 96; ++i)
        REQUIRE(pthread_mutex_lock_soloader(&mutexes[i]));
    for (unsigned i = 0; i < 96; ++i)
        REQUIRE(pthread_mutex_trylock_soloader(&mutexes[i]) == EBUSY ? 0 : EINVAL);
    for (unsigned i = 96; i > 0; --i) {
        REQUIRE(pthread_mutex_unlock_soloader(&mutexes[i - 1]));
        REQUIRE(pthread_mutex_destroy_soloader(&mutexes[i - 1]));
    }
    return 0;
}

static int test_condition_reacquire(int late_signal) {
    static test_state state;
    state = (test_state){0};
    pthread_t thread;
    REQUIRE(pthread_mutex_init_soloader(&state.mutex, NULL));
    REQUIRE(pthread_cond_init_soloader(&state.condition, NULL));
    REQUIRE(start_thread(&thread, short_condition_thread, &state));
    REQUIRE(wait_count(&state.ready, 1));
    REQUIRE(pthread_mutex_lock_soloader(&state.mutex));
    if (late_signal) sceKernelDelayThread(50000);
    REQUIRE(pthread_cond_signal_soloader(&state.condition));
    if (!late_signal) sceKernelDelayThread(50000);
    REQUIRE(!__atomic_load_n(&state.counter, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&state.done, __ATOMIC_ACQUIRE) ? 0 : EINVAL);
    REQUIRE(pthread_mutex_unlock_soloader(&state.mutex));
    REQUIRE(wait_count(&state.done, 1));
    REQUIRE(pthread_join(thread, NULL));
    REQUIRE(!state.errors && state.counter == 1 ? 0 : EINVAL);
    REQUIRE(pthread_cond_destroy_soloader(&state.condition));
    REQUIRE(pthread_mutex_destroy_soloader(&state.mutex));
    return 0;
}

static int test_timeout(clockid_t clock_id) {
    static test_state state;
    state = (test_state){0};
    pthread_condattr_t attr;
    REQUIRE(pthread_condattr_init(&attr));
    REQUIRE(pthread_condattr_setclock(&attr, clock_id));
    REQUIRE(pthread_cond_init_soloader(&state.condition, &attr));
    REQUIRE(pthread_condattr_destroy(&attr));
    REQUIRE(pthread_mutex_lock_soloader(&state.mutex));
    struct timespec deadline;
    REQUIRE(clock_gettime(clock_id, &deadline));
    deadline.tv_nsec += 20000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    uint64_t start = sceKernelGetSystemTimeWide();
    REQUIRE(pthread_cond_timedwait_soloader(&state.condition, &state.mutex, &deadline)
            == ETIMEDOUT ? 0 : EINVAL);
    REQUIRE(sceKernelGetSystemTimeWide() - start >= 5000 ? 0 : EINVAL);
    REQUIRE(pthread_mutex_trylock_soloader(&state.mutex) == EBUSY ? 0 : EINVAL);
    deadline.tv_nsec = 1000000000;
    REQUIRE(pthread_cond_timedwait_soloader(&state.condition, &state.mutex, &deadline)
            == EINVAL ? 0 : EINVAL);
    REQUIRE(pthread_mutex_unlock_soloader(&state.mutex));
    REQUIRE(pthread_cond_destroy_soloader(&state.condition));
    REQUIRE(pthread_mutex_destroy_soloader(&state.mutex));
    return 0;
}

int pthr_run_native_tests(void) {
    port_trace("PTHREAD TEST: begin");
    REQUIRE(test_mutex_types());
    REQUIRE(test_many_owned_mutexes());
    REQUIRE(test_threads(0, 4));
    REQUIRE(test_threads(1, 1));
    REQUIRE(test_threads(1, 4));
    REQUIRE(test_condition_reacquire(0));
    REQUIRE(test_condition_reacquire(1));
    REQUIRE(test_timeout(CLOCK_REALTIME));
    REQUIRE(test_timeout(CLOCK_MONOTONIC));
    pthread_mutex_t_bionic *mutexes = calloc(2500, sizeof(*mutexes));
    REQUIRE(mutexes ? 0 : ENOMEM);
    pthread_cond_t_bionic *conditions = calloc(2500, sizeof(*conditions));
    REQUIRE(conditions ? 0 : ENOMEM);
    for (unsigned i = 0; i < 2500; ++i) {
        REQUIRE(pthread_mutex_init_soloader(&mutexes[i], NULL));
        REQUIRE(pthread_mutex_lock_soloader(&mutexes[i]));
        REQUIRE(pthread_mutex_unlock_soloader(&mutexes[i]));
        REQUIRE(pthread_cond_init_soloader(&conditions[i], NULL));
        REQUIRE(pthread_cond_signal_soloader(&conditions[i]));
    }
    SceKernelLwMutexWork native_work __attribute__((aligned(8)));
    int native_result = sceKernelCreateLwMutex(&native_work, "RioPthreadTest", 0, 0, NULL);
    port_trace("PTHREAD TEST: 2500 mutexes and 2500 conditions live native_create=0x%08x", native_result);
    REQUIRE(native_result);
    REQUIRE(sceKernelDeleteLwMutex(&native_work));
    for (unsigned i = 0; i < 2500; ++i) {
        REQUIRE(pthread_mutex_destroy_soloader(&mutexes[i]));
        REQUIRE(pthread_cond_destroy_soloader(&conditions[i]));
    }
    free(mutexes);
    free(conditions);
    port_trace("PTHREAD TEST: PASS types, ownership, retirement, contention, signal, broadcast, clocks and UID footprint");
    return 0;
}
