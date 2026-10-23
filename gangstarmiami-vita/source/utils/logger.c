/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_int _log_state = ATOMIC_VAR_INIT(0);
static atomic_bool _log_running = ATOMIC_VAR_INIT(false);
static atomic_uint _log_completed = ATOMIC_VAR_INIT(0);
static SceUID _log_thread = -1, _log_wake = -1;
#define LOG_QUEUE_SIZE 64
typedef struct {
    char line[2048];
    unsigned sequence;
    bool important;
} log_entry_t;
static log_entry_t _log_queue[LOG_QUEUE_SIZE];
static unsigned _log_head, _log_count, _log_sequence, _log_dropped;

// All sinks belong to the drain worker. Network output is best effort;
// the local mirror remains available when Wi-Fi is absent.
#define DEBUGNET_PORT 9999
static char _debugnet_net_buf[128 * 1024];
static int _debugnet_sock = -1;
static SceNetSockaddrIn _debugnet_addr;
static bool _debugnet_init_done = false;

static void _debugnet_init(void) {
    _debugnet_init_done = true;

    // sceNetInit/sceNetCtlInit are import stubs that only resolve to real
    // syscalls once the SceNet sysmodule is actually loaded -- without this,
    // the stub jumps through an unresolved NID and the thread takes a
    // Prefetch abort at PC=0x0 right after the `blx sceNetInit` (confirmed in
    // gangstarmiamivindication-psp2core-1788275718, the very first attempt to
    // add debugnet logging).
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return;

    SceNetInitParam net_init_param;
    net_init_param.memory = _debugnet_net_buf;
    net_init_param.size = sizeof(_debugnet_net_buf);
    net_init_param.flags = 0;
    if (sceNetInit(&net_init_param) < 0) return;
    sceNetCtlInit();

    _debugnet_sock = sceNetSocket("debugnet", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (_debugnet_sock < 0) return;

    int nonblocking = 1;
    sceNetSetsockopt(_debugnet_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
                    &nonblocking, sizeof(nonblocking));
    int broadcast = 1;
    sceNetSetsockopt(_debugnet_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST,
                      &broadcast, sizeof(broadcast));

    memset(&_debugnet_addr, 0, sizeof(_debugnet_addr));
    _debugnet_addr.sin_family = SCE_NET_AF_INET;
    _debugnet_addr.sin_port = sceNetHtons(DEBUGNET_PORT);
    _debugnet_addr.sin_addr.s_addr = SCE_NET_INADDR_BROADCAST;
}

static void _debugnet_send(const char *line) {
    if (_debugnet_sock < 0) return;
    sceNetSendto(_debugnet_sock, line, strlen(line), 0,
                 (SceNetSockaddr *) &_debugnet_addr, sizeof(SceNetSockaddrIn));
}

// One local file per game session. Batched writes avoid storage round-trips
// on game/audio threads; important messages wait for a worker sync.
#define LOCAL_LOG_DIR DATA_PATH "logs"
#define LOCAL_LOG_COUNTER_PATH LOCAL_LOG_DIR "/debug_local_counter.txt"

static char _localfile_path[128];
static bool _localfile_init_done = false;
static SceUID _localfile_fd = -1;
static unsigned _localfile_unsynced = 0;

// Ordinary output is written at least once a second and synced in batches;
// fatal/error/warning messages request a sync before publication completes.
#define LOCAL_LOG_SYNC_EVERY 256

static void _localfile_init(void) {
    _localfile_init_done = true;

    int session = 1;
    SceUID fd = sceIoOpen(LOCAL_LOG_COUNTER_PATH, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[8];
        int n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        if (n > 0) {
            buf[n] = '\0';
            session = (int) strtoul(buf, NULL, 10) + 1;
        }
    }
    if (session < 1 || session > 999) session = 1;

    sceClibSnprintf(_localfile_path, sizeof(_localfile_path),
                     LOCAL_LOG_DIR "/debug_local_%03d.log", session);

    char counter_buf[8];
    int len = sceClibSnprintf(counter_buf, sizeof(counter_buf), "%d", session);
    fd = sceIoOpen(LOCAL_LOG_COUNTER_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, counter_buf, len);
        sceIoClose(fd);
    }

    _localfile_fd = sceIoOpen(_localfile_path,
                              SCE_O_WRONLY | SCE_O_APPEND | SCE_O_CREAT, 0777);
    if (_localfile_fd < 0)
        sceClibPrintf("logger: cannot open %s: 0x%x\n", _localfile_path, _localfile_fd);
}

static char _localfile_buffer[8192];
static size_t _localfile_used;

static void _localfile_flush(bool sync) {
    if (_localfile_fd < 0) return;
    size_t written = 0;
    while (written < _localfile_used) {
        int n = sceIoWrite(_localfile_fd, _localfile_buffer + written,
                           _localfile_used - written);
        if (n <= 0) {
            sceClibPrintf("logger: file write failed: 0x%x\n", n);
            break;
        }
        written += (size_t)n;
    }
    if (written) {
        _localfile_used -= written;
        memmove(_localfile_buffer, _localfile_buffer + written, _localfile_used);
    }
    if (sync) {
        int result = sceIoSyncByFd(_localfile_fd, 0);
        if (result < 0) sceClibPrintf("logger: file sync failed: 0x%x\n", result);
        _localfile_unsynced = 0;
    }
}

static void _localfile_send(const char *line, bool important) {
    if (!_localfile_init_done) _localfile_init();
    if (_localfile_fd < 0) return;
    size_t len = strlen(line);
    if (len > sizeof(_localfile_buffer) - _localfile_used)
        _localfile_flush(false);
    if (len <= sizeof(_localfile_buffer) - _localfile_used) {
        memcpy(_localfile_buffer + _localfile_used, line, len);
        _localfile_used += len;
    } else {
        sceClibPrintf("logger: line dropped after failed file flush\n");
    }
    if (important || ++_localfile_unsynced >= LOCAL_LOG_SYNC_EVERY)
        _localfile_flush(true);
}

static void _log_flush_due(SceUInt64 *flushed_at) {
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now - *flushed_at >= 1000000) {
        _localfile_flush(false);
        *flushed_at = now;
    }
}

static int _log_drain(SceSize argc, void *argv) {
    (void)argc;
    (void)argv;
    _debugnet_init();
    SceUInt64 flushed_at = sceKernelGetProcessTimeWide();
    bool running = true;
    while (running) {
        SceUInt timeout = 100000;
        sceKernelWaitSema(_log_wake, 1, &timeout);
        for (;;) {
            log_entry_t entry;
            sceKernelLockLwMutex(&_log_mutex, 1, NULL);
            if (!_log_count) {
                unsigned dropped = _log_dropped;
                _log_dropped = 0;
                running = atomic_load_explicit(&_log_running, memory_order_acquire);
                sceKernelUnlockLwMutex(&_log_mutex, 1);
                if (dropped) {
                    char notice[96];
                    sceClibSnprintf(notice, sizeof(notice),
                                    "logger: dropped %u low-severity lines (queue full)\n", dropped);
                    sceClibPrintf("%s", notice);
                    _debugnet_send(notice);
                    _localfile_send(notice, false);
                }
                break;
            }
            entry = _log_queue[_log_head];
            _log_head = (_log_head + 1) % LOG_QUEUE_SIZE;
            _log_count--;
            sceKernelUnlockLwMutex(&_log_mutex, 1);
            sceClibPrintf("%s", entry.line);
            _debugnet_send(entry.line);
            _localfile_send(entry.line, entry.important);
            atomic_store_explicit(&_log_completed, entry.sequence, memory_order_release);
            _log_flush_due(&flushed_at);
        }
        _log_flush_due(&flushed_at);
    }
    _localfile_flush(true);
    if (_localfile_fd >= 0) sceIoClose(_localfile_fd);
    if (_debugnet_sock >= 0) sceNetSocketClose(_debugnet_sock);
    return 0;
}

static void _log_shutdown(void) {
    if (atomic_load_explicit(&_log_state, memory_order_acquire) != 2) return;
    atomic_store_explicit(&_log_running, false, memory_order_release);
    sceKernelSignalSema(_log_wake, 1);
    SceUInt timeout = 2000000;
    if (sceKernelWaitThreadEnd(_log_thread, NULL, &timeout) >= 0)
        sceKernelDeleteThread(_log_thread);
    atomic_store_explicit(&_log_state, -1, memory_order_release);
}

static bool _log_init(void) {
    int state = atomic_load_explicit(&_log_state, memory_order_acquire);
    if (state == 2) return true;
    if (state < 0) return false;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&_log_state, &expected, 1,
                                                memory_order_acq_rel, memory_order_acquire)) {
        int result = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (result < 0) goto failed;
        _log_wake = sceKernelCreateSema("log_wake", 0, 0, 1, NULL);
        if (_log_wake < 0) goto failed;
        _log_thread = sceKernelCreateThread("log_drain", _log_drain, 191, 0x10000,
                                            0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
        if (_log_thread < 0) goto failed;
        atomic_store_explicit(&_log_running, true, memory_order_release);
        if (sceKernelStartThread(_log_thread, 0, NULL) < 0) goto failed;
        atexit(_log_shutdown);
        atomic_store_explicit(&_log_state, 2, memory_order_release);
        return true;
failed:
        atomic_store_explicit(&_log_running, false, memory_order_release);
        if (_log_thread >= 0) sceKernelDeleteThread(_log_thread);
        if (_log_wake >= 0) sceKernelDeleteSema(_log_wake);
        if (result >= 0) sceKernelDeleteLwMutex(&_log_mutex);
        sceClibPrintf("logger: background drain initialization failed\n");
        atomic_store_explicit(&_log_state, -1, memory_order_release);
        return false;
    }
    while (atomic_load_explicit(&_log_state, memory_order_acquire) == 1)
        sceKernelDelayThread(1000);
    return atomic_load_explicit(&_log_state, memory_order_acquire) == 2;
}

static void _log_emit(const char *line, bool important) {
    if (!_log_init()) {
        sceClibPrintf("%s", line);
        return;
    }
    unsigned sequence;
    for (;;) {
        sceKernelLockLwMutex(&_log_mutex, 1, NULL);
        if (!atomic_load_explicit(&_log_running, memory_order_acquire)) {
            sceKernelUnlockLwMutex(&_log_mutex, 1);
            return;
        }
        if (_log_count < LOG_QUEUE_SIZE) break;
        if (!important) {
            _log_dropped++;
            sceKernelUnlockLwMutex(&_log_mutex, 1);
            return;
        }
        sceKernelUnlockLwMutex(&_log_mutex, 1);
        sceKernelSignalSema(_log_wake, 1);
        sceKernelDelayThread(1000);
    }
    log_entry_t *entry = &_log_queue[(_log_head + _log_count) % LOG_QUEUE_SIZE];
    size_t len = strnlen(line, sizeof(entry->line) - 2);
    memcpy(entry->line, line, len);
    if (!len || line[len - 1] != '\n') entry->line[len++] = '\n';
    entry->line[len] = 0;
    entry->sequence = sequence = ++_log_sequence;
    entry->important = important;
    _log_count++;
    sceKernelUnlockLwMutex(&_log_mutex, 1);
    sceKernelSignalSema(_log_wake, 1);
    if (important) {
        /* Wait for the worker's write/sync attempt. Sink failures are reported
         * directly to the console; ordinary output only enqueues. */
        while ((int)(atomic_load_explicit(&_log_completed, memory_order_acquire) - sequence) < 0)
            sceKernelDelayThread(1000);
    }
}

void l_raw_line(const char *line) {
    _log_emit(line, false);
}

void _log_print(int t, const char* fmt, ...) {
    char buffer_a[2048], buffer_b[2048];
    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    _log_emit(buffer_b, t == LT_WARN || t == LT_ERROR || t == LT_FATAL);
}
