/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/texture_mip.h"

extern void port_trace(const char *format, ...);

static __thread IoPhysicalStats s_physical_stats;

void io_get_physical_stats(IoPhysicalStats *stats) {
    *stats = s_physical_stats;
}

static int profiled_pread(SceUID fd, void *data, SceSize count, SceOff offset) {
    uint64_t started = sceKernelGetSystemTimeWide();
    int result = sceIoPread(fd, data, count, offset);
    unsigned elapsed = (unsigned)(sceKernelGetSystemTimeWide() - started);
    s_physical_stats.pread_calls++;
    s_physical_stats.pread_us += elapsed;
    if (result > 0) s_physical_stats.pread_bytes += (unsigned)result;
    return result;
}

static FILE *profiled_fopen(const char *path, const char *mode) {
    uint64_t started = sceKernelGetSystemTimeWide();
#ifdef USE_SCELIBC_IO
    FILE *result = sceLibcBridge_fopen(path, mode);
#else
    FILE *result = fopen(path, mode);
#endif
    unsigned elapsed = (unsigned)(sceKernelGetSystemTimeWide() - started);
    s_physical_stats.fopen_calls++;
    s_physical_stats.fopen_us += elapsed;
    static unsigned slow_count;
    if (elapsed >= 50000 && __sync_add_and_fetch(&slow_count, 1) <= 8)
        port_trace("IO PHYSICAL: fopen thread=0x%x path=%s result=%p took=%u us",
                   (unsigned)sceKernelGetThreadId(), path, result, elapsed);
    return result;
}

static int profiled_stat(const char *path, struct stat *status, int follow_links) {
    uint64_t started = sceKernelGetSystemTimeWide();
    int result = follow_links ? stat(path, status) : lstat(path, status);
    unsigned elapsed = (unsigned)(sceKernelGetSystemTimeWide() - started);
    s_physical_stats.stat_calls++;
    s_physical_stats.stat_us += elapsed;
    static unsigned slow_count;
    if (elapsed >= 50000 && __sync_add_and_fetch(&slow_count, 1) <= 8)
        port_trace("IO PHYSICAL: %s thread=0x%x path=%s result=%d took=%u us",
                   follow_links ? "stat" : "lstat", (unsigned)sceKernelGetThreadId(), path, result, elapsed);
    return result;
}


#define IO_READ_DIAGNOSTIC_SLOTS 8
/* Rio performs two very different access patterns: a short sequential index
 * scan followed by thousands of small random asset seeks.  A 256 KiB stdio
 * buffer made every random miss read roughly 256 times more data than the
 * common 1 KiB request (about 20-23 ms per miss on Vita storage), which is
 * exactly the long pause observed at 99%.  One Vita storage page keeps the
 * sequential scan efficient without that read amplification or the large
 * per-stream memory cost. */
#define OBB_STDIO_BUFFER_SIZE (16 * 1024)
#define ENABLE_LOOSE_OBB_SHADOW_READS 0

typedef struct {
    volatile unsigned serial;
    volatile unsigned completed;
    void *destination;
    FILE *stream;
    size_t size;
    size_t count;
    size_t result;
    SceUID thread;
} IoReadDiagnostic;

static IoReadDiagnostic s_recent_reads[IO_READ_DIAGNOSTIC_SLOTS];
static volatile unsigned s_read_serial;

void io_dump_recent_reads(void) {
    unsigned newest = s_read_serial;
    port_trace("IO: recent fread snapshot newest=%u", newest);
    unsigned first = newest > IO_READ_DIAGNOSTIC_SLOTS
        ? newest - IO_READ_DIAGNOSTIC_SLOTS + 1 : 1;
    for (unsigned serial = first; serial <= newest; ++serial) {
        const IoReadDiagnostic *read =
            &s_recent_reads[(serial - 1) % IO_READ_DIAGNOSTIC_SLOTS];
        __sync_synchronize();
        if (read->serial != serial) continue;
        port_trace("IO: recent #%u thread=0x%08x f=%p dst=%p "
                   "size=%u count=%u result=%u completed=%u",
                   serial, (unsigned)read->thread, read->stream,
                   read->destination, (unsigned)read->size,
                   (unsigned)read->count, (unsigned)read->result,
                   read->completed);
    }
}

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

static int read_u32_be(FILE *stream, uint32_t *value) {
    unsigned char bytes[4];
    if (fread(bytes, 1, sizeof(bytes), stream) != sizeof(bytes)) {
        return 0;
    }

    *value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
             ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
    return 1;
}

static int make_packres_key(const char *path, char key[PATH_MAX]) {
    if (!path || !key) {
        return 0;
    }

    const char *packres = strstr(path, "_packRes/");
    if (!packres) {
        packres = strstr(path, "_packres/");
    }
    if (packres) {
        return snprintf(key, PATH_MAX, "%s", packres) < PATH_MAX;
    }

    const char *packetc = strstr(path, "_packEtc/");
    if (!packetc) {
        packetc = strstr(path, "_packetc/");
    }
    if (packetc) {
        return snprintf(key, PATH_MAX, "%s", packetc) < PATH_MAX;
    }

    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    return snprintf(key, PATH_MAX, "_packRes/%s", basename) < PATH_MAX;
}

static FILE *open_obb_cache(const char *key) {
    char cache_dir[PATH_MAX];
    char cache_path[PATH_MAX];
    uint32_t hash = 2166136261u;
    const unsigned char *cursor = (const unsigned char *)key;

    while (*cursor) {
        hash ^= *cursor++;
        hash *= 16777619u;
    }
    if (snprintf(cache_dir, sizeof(cache_dir), "%scache", DATA_PATH) >=
            (int)sizeof(cache_dir) ||
        snprintf(cache_path, sizeof(cache_path), "%s/packres-%08x.bin",
                 cache_dir, hash) >= (int)sizeof(cache_path)) {
        return NULL;
    }
    (void)mkdir(cache_dir, 0777);

    FILE *stream = fopen(cache_path, "wb+");
    if (!stream) {
        port_trace("IO: cannot create OBB cache file for %s", key);
    }
    return stream;
}

typedef struct {
    uint32_t offset;
    uint32_t size;
    const char *name;
    int8_t loose_cached; /* 0 = unprobed, 1 = exists, -1 = missing */
} ObbEntry;

typedef struct {
    char path[PATH_MAX];
    int loaded; /* 0 = uninitialized, 1 = loaded, -1 = missing/invalid */
    uint32_t entry_count;
    uint32_t data_offset;
    long long file_size;
    stat64_bionic file_stat;
    ObbEntry *entries;
    char *names_blob;
    uint32_t lookup_capacity;
    uint32_t *name_slots;
    uint32_t *basename_slots;
    SceUID raw_fd;
    unsigned char *prefix_cache;
    uint32_t prefix_size;
} CachedObb;

static CachedObb s_obb_archives[2];
static void ensure_obb_indexed(void);
static int is_obb_file_path(const char *path);
static const ObbEntry *find_entry_at_offset(const CachedObb *archive,
                                            uint32_t offset,
                                            size_t requested);
static int should_read_loose_entry(const ObbEntry *entry);

/* A SceLibc FILE owns cursor, buffering and locking state. Rio constructs and
 * destroys several thousand short-lived archive streams during a city load,
 * even though every stream points at one of the same two OBBs. On Vita that
 * bookkeeping is substantially more expensive than the small reads.
 *
 * Present a FILE-shaped opaque token to the Android binary, retain an
 * independent logical cursor per token, and service reads with sceIoPread on
 * one shared descriptor per archive. The small per-stream buffer preserves
 * stdio's efficient sequential 512/1024-byte scan without sharing cursors. */
#define VIRTUAL_OBB_STREAMS 96
#define VIRTUAL_OBB_BUFFER_SIZE (16 * 1024)
#define CITY_PAGE_CACHE_SLOTS 32
#define VIRTUAL_OBB_MAGIC 0x52494f46u

typedef struct {
    uint32_t magic;
    CachedObb *archive;
    int64_t position;
    int64_t buffer_offset;
    size_t buffer_valid;
    int eof_flag;
    int error_flag;
    int ungot;
    SceUID loose_fd;
    const ObbEntry *loose_entry;
    int loose_unavailable;
    unsigned generation;
    unsigned char buffer[VIRTUAL_OBB_BUFFER_SIZE];
} VirtualObbStream;

static VirtualObbStream s_virtual_obb[VIRTUAL_OBB_STREAMS];
static pthread_mutex_t s_virtual_obb_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    const ObbEntry *entry;
    uint32_t relative_offset;
    unsigned long long age;
    int loading;
    unsigned char data[VIRTUAL_OBB_BUFFER_SIZE];
} CityPage;

static CityPage s_city_pages[CITY_PAGE_CACHE_SLOTS];
static pthread_mutex_t s_city_page_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long s_city_page_age;
static volatile unsigned s_city_page_hits;
static volatile unsigned s_city_page_fills;
static pthread_mutex_t s_obb_index_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned s_virtual_obb_opens;
static volatile unsigned s_virtual_obb_reads;
static volatile unsigned s_virtual_obb_bytes;
static volatile unsigned s_virtual_obb_active;
static volatile unsigned s_virtual_obb_exhaustions;
static volatile unsigned s_prefix_cache_reads;
static volatile unsigned s_prefix_cache_bytes;
static volatile unsigned s_loose_read_count;
static volatile unsigned s_loose_read_bytes;

/* Worker prefetch and native reads share immutable asset ranges. In-flight
 * fills stay pinned; misses never wait for another reader while holding the
 * mutex. The byte budget covers headers and payloads together. */
#define TEXTURE_CACHE_SLOTS 256
#define TEXTURE_CACHE_BUDGET (16 * 1024 * 1024)
#define TEXTURE_CACHE_MAX_ENTRY (12 * 1024 * 1024)
typedef struct {
    const ObbEntry *entry;
    unsigned char *data;
    uint32_t offset;
    uint32_t size;
    int loading;
    unsigned long long age;
} TextureCache;

static TextureCache s_texture_cache[TEXTURE_CACHE_SLOTS];
static pthread_mutex_t s_texture_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long s_texture_cache_age;
static unsigned s_texture_cache_bytes;
static volatile unsigned s_texture_cache_hits;
static volatile unsigned s_texture_cache_fills;

static int is_cacheable_texture(const char *name) {
    if (!name || !strstr(name, "_packEtc/")) return 0;
    const char *extension = strrchr(name, '.');
    return extension && (strcasecmp(extension, ".tga") == 0 ||
                         strcasecmp(extension, ".bmp") == 0 ||
                         strcasecmp(extension, ".dds") == 0);
}

static int read_texture_cache(const ObbEntry *entry, SceUID fd,
                              uint32_t physical_base, uint32_t relative,
                              size_t requested, void *destination, int allow_fill) {
    if (!entry || !requested ||
        !is_cacheable_texture(entry->name) ||
        requested > TEXTURE_CACHE_MAX_ENTRY ||
        requested > TEXTURE_CACHE_BUDGET ||
        relative > entry->size || requested > entry->size - relative) {
        return 0;
    }

    uint64_t lock_started = sceKernelGetSystemTimeWide();
    pthread_mutex_lock(&s_texture_cache_lock);
    unsigned lock_elapsed = (unsigned)(sceKernelGetSystemTimeWide() - lock_started);
    s_physical_stats.cache_lock_calls++;
    s_physical_stats.cache_lock_us += lock_elapsed;
    int pending = 0;
    for (unsigned i = 0; i < TEXTURE_CACHE_SLOTS; ++i) {
        TextureCache *cached = &s_texture_cache[i];
        if (cached->entry != entry || !cached->data || relative < cached->offset ||
            requested > cached->size ||
            relative - cached->offset > cached->size - requested) continue;
        if (cached->loading) {
            pending = 1;
            continue;
        }
        if (destination)
            memcpy(destination, cached->data + relative - cached->offset, requested);
        cached->age = ++s_texture_cache_age;
        unsigned hit = __sync_add_and_fetch(&s_texture_cache_hits, 1);
        pthread_mutex_unlock(&s_texture_cache_lock);
        if (hit <= 12 || hit % 500 == 0) {
            port_trace("IO: texture cache hit #%u %s +%u bytes=%u",
                       hit, entry->name, relative, (unsigned)requested);
        }
        return 1;
    }
    if (pending || !allow_fill) {
        pthread_mutex_unlock(&s_texture_cache_lock);
        return 0;
    }

    unsigned victim;
    for (;;) {
        unsigned empty = TEXTURE_CACHE_SLOTS;
        unsigned oldest = TEXTURE_CACHE_SLOTS;
        for (unsigned i = 0; i < TEXTURE_CACHE_SLOTS; ++i) {
            TextureCache *cached = &s_texture_cache[i];
            if (!cached->data) {
                if (empty == TEXTURE_CACHE_SLOTS) empty = i;
            } else if (!cached->loading &&
                       (oldest == TEXTURE_CACHE_SLOTS ||
                        cached->age < s_texture_cache[oldest].age)) {
                oldest = i;
            }
        }
        if (empty < TEXTURE_CACHE_SLOTS &&
            s_texture_cache_bytes <=
                TEXTURE_CACHE_BUDGET - requested) {
            victim = empty;
            break;
        }
        if (oldest == TEXTURE_CACHE_SLOTS) {
            pthread_mutex_unlock(&s_texture_cache_lock);
            return 0;
        }
        TextureCache *evicted = &s_texture_cache[oldest];
        free(evicted->data);
        s_texture_cache_bytes -= evicted->size;
        memset(evicted, 0, sizeof(*evicted));
    }

    unsigned char *data = (unsigned char *)malloc(requested);
    if (!data) {
        pthread_mutex_unlock(&s_texture_cache_lock);
        return 0;
    }
    /* Reserve both the slot and its bytes before dropping the lock. Other
     * readers may evict completed entries, but cannot reuse an in-flight fill. */
    TextureCache *cached = &s_texture_cache[victim];
    cached->entry = entry;
    cached->data = data;
    cached->offset = relative;
    cached->size = (uint32_t)requested;
    cached->loading = 1;
    s_texture_cache_bytes += cached->size;
    pthread_mutex_unlock(&s_texture_cache_lock);

    int result = profiled_pread(fd, data, (SceSize)requested, (SceOff)physical_base + relative);
    if (result != (int)requested) {
        pthread_mutex_lock(&s_texture_cache_lock);
        s_texture_cache_bytes -= cached->size;
        memset(cached, 0, sizeof(*cached));
        pthread_mutex_unlock(&s_texture_cache_lock);
        free(data);
        return 0;
    }

    if (destination) memcpy(destination, data, requested);
    pthread_mutex_lock(&s_texture_cache_lock);
    cached->age = ++s_texture_cache_age;
    cached->loading = 0;
    unsigned resident = s_texture_cache_bytes;
    unsigned fill = __sync_add_and_fetch(&s_texture_cache_fills, 1);
    pthread_mutex_unlock(&s_texture_cache_lock);
    if (fill <= 12 || fill % 100 == 0) {
        port_trace("IO: texture cache fill #%u %s +%u size=%u resident=%u/%u",
                   fill, entry->name, relative, (unsigned)requested,
                   resident,
                   TEXTURE_CACHE_BUDGET);
    }
    return 1;
}

static VirtualObbStream *virtual_obb_from_file(FILE *stream) {
    uintptr_t address = (uintptr_t)stream;
    uintptr_t begin = (uintptr_t)&s_virtual_obb[0];
    uintptr_t end = (uintptr_t)&s_virtual_obb[VIRTUAL_OBB_STREAMS];
    if (address < begin || address >= end ||
        ((address - begin) % sizeof(VirtualObbStream)) != 0) return NULL;
    VirtualObbStream *virtual_stream = (VirtualObbStream *)stream;
    return virtual_stream->magic == VIRTUAL_OBB_MAGIC ? virtual_stream : NULL;
}

static CachedObb *find_cached_obb(const char *path) {
    if (!is_obb_file_path(path)) return NULL;
    ensure_obb_indexed();
    for (unsigned i = 0; i < 2; ++i) {
        if (s_obb_archives[i].loaded == 1 &&
            strcmp(s_obb_archives[i].path, path) == 0)
            return &s_obb_archives[i];
    }
    return NULL;
}

static int stat_cached_obb(const char *path, stat64_bionic *status) {
    CachedObb *archive = find_cached_obb(path);
    if (!archive || !S_ISREG(archive->file_stat.st_mode)) return -1;
    *status = archive->file_stat;
    static unsigned hits;
    unsigned call = __sync_add_and_fetch(&hits, 1);
    if (call <= 8 || call % 500 == 0)
        port_trace("IO: cached OBB stat #%u path=%s size=%lld", call,
                   path, (long long)status->st_size);
    return 0;
}

static VirtualObbStream *open_virtual_obb_stream(const char *path) {
    CachedObb *archive = find_cached_obb(path);
    if (!archive || archive->raw_fd < 0) return NULL;

    VirtualObbStream *result = NULL;
    pthread_mutex_lock(&s_virtual_obb_lock);
    for (unsigned i = 0; i < VIRTUAL_OBB_STREAMS; ++i) {
        VirtualObbStream *candidate = &s_virtual_obb[i];
        if (candidate->magic != VIRTUAL_OBB_MAGIC) {
            unsigned generation = candidate->generation + 1;
            memset(candidate, 0, sizeof(*candidate));
            candidate->magic = VIRTUAL_OBB_MAGIC;
            candidate->archive = archive;
            candidate->buffer_offset = -1;
            candidate->ungot = EOF;
            candidate->loose_fd = -1;
            candidate->generation = generation;
            result = candidate;
            __sync_add_and_fetch(&s_virtual_obb_active, 1);
            break;
        }
    }
    pthread_mutex_unlock(&s_virtual_obb_lock);

    if (result) {
        unsigned call = __sync_add_and_fetch(&s_virtual_obb_opens, 1);
        if (call <= 20 || call % 500 == 0) {
            port_trace("IO: virtual OBB open #%u stream=%p fd=%d path=%s",
                       call, result, archive->raw_fd, path);
        }
    } else {
        __sync_add_and_fetch(&s_virtual_obb_exhaustions, 1);
        port_trace("IO: virtual OBB stream pool exhausted for %s", path);
    }
    return result;
}

static int close_virtual_obb_stream(VirtualObbStream *stream) {
    if (!stream) return 0;
    pthread_mutex_lock(&s_virtual_obb_lock);
    if (stream->loose_fd >= 0) {
        sceIoClose(stream->loose_fd);
        stream->loose_fd = -1;
    }
    stream->magic = 0;
    stream->archive = NULL;
    stream->buffer_valid = 0;
    stream->ungot = EOF;
    __sync_sub_and_fetch(&s_virtual_obb_active, 1);
    pthread_mutex_unlock(&s_virtual_obb_lock);
    return 1;
}

static int read_city_page(CachedObb *archive, const ObbEntry *entry,
                          uint32_t position, unsigned char *destination,
                          int64_t *buffer_offset) {
    uint32_t relative = position - entry->offset;
    uint32_t page_relative = relative & ~(uint32_t)(VIRTUAL_OBB_BUFFER_SIZE - 1);
    size_t page_size = entry->size - page_relative;
    if (page_size > VIRTUAL_OBB_BUFFER_SIZE)
        page_size = VIRTUAL_OBB_BUFFER_SIZE;

    pthread_mutex_lock(&s_city_page_lock);
    CityPage *victim = NULL;
    for (unsigned i = 0; i < CITY_PAGE_CACHE_SLOTS; ++i) {
        CityPage *page = &s_city_pages[i];
        if (page->entry == entry && page->relative_offset == page_relative) {
            if (page->loading) {
                pthread_mutex_unlock(&s_city_page_lock);
                return 0;
            }
            memcpy(destination, page->data, page_size);
            page->age = ++s_city_page_age;
            pthread_mutex_unlock(&s_city_page_lock);
            unsigned hits = __sync_add_and_fetch(&s_city_page_hits, 1);
            if (hits <= 8 || hits % 512 == 0)
                port_trace("IO: city page cache hit #%u offset=%u", hits,
                           page_relative);
            *buffer_offset = (int64_t)entry->offset + page_relative;
            return (int)page_size;
        }
        if (!page->loading && (!victim || !page->entry ||
            (victim->entry && page->age < victim->age))) {
            victim = page;
        }
    }
    if (!victim) {
        pthread_mutex_unlock(&s_city_page_lock);
        return 0;
    }
    victim->entry = entry;
    victim->relative_offset = page_relative;
    victim->loading = 1;
    pthread_mutex_unlock(&s_city_page_lock);

    int result = profiled_pread(archive->raw_fd, victim->data,
                                (SceSize)page_size,
                                (SceOff)entry->offset + page_relative);
    pthread_mutex_lock(&s_city_page_lock);
    if (result == (int)page_size) {
        memcpy(destination, victim->data, page_size);
        victim->age = ++s_city_page_age;
        victim->loading = 0;
        pthread_mutex_unlock(&s_city_page_lock);
        unsigned fills = __sync_add_and_fetch(&s_city_page_fills, 1);
        if (fills <= 8 || fills % 512 == 0)
            port_trace("IO: city page cache fill #%u offset=%u", fills,
                       page_relative);
        *buffer_offset = (int64_t)entry->offset + page_relative;
        return result;
    }
    victim->entry = NULL;
    victim->loading = 0;
    pthread_mutex_unlock(&s_city_page_lock);
    return result < 0 ? result : 0;
}

static SceUID open_raw_loose_entry(const ObbEntry *entry) {
    ObbEntry *mutable_entry = (ObbEntry *)entry;
    if (mutable_entry->loose_cached == -1) return -1;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s%s", DATA_PATH, entry->name) >=
        (int)sizeof(path)) return -1;
    if (mutable_entry->loose_cached == 0) {
        SceIoStat status;
        memset(&status, 0, sizeof(status));
        if (sceIoGetstat(path, &status) < 0 ||
            (uint64_t)status.st_size != entry->size) {
            mutable_entry->loose_cached = -1;
            return -1;
        }
        mutable_entry->loose_cached = 1;
    }
    return sceIoOpen(path, SCE_O_RDONLY, 0);
}

static int open_virtual_loose_entry(VirtualObbStream *stream,
                                    const ObbEntry *entry) {
    if (stream->loose_entry == entry)
        return stream->loose_fd >= 0 && !stream->loose_unavailable;
    if (stream->loose_fd >= 0) sceIoClose(stream->loose_fd);
    stream->loose_entry = entry;
    stream->loose_fd = open_raw_loose_entry(entry);
    stream->loose_unavailable = stream->loose_fd < 0;
    return !stream->loose_unavailable;
}

static size_t read_virtual_obb(VirtualObbStream *stream, void *destination,
                               size_t requested) {
    if (!stream || !destination || !requested || !stream->archive) return 0;
    unsigned char *output = (unsigned char *)destination;
    int64_t start_position = stream->position;
    size_t completed = 0;

    if (stream->ungot != EOF && requested) {
        output[completed++] = (unsigned char)stream->ungot;
        stream->ungot = EOF;
        ++stream->position;
    }

    while (completed < requested &&
           stream->position < stream->archive->file_size) {
        size_t remaining = requested - completed;

        /* Purple reopens an OBB for nearly every asset and rereads the same
         * directory. In 0.42 this became thousands of physical 1 KiB reads
         * during the 99% pause. The immutable directory is only about
         * 350 KiB, so serve it from one archive-wide memory copy. */
        if (stream->archive->prefix_cache &&
            stream->position < stream->archive->prefix_size) {
            size_t available = stream->archive->prefix_size -
                               (size_t)stream->position;
            size_t chunk = remaining < available ? remaining : available;
            memcpy(output + completed,
                   stream->archive->prefix_cache + stream->position, chunk);
            completed += chunk;
            stream->position += (int64_t)chunk;
            unsigned hit = __sync_add_and_fetch(&s_prefix_cache_reads, 1);
            __sync_add_and_fetch(&s_prefix_cache_bytes, (unsigned)chunk);
            if (hit <= 12 || hit % 5000 == 0) {
                port_trace("IO: OBB prefix cache hit #%u bytes=%u path=%s",
                           hit, (unsigned)chunk, stream->archive->path);
            }
            continue;
        }

        /* Large texture payloads can use the installed unpacked cache. A
         * private raw descriptor plus pread avoids the FILE locking and
         * shared-cursor contention that made the old loose-file path slower. */
        const ObbEntry *entry = NULL;
        if (stream->position <= UINT32_MAX) {
            entry = find_entry_at_offset(stream->archive,
                                         (uint32_t)stream->position,
                                         remaining);
        }
        if (entry && read_texture_cache(entry, -1, 0,
                (uint32_t)stream->position - entry->offset, remaining,
                output + completed, 0)) {
            completed += remaining;
            stream->position += (int64_t)remaining;
            continue;
        }
        if (remaining >= 64 * 1024 && entry && should_read_loose_entry(entry) &&
            open_virtual_loose_entry(stream, entry)) {
            uint32_t relative = (uint32_t)stream->position - entry->offset;
            if (read_texture_cache(entry, stream->loose_fd, 0, relative,
                                   remaining, output + completed, 1)) {
                completed += remaining;
                stream->position += (int64_t)remaining;
                continue;
            }
            int result = profiled_pread(stream->loose_fd, output + completed,
                                    (SceSize)remaining, (SceOff)relative);
            if (result > 0) {
                completed += (size_t)result;
                stream->position += result;
                unsigned loose_call = __sync_add_and_fetch(
                    &s_loose_read_count, 1);
                unsigned loose_total = __sync_add_and_fetch(
                    &s_loose_read_bytes, (unsigned)result);
                if (loose_call <= 20 || loose_call % 500 == 0 ||
                    remaining >= 1024 * 1024) {
                    port_trace("IO: virtual loose read #%u %s +%u "
                               "bytes=%u total=%u",
                               loose_call, entry->name, relative,
                               (unsigned)result, loose_total);
                }
                continue;
            }
            stream->loose_unavailable = 1;
            sceIoClose(stream->loose_fd);
            stream->loose_fd = -1;
        }

        if (remaining >= VIRTUAL_OBB_BUFFER_SIZE) {
            int result = profiled_pread(stream->archive->raw_fd,
                                    output + completed,
                                    (SceSize)remaining,
                                    (SceOff)stream->position);
            if (result < 0) {
                stream->error_flag = 1;
                break;
            }
            if (result == 0) break;
            completed += (size_t)result;
            stream->position += result;
            if ((size_t)result < remaining) break;
            continue;
        }

        int64_t buffer_end = stream->buffer_offset +
                             (int64_t)stream->buffer_valid;
        if (stream->buffer_offset < 0 ||
            stream->position < stream->buffer_offset ||
            stream->position >= buffer_end) {
            int city_entry = entry &&
                !strcasecmp(entry->name, "_packRes/lowmem/city.bdae");
            int64_t read_offset = stream->position;
            int result = city_entry
                ? read_city_page(stream->archive, entry,
                                 (uint32_t)stream->position,
                                 stream->buffer, &read_offset) : 0;
            if (result == 0) {
                result = profiled_pread(stream->archive->raw_fd,
                                        stream->buffer,
                                        VIRTUAL_OBB_BUFFER_SIZE,
                                        (SceOff)stream->position);
            }
            if (result < 0) {
                stream->error_flag = 1;
                break;
            }
            stream->buffer_offset = read_offset;
            stream->buffer_valid = result > 0 ? (size_t)result : 0;
            buffer_end = stream->buffer_offset +
                         (int64_t)stream->buffer_valid;
            if (!stream->buffer_valid) break;
        }

        size_t available = (size_t)(buffer_end - stream->position);
        size_t chunk = remaining < available ? remaining : available;
        memcpy(output + completed,
               stream->buffer + (size_t)(stream->position -
                                          stream->buffer_offset), chunk);
        completed += chunk;
        stream->position += (int64_t)chunk;
    }

    if (completed < requested) stream->eof_flag = 1;
    unsigned call = __sync_add_and_fetch(&s_virtual_obb_reads, 1);
    unsigned total = __sync_add_and_fetch(&s_virtual_obb_bytes,
                                          (unsigned)completed);
    if (call <= 20 || call % 5000 == 0 || requested >= 1024 * 1024) {
        const ObbEntry *trace_entry = NULL;
        if (start_position >= 0 && start_position <= UINT32_MAX) {
            trace_entry = find_entry_at_offset(stream->archive,
                                                (uint32_t)start_position,
                                                requested);
        }
        port_trace("IO: virtual OBB read #%u stream=%p entry=%s offset=%u "
                   "requested=%u got=%u total=%u",
                   call, stream, trace_entry ? trace_entry->name : "<mixed>",
                   (unsigned)(start_position >= 0 ? start_position : 0),
                   (unsigned)requested, (unsigned)completed, total);
    }
    return completed;
}

void io_get_virtual_obb_stats(unsigned *opens, unsigned *reads,
                              unsigned *bytes, unsigned *active,
                              unsigned *exhaustions) {
    if (opens) *opens = __atomic_load_n(&s_virtual_obb_opens,
                                        __ATOMIC_RELAXED);
    if (reads) *reads = __atomic_load_n(&s_virtual_obb_reads,
                                        __ATOMIC_RELAXED);
    if (bytes) *bytes = __atomic_load_n(&s_virtual_obb_bytes,
                                        __ATOMIC_RELAXED);
    if (active) *active = __atomic_load_n(&s_virtual_obb_active,
                                          __ATOMIC_RELAXED);
    if (exhaustions) *exhaustions = __atomic_load_n(
        &s_virtual_obb_exhaustions, __ATOMIC_RELAXED);
}

/* Rio's own virtual filesystem opens the multi-gigabyte OBB for every asset
 * and then seeks to the indexed byte range.  Keep the archive FILE alive for
 * its header/index semantics, but serve large texture ranges from the already
 * extracted files.  Models, audio and metadata are deliberately left in the
 * OBB: the engine interleaves their small reads, and repeatedly opening loose
 * files on Vita's FAT filesystem is hundreds of times slower than seeking in
 * the already-open archive. */
#define ACTIVE_OBB_STREAMS 64
typedef struct {
    FILE *stream;
    CachedObb *archive;
    const ObbEntry *loose_entry;
    FILE *loose_stream;
    long loose_position;
    unsigned references;
    int retired;
} ActiveObbStream;

static ActiveObbStream s_active_obb[ACTIVE_OBB_STREAMS];
static pthread_mutex_t s_active_obb_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t obb_name_hash(const char *name) {
    uint32_t hash = 2166136261u;
    while (name && *name) {
        unsigned char ch = (unsigned char)*name++;
        if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch + ('a' - 'A'));
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash;
}

static const char *obb_basename(const char *name) {
    const char *slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

static void obb_hash_insert(uint32_t *slots, uint32_t capacity,
                            const ObbEntry *entries, uint32_t entry_index,
                            int basename_only) {
    const char *name = basename_only ? obb_basename(entries[entry_index].name)
                                     : entries[entry_index].name;
    uint32_t position = obb_name_hash(name) & (capacity - 1);
    while (slots[position]) {
        const ObbEntry *existing = &entries[slots[position] - 1];
        const char *existing_name = basename_only
            ? obb_basename(existing->name) : existing->name;
        if (strcasecmp(existing_name, name) == 0) {
            /* Preserve the first entry, matching the old linear scan. */
            return;
        }
        position = (position + 1) & (capacity - 1);
    }
    slots[position] = entry_index + 1;
}

static const ObbEntry *obb_hash_lookup(const CachedObb *archive,
                                       const uint32_t *slots,
                                       const char *name,
                                       int basename_only) {
    if (!slots || !archive->lookup_capacity) return NULL;
    uint32_t position = obb_name_hash(name) & (archive->lookup_capacity - 1);
    for (uint32_t probes = 0; probes < archive->lookup_capacity; ++probes) {
        uint32_t slot = slots[position];
        if (!slot) return NULL;
        const ObbEntry *entry = &archive->entries[slot - 1];
        const char *entry_name = basename_only
            ? obb_basename(entry->name) : entry->name;
        if (strcasecmp(entry_name, name) == 0) return entry;
        position = (position + 1) & (archive->lookup_capacity - 1);
    }
    return NULL;
}

static void init_single_obb_index(CachedObb *c, const char *obb_path) {
    if (c->loaded != 0) {
        return;
    }
    c->raw_fd = -1;
    snprintf(c->path, sizeof(c->path), "%s", obb_path);

    FILE *obb = fopen(obb_path, "rb");
    if (!obb) {
        c->loaded = -1;
        return;
    }

    uint32_t unknown = 0;
    uint32_t data_offset = 0;
    uint32_t names_offset = 0;
    uint32_t entry_count = 0;

    if (!read_u32_be(obb, &unknown) ||
        !read_u32_be(obb, &data_offset) ||
        !read_u32_be(obb, &names_offset) ||
        !read_u32_be(obb, &entry_count) ||
        entry_count == 0) {
        fclose(obb);
        c->loaded = -1;
        return;
    }

    if ((unsigned long long)names_offset !=
        16ULL + (unsigned long long)entry_count * 16ULL) {
        fclose(obb);
        c->loaded = -1;
        return;
    }

    if (fseek(obb, 0, SEEK_END) != 0) {
        fclose(obb);
        c->loaded = -1;
        return;
    }
    long long file_size = ftell(obb);
    if (file_size < 0 || (long long)names_offset > file_size ||
        file_size < (long long)data_offset) {
        fclose(obb);
        c->loaded = -1;
        return;
    }

    uint32_t table_bytes = entry_count * 16;
    uint32_t names_bytes = data_offset - names_offset;

    uint32_t *table_buf = (uint32_t *)malloc(table_bytes);
    char *names_blob = (char *)malloc(names_bytes + 1);
    ObbEntry *entries = (ObbEntry *)malloc(entry_count * sizeof(ObbEntry));

    if (!table_buf || !names_blob || !entries) {
        free(table_buf);
        free(names_blob);
        free(entries);
        fclose(obb);
        c->loaded = -1;
        return;
    }

    if (fseek(obb, 16, SEEK_SET) != 0 ||
        fread(table_buf, 1, table_bytes, obb) != table_bytes ||
        fseek(obb, names_offset, SEEK_SET) != 0 ||
        fread(names_blob, 1, names_bytes, obb) != names_bytes) {
        free(table_buf);
        free(names_blob);
        free(entries);
        fclose(obb);
        c->loaded = -1;
        return;
    }
    names_blob[names_bytes] = '\0';
    fclose(obb);

    char *name_ptr = names_blob;
    char *names_end = names_blob + names_bytes;

    for (uint32_t i = 0; i < entry_count; ++i) {
        uint32_t raw_off = table_buf[i * 4 + 0];
        uint32_t raw_sz  = table_buf[i * 4 + 1];
        entries[i].offset = ((raw_off >> 24) & 0xff) |
                            ((raw_off >> 8) & 0xff00) |
                            ((raw_off << 8) & 0xff0000) |
                            ((raw_off << 24) & 0xff000000);
        entries[i].size   = ((raw_sz >> 24) & 0xff) |
                            ((raw_sz >> 8) & 0xff00) |
                            ((raw_sz << 8) & 0xff0000) |
                            ((raw_sz << 24) & 0xff000000);
        if (name_ptr < names_end) {
            entries[i].name = name_ptr;
            name_ptr += strlen(name_ptr) + 1;
        } else {
            entries[i].name = "";
        }
        entries[i].loose_cached = 0;
    }

    free(table_buf);

    uint32_t lookup_capacity = 1;
    while (lookup_capacity < entry_count * 2u) lookup_capacity <<= 1;
    uint32_t *name_slots = (uint32_t *)calloc(lookup_capacity,
                                               sizeof(uint32_t));
    uint32_t *basename_slots = (uint32_t *)calloc(lookup_capacity,
                                                   sizeof(uint32_t));
    if (name_slots && basename_slots) {
        for (uint32_t i = 0; i < entry_count; ++i) {
            obb_hash_insert(name_slots, lookup_capacity, entries, i, 0);
            obb_hash_insert(basename_slots, lookup_capacity, entries, i, 1);
        }
        c->lookup_capacity = lookup_capacity;
        c->name_slots = name_slots;
        c->basename_slots = basename_slots;
    } else {
        free(name_slots);
        free(basename_slots);
    }

    c->entries = entries;
    c->names_blob = names_blob;
    c->entry_count = entry_count;
    c->data_offset = data_offset;
    c->file_size = file_size;
    c->raw_fd = sceIoOpen(obb_path, SCE_O_RDONLY, 0);
    if (c->raw_fd < 0) {
        port_trace("IO: raw OBB descriptor unavailable path=%s error=0x%08x",
                   obb_path, (unsigned)c->raw_fd);
    }
    if (c->raw_fd >= 0 && data_offset > 0 && data_offset <= 1024 * 1024) {
        c->prefix_cache = (unsigned char *)malloc(data_offset);
        if (c->prefix_cache) {
            int prefix_read = sceIoPread(c->raw_fd, c->prefix_cache,
                                         data_offset, 0);
            if (prefix_read == (int)data_offset) {
                c->prefix_size = data_offset;
            } else {
                free(c->prefix_cache);
                c->prefix_cache = NULL;
            }
        }
    }
    struct stat metadata;
    if (lstat(obb_path, &metadata) == 0 && S_ISREG(metadata.st_mode)) {
        stat_newlib_to_bionic(&metadata, &c->file_stat);
        c->file_stat.st_size = c->file_size;
    }
    c->loaded = 1;
    port_trace("IO: Indexed OBB %s (%u entries, hash=%u raw_fd=%d "
               "prefix=%u/%u)", obb_path, entry_count, c->lookup_capacity,
               c->raw_fd, c->prefix_size, data_offset);
}

static const ObbEntry *find_entry_in_obb(const CachedObb *c, const char *key) {
    if (!c || c->loaded != 1) {
        return NULL;
    }

    const ObbEntry *hashed = obb_hash_lookup(c, c->name_slots, key, 0);
    if (hashed) return hashed;

    const char *key_base = obb_basename(key);
    hashed = obb_hash_lookup(c, c->basename_slots, key_base, 1);
    if (hashed) return hashed;

    /* Allocation failure fallback: preserve the old lookup behavior. */
    if (c->name_slots && c->basename_slots) return NULL;

    /* 1. Exact match */
    for (uint32_t i = 0; i < c->entry_count; ++i) {
        if (strcasecmp(c->entries[i].name, key) == 0) {
            return &c->entries[i];
        }
    }

    /* 2. Basename match */
    for (uint32_t i = 0; i < c->entry_count; ++i) {
        const char *ent_base = obb_basename(c->entries[i].name);
        if (strcasecmp(ent_base, key_base) == 0) {
            return &c->entries[i];
        }
    }

    return NULL;
}

static void ensure_obb_indexed(void) {
    char obb_root[PATH_MAX];
    static const char *const priority_archives[] = {
        "patch.12221.com.gameloft.android.ANMP.GloftG4HM.obb",
        "main.12030.com.gameloft.android.ANMP.GloftG4HM.obb",
    };
    if (snprintf(obb_root, PATH_MAX, "%sobb", DATA_PATH) >= PATH_MAX) {
        return;
    }
    pthread_mutex_lock(&s_obb_index_lock);
    for (unsigned int i = 0; i < 2; ++i) {
        if (s_obb_archives[i].loaded == 0) {
            char obb_path[PATH_MAX];
            if (snprintf(obb_path, PATH_MAX, "%s/%s", obb_root, priority_archives[i]) < PATH_MAX) {
                init_single_obb_index(&s_obb_archives[i], obb_path);
            }
        }
    }
    pthread_mutex_unlock(&s_obb_index_lock);
}

static const ObbEntry *find_entry_in_any_obb(const char *key,
                                             CachedObb **archive_out) {
    ensure_obb_indexed();
    for (unsigned int i = 0; i < 2; ++i) {
        const ObbEntry *entry = find_entry_in_obb(&s_obb_archives[i], key);
        if (entry) {
            if (archive_out) *archive_out = &s_obb_archives[i];
            return entry;
        }
    }
    if (archive_out) *archive_out = NULL;
    return NULL;
}

unsigned io_prefetch_texture(const char *name, unsigned max_dimension) {
    if (!name || !*name) return 0;
    CachedObb *archive = NULL;
    const ObbEntry *entry = find_entry_in_any_obb(name, &archive);
    if (!entry || !archive || archive->raw_fd < 0 || entry->size < 128 ||
        !is_cacheable_texture(entry->name)) return 0;

    SceUID loose = should_read_loose_entry(entry) ? open_raw_loose_entry(entry) : -1;
    SceUID fd = loose >= 0 ? loose : archive->raw_fd;
    uint32_t base = loose >= 0 ? 0 : entry->offset;
    if (max_dimension == UINT32_MAX) {
        unsigned bytes = read_texture_cache(entry, fd, base, 0,
                                            entry->size, NULL, 1)
            ? entry->size : 0;
        if (loose >= 0) sceIoClose(loose);
        return bytes;
    }
    uint32_t header[256];
    unsigned header_size = entry->size < sizeof(header) ? entry->size : sizeof(header);
    unsigned offset = 0, size = 0, prefetched = 0;
    if (read_texture_cache(entry, fd, base, 0, header_size, header, 1) &&
        texture_dds_payload_range(header, entry->size, max_dimension, &offset, &size) &&
        read_texture_cache(entry, fd, base, offset, size, NULL, 1))
        prefetched = size;
    if (loose >= 0) sceIoClose(loose);
    return prefetched;
}

static int has_packaged_prefix(const char *path) {
    if (!path) return 0;
    for (const char *cursor = path; *cursor; ++cursor) {
        if (strncasecmp(cursor, "_packres/", 9) == 0 ||
            strncasecmp(cursor, "_packetc/", 9) == 0) return 1;
    }
    return 0;
}

static int packaged_path_looks_like_file(const char *path) {
    const char *base = obb_basename(path);
    return base && strchr(base, '.') != NULL;
}

static int is_obb_file_path(const char *path) {
    size_t length = path ? strlen(path) : 0;
    return length >= 4 && strcasecmp(path + length - 4, ".obb") == 0;
}

/* The Android VFS opens and closes the same archive once per asset. On Vita
 * that repeatedly allocates a SceLibc FILE, a stdio buffer and synchronization
 * state; the city load performs thousands of these cycles. Keep a small set of
 * independent read-only handles and return an idle one at the next fopen. A
 * handle is never shared while in use, so each caller retains its own cursor. */
#define OBB_HANDLE_CACHE_SIZE ACTIVE_OBB_STREAMS
typedef struct {
    FILE *stream;
    char path[PATH_MAX];
    int in_use;
} CachedObbHandle;

static CachedObbHandle s_obb_handle_cache[OBB_HANDLE_CACHE_SIZE];
static pthread_mutex_t s_obb_handle_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned s_obb_handle_reuses;

static int raw_obb_seek_start(FILE *stream) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fseek(stream, 0, SEEK_SET);
#else
    return fseek(stream, 0, SEEK_SET);
#endif
}

static FILE *take_cached_obb_handle(const char *path) {
    FILE *stream = NULL;
    unsigned slot = OBB_HANDLE_CACHE_SIZE;

    pthread_mutex_lock(&s_obb_handle_cache_lock);
    for (unsigned i = 0; i < OBB_HANDLE_CACHE_SIZE; ++i) {
        CachedObbHandle *cached = &s_obb_handle_cache[i];
        if (cached->stream && !cached->in_use &&
            strcmp(cached->path, path) == 0) {
            cached->in_use = 1;
            stream = cached->stream;
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&s_obb_handle_cache_lock);

    if (!stream) return NULL;
    if (raw_obb_seek_start(stream) != 0) {
        pthread_mutex_lock(&s_obb_handle_cache_lock);
        memset(&s_obb_handle_cache[slot], 0,
               sizeof(s_obb_handle_cache[slot]));
        pthread_mutex_unlock(&s_obb_handle_cache_lock);
#ifdef USE_SCELIBC_IO
        sceLibcBridge_fclose(stream);
#else
        fclose(stream);
#endif
        port_trace("IO: discarded cached OBB handle after failed rewind %p",
                   stream);
        return NULL;
    }

    unsigned reuse = __sync_add_and_fetch(&s_obb_handle_reuses, 1);
    if (reuse <= 20 || reuse % 500 == 0) {
        port_trace("IO: reused OBB handle #%u slot=%u f=%p path=%s",
                   reuse, slot, stream, path);
    }
    return stream;
}

static int adopt_cached_obb_handle(FILE *stream, const char *path) {
    int adopted = 0;
    pthread_mutex_lock(&s_obb_handle_cache_lock);
    for (unsigned i = 0; i < OBB_HANDLE_CACHE_SIZE; ++i) {
        CachedObbHandle *cached = &s_obb_handle_cache[i];
        if (!cached->stream) {
            cached->stream = stream;
            cached->in_use = 1;
            snprintf(cached->path, sizeof(cached->path), "%s", path);
            adopted = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_obb_handle_cache_lock);
    return adopted;
}

static int release_cached_obb_handle(FILE *stream) {
    int released = 0;
    pthread_mutex_lock(&s_obb_handle_cache_lock);
    for (unsigned i = 0; i < OBB_HANDLE_CACHE_SIZE; ++i) {
        CachedObbHandle *cached = &s_obb_handle_cache[i];
        if (cached->stream == stream) {
            cached->in_use = 0;
            released = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_obb_handle_cache_lock);
    return released;
}

static void configure_obb_stdio_buffer(FILE *stream, const char *path) {
    if (!stream || !is_obb_file_path(path)) return;
#ifdef USE_SCELIBC_IO
    int result = sceLibcBridge_setvbuf(stream, NULL, _IOFBF,
                                      OBB_STDIO_BUFFER_SIZE);
#else
    int result = setvbuf(stream, NULL, _IOFBF, OBB_STDIO_BUFFER_SIZE);
#endif
    static unsigned configured_count;
    unsigned call = __sync_add_and_fetch(&configured_count, 1);
    if (call <= 10 || result != 0) {
        port_trace("IO: OBB stdio buffer #%u size=%u result=%d path=%s",
                   call, (unsigned)OBB_STDIO_BUFFER_SIZE, result, path);
    }
}

static const ObbEntry *find_entry_at_offset(const CachedObb *archive,
                                            uint32_t offset,
                                            size_t requested) {
    if (!archive || archive->loaded != 1 || !archive->entry_count) return NULL;

    uint32_t lo = 0;
    uint32_t hi = archive->entry_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (archive->entries[mid].offset <= offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo) return NULL;

    const ObbEntry *entry = &archive->entries[lo - 1];
    uint64_t begin = entry->offset;
    uint64_t end = begin + entry->size;
    uint64_t read_end = (uint64_t)offset + requested;
    return offset >= begin && read_end >= offset && read_end <= end
        ? entry : NULL;
}

static void register_obb_stream(FILE *stream, const char *path) {
    if (!stream || !path) return;
    ensure_obb_indexed();

    CachedObb *archive = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        if (s_obb_archives[i].loaded == 1 &&
            strcmp(s_obb_archives[i].path, path) == 0) {
            archive = &s_obb_archives[i];
            break;
        }
    }
    if (!archive) return;

    pthread_mutex_lock(&s_active_obb_lock);
    for (unsigned i = 0; i < ACTIVE_OBB_STREAMS; ++i) {
        if (!s_active_obb[i].stream && !s_active_obb[i].retired &&
            s_active_obb[i].references == 0) {
            memset(&s_active_obb[i], 0, sizeof(s_active_obb[i]));
            s_active_obb[i].stream = stream;
            s_active_obb[i].archive = archive;
            pthread_mutex_unlock(&s_active_obb_lock);
            return;
        }
    }
    pthread_mutex_unlock(&s_active_obb_lock);
    port_trace("IO: direct OBB stream table full for %p", stream);
}

static ActiveObbStream *acquire_active_obb_stream(FILE *stream) {
    ActiveObbStream *result = NULL;
    pthread_mutex_lock(&s_active_obb_lock);
    for (unsigned i = 0; i < ACTIVE_OBB_STREAMS; ++i) {
        ActiveObbStream *active = &s_active_obb[i];
        if (active->stream == stream && !active->retired) {
            ++active->references;
            result = active;
            break;
        }
    }
    pthread_mutex_unlock(&s_active_obb_lock);
    return result;
}

static void release_active_obb_stream(ActiveObbStream *active) {
    if (!active) return;
    pthread_mutex_lock(&s_active_obb_lock);
    if (active->references > 0) --active->references;
    if (active->retired && active->references == 0) {
        if (active->loose_stream) fclose(active->loose_stream);
        memset(active, 0, sizeof(*active));
    }
    pthread_mutex_unlock(&s_active_obb_lock);
}

static void unregister_obb_stream(FILE *stream) {
    pthread_mutex_lock(&s_active_obb_lock);
    for (unsigned i = 0; i < ACTIVE_OBB_STREAMS; ++i) {
        ActiveObbStream *active = &s_active_obb[i];
        if (active->stream != stream) continue;
        active->stream = NULL;
        active->retired = 1;
        if (active->references == 0) {
            if (active->loose_stream) fclose(active->loose_stream);
            memset(active, 0, sizeof(*active));
        }
        break;
    }
    pthread_mutex_unlock(&s_active_obb_lock);
}

static int open_loose_entry(ActiveObbStream *active,
                            const ObbEntry *entry) {
    if (active->loose_entry == entry && active->loose_stream) return 1;
    if (active->loose_stream) fclose(active->loose_stream);
    active->loose_stream = NULL;
    active->loose_entry = NULL;
    active->loose_position = 0;

    ObbEntry *mutable_entry = (ObbEntry *)entry;
    if (mutable_entry->loose_cached == -1) {
        return 0;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s%s", DATA_PATH, entry->name) >=
        (int)sizeof(path)) return 0;
    FILE *loose = fopen(path, "rb");
    if (!loose) {
        mutable_entry->loose_cached = -1;
        return 0;
    }

    if (fseek(loose, 0, SEEK_END) != 0 ||
        ftell(loose) != (long)entry->size ||
        fseek(loose, 0, SEEK_SET) != 0) {
        fclose(loose);
        mutable_entry->loose_cached = -1;
        return 0;
    }
    mutable_entry->loose_cached = 1;
    active->loose_stream = loose;
    active->loose_entry = entry;
    return 1;
}

static int should_read_loose_entry(const ObbEntry *entry) {
    if (!entry || entry->size < 256 * 1024) return 0;
    const char *dot = strrchr(entry->name, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".tga") == 0 ||
           strcasecmp(dot, ".bmp") == 0 ||
           strcasecmp(dot, ".dds") == 0 ||
           strcasecmp(dot, ".pvr") == 0;
}

static size_t fread_from_loose_obb(void *ptr, size_t size, size_t nmemb,
                                   FILE *stream, int *handled) {
    *handled = 0;
    if (!size || !nmemb || nmemb > SIZE_MAX / size) return 0;

    /* Keep the registry entry alive for the whole shadow read.  A concurrent
     * fclose retires the slot and its loose FILE is closed by the final
     * reader, instead of invalidating a pointer after lookup. */
    ActiveObbStream *active = acquire_active_obb_stream(stream);
    if (!active) return 0;

    long position = ftell(stream);
    if (position < 0 || (unsigned long)position > UINT32_MAX) {
        release_active_obb_stream(active);
        return 0;
    }
    size_t requested = size * nmemb;
    const ObbEntry *entry = find_entry_at_offset(
        active->archive, (uint32_t)position, requested);
    if (!entry) {
        release_active_obb_stream(active);
        return 0;
    }

    if (!should_read_loose_entry(entry)) {
        static unsigned archive_entry_bypass_count;
        unsigned call = __sync_add_and_fetch(&archive_entry_bypass_count, 1);
        if (call <= 16 || call % 2000 == 0) {
            port_trace("IO: archive fast path #%u %s size=%u request=%u",
                       call, entry->name, entry->size,
                       (unsigned)requested);
        }
        release_active_obb_stream(active);
        return 0;
    }

    if (!open_loose_entry(active, entry)) {
        release_active_obb_stream(active);
        return 0;
    }

    long relative = position - (long)entry->offset;
    if (active->loose_position != relative) {
        if (fseek(active->loose_stream, relative, SEEK_SET) != 0) {
            release_active_obb_stream(active);
            return 0;
        }
        active->loose_position = relative;
    }

    size_t result = fread(ptr, size, nmemb, active->loose_stream);
    size_t bytes = result * size;
    active->loose_position += (long)bytes;
    *handled = 1;
    if (fseek(stream, position + (long)bytes, SEEK_SET) != 0) {
        port_trace("IO: failed to advance OBB shadow stream %p", stream);
    }

    unsigned call = __sync_add_and_fetch(&s_loose_read_count, 1);
    unsigned total = __sync_add_and_fetch(
        &s_loose_read_bytes, (unsigned)bytes);
    if (call <= 20 || call % 1000 == 0 || requested > 1024 * 1024) {
        port_trace("IO: loose OBB read #%u %s +%ld bytes=%u total=%u",
                   call, entry->name, relative, (unsigned)bytes, total);
    }
    release_active_obb_stream(active);
    return result;
}

static FILE *fopen_from_obb(const char *key) {
    if (!key) {
        return NULL;
    }

    /* Resolve before touching the extracted directory.  A failed lookup in
     * `_packRes` otherwise makes Vita's FAT driver linearly walk thousands
     * of loose files, taking around 1.1 seconds for every optional asset. */
    CachedObb *archive = NULL;
    const ObbEntry *entry = find_entry_in_any_obb(key, &archive);
    if (!entry || !archive) return NULL;

    /* Some Glitch VFS paths name an entry as
     * "...main.obb/_packres/file".  That is not a real directory, so the
     * normal path translation reaches this fallback even when the matching
     * unpacked resource is already installed.  Prefer the canonical loose
     * tree before copying the same asset out of the multi-gigabyte OBB. */
    const char *relative = NULL;
    const char *directory = NULL;
    if (strncasecmp(key, "_packres/", 9) == 0) {
        directory = "_packRes";
        relative = key + 9;
    } else if (strncasecmp(key, "_packetc/", 9) == 0) {
        directory = "_packEtc";
        relative = key + 9;
    }
    if (directory && relative && *relative) {
        char loose_path[PATH_MAX];
        if (snprintf(loose_path, sizeof(loose_path), "%s%s/%s",
                     DATA_PATH, directory, relative) <
            (int)sizeof(loose_path)) {
            FILE *loose = fopen(loose_path, "rb");
            if (loose) {
                static unsigned loose_open_count;
                unsigned call = __sync_add_and_fetch(&loose_open_count, 1);
                if (call <= 20 || call % 500 == 0) {
                    port_trace("IO: unpacked fallback #%u %s -> %s",
                               call, key, loose_path);
                }
                return loose;
            }
        }
    }

    /* Check if disk cache already exists first */
    char cache_dir[PATH_MAX];
    char cache_path[PATH_MAX];
    uint32_t hash = 2166136261u;
    const unsigned char *cursor = (const unsigned char *)key;
    while (*cursor) {
        hash ^= *cursor++;
        hash *= 16777619u;
    }
    if (snprintf(cache_dir, sizeof(cache_dir), "%scache", DATA_PATH) < (int)sizeof(cache_dir) &&
        snprintf(cache_path, sizeof(cache_path), "%s/packres-%08x.bin", cache_dir, hash) < (int)sizeof(cache_path)) {
        FILE *cached_stream = fopen(cache_path, "rb");
        if (cached_stream) {
            return cached_stream;
        }
    }

    {
        FILE *obb = fopen(archive->path, "rb");
        if (!obb) {
            return NULL;
        }

        if (fseek(obb, entry->offset, SEEK_SET) != 0) {
            fclose(obb);
            return NULL;
        }

        FILE *tmp = open_obb_cache(key);
        if (!tmp) {
            fclose(obb);
            return NULL;
        }

        unsigned char copy_buffer[65536];
        uint32_t remaining = entry->size;
        while (remaining > 0) {
            size_t chunk = remaining < (uint32_t)sizeof(copy_buffer) ? remaining : (uint32_t)sizeof(copy_buffer);
            size_t read_bytes = fread(copy_buffer, 1, chunk, obb);
            if (!read_bytes) {
                break;
            }
            if (fwrite(copy_buffer, 1, read_bytes, tmp) != read_bytes) {
                break;
            }
            remaining -= (uint32_t)read_bytes;
        }
        fclose(obb);

        if (!remaining) {
            rewind(tmp);
            port_trace("IO: OBB fallback for %s from %s (size %u)", key,
                       archive->path, entry->size);
            return tmp;
        }
        fclose(tmp);
    }

    return NULL;
}


// Translate the Android SD-card layout used by Gameloft's 2011 runtime to the
// Vita data directory.  Keep the old bundle:// fallback for boilerplate
// compatibility, although Modern Combat 2 primarily uses POSIX file calls.
static const char *translate_bundle_path(const char *path, char translated[PATH_MAX]) {
    static const char prefix[] = "bundle://";
    static const char sdcard_prefix[] =
        "/sdcard/gameloft/games/GloftBPHP";
    static const char mnt_sdcard_prefix[] =
        "/mnt/sdcard/gameloft/games/GloftBPHP";
    static const char rio_sdcard_prefix[] =
        "/sdcard/gameloft/games/GloftG4HM";
    static const char rio_mnt_sdcard_prefix[] =
        "/mnt/sdcard/gameloft/games/GloftG4HM";
    static const char rio_android_data_prefix[] =
        "/sdcard/Android/data/com.gameloft.android.ANMP.GloftG4HM";
    static const char app_prefix[] =
        "/data/data/com.gameloft.android.GAND.GloftBPHP.ML";
    static const char rio_data_prefix[] =
        "/data/data/com.gameloft.android.ANMP.GloftG4HM";
    static const char vita_mc2_prefix[] = "ux0:data/mc2";
    static const char data_prefix[] = DATA_PATH;
    static const char documents_prefix[] = "../Documents/";
    static const char rio_vita_game_prefix[] = DATA_PATH "game/";
    if (!path) {
        return path;
    }

    char clean_path[PATH_MAX];
    size_t path_len = strlen(path);
    if (path_len >= PATH_MAX) path_len = PATH_MAX - 1;
    for (size_t i = 0; i < path_len; ++i) {
        clean_path[i] = (path[i] == '\\') ? '/' : path[i];
    }
    clean_path[path_len] = '\0';

    const char *normalized = clean_path;
    while (strncasecmp(normalized, documents_prefix,
                       sizeof(documents_prefix) - 1) == 0) {
        normalized += sizeof(documents_prefix) - 1;
    }

    const char *obb_ext = strstr(normalized, ".obb");
    if (obb_ext && strcasecmp(obb_ext, ".obb") == 0) {
        const char *filename = strrchr(normalized, '/');
        if (filename) {
            filename++;
        } else {
            filename = normalized;
        }
        snprintf(translated, PATH_MAX, "%sobb/%s", DATA_PATH, filename);
        return translated;
    }

    /* The game's CFileSystem normally turns "data/foo" into
     * "./data/foo" or "/data/foo" before calling fopen.  Check Android's
     * true absolute paths first, then normalize those harmless prefixes
     * before matching the packaged data directory. */
    if (strncasecmp(normalized, sdcard_prefix,
                    sizeof(sdcard_prefix) - 1) == 0) {
        const char *relative = normalized + sizeof(sdcard_prefix) - 1;
        while (*relative == '/') relative++;
        snprintf(translated, PATH_MAX, "%sGloftBPHP/%s", DATA_PATH,
                 relative);
        return translated;
    } else if (strncasecmp(normalized, mnt_sdcard_prefix,
                           sizeof(mnt_sdcard_prefix) - 1) == 0) {
        const char *relative = normalized + sizeof(mnt_sdcard_prefix) - 1;
        while (*relative == '/') relative++;
        snprintf(translated, PATH_MAX, "%sGloftBPHP/%s", DATA_PATH,
                 relative);
        return translated;
    } else if (strncasecmp(normalized, rio_sdcard_prefix,
                           sizeof(rio_sdcard_prefix) - 1) == 0) {
        const char *relative = normalized + sizeof(rio_sdcard_prefix) - 1;
        while (*relative == '/') relative++;
        snprintf(translated, PATH_MAX, "%s%s", DATA_PATH, relative);
        return translated;
    } else if (strncasecmp(normalized, rio_mnt_sdcard_prefix,
                           sizeof(rio_mnt_sdcard_prefix) - 1) == 0) {
        const char *relative = normalized + sizeof(rio_mnt_sdcard_prefix) - 1;
        while (*relative == '/') relative++;
        snprintf(translated, PATH_MAX, "%s%s", DATA_PATH, relative);
        return translated;
    } else if (strncasecmp(normalized, rio_android_data_prefix,
                           sizeof(rio_android_data_prefix) - 1) == 0) {
        const char *relative = normalized + sizeof(rio_android_data_prefix) - 1;
        while (*relative == '/') relative++;
        if (strncmp(relative, "files/", 6) == 0)
            relative += 6;
        snprintf(translated, PATH_MAX, "%sfiles/%s", DATA_PATH, relative);
        return translated;
    } else if (strncmp(normalized, rio_data_prefix,
                       sizeof(rio_data_prefix) - 1) == 0) {
        const char *relative = normalized + sizeof(rio_data_prefix) - 1;
        while (*relative == '/') relative++;
        if (strncmp(relative, "files/", 6) == 0)
            relative += 6;
        snprintf(translated, PATH_MAX, "%sfiles/%s", DATA_PATH, relative);
        return translated;
    } else if (strncmp(normalized, app_prefix,
                       sizeof(app_prefix) - 1) == 0) {
        const char *relative = normalized + sizeof(app_prefix) - 1;
        while (*relative == '/') relative++;
        if (strncmp(relative, "files/", 6) == 0)
            relative += 6;
        snprintf(translated, PATH_MAX, "%sfiles/%s", DATA_PATH, relative);
        return translated;
    } else if (strncasecmp(normalized, rio_vita_game_prefix,
                           sizeof(rio_vita_game_prefix) - 1) == 0) {
        const char *relative = normalized + sizeof(rio_vita_game_prefix) - 1;
        while (*relative == '/') relative++;
        while (strncasecmp(relative, DATA_PATH, sizeof(DATA_PATH) - 1) == 0) {
            relative += sizeof(DATA_PATH) - 1;
            while (*relative == '/') relative++;
        }
        /* Android package keys are case-insensitive, while Vita paths are
         * not. Purple frequently emits the all-lowercase virtual mount name
         * `_packetc`; point that top-level mount at the extracted `_packEtc`
         * directory so textures are really read from loose resources. */
        if (strncasecmp(relative, "_packetc/", 9) == 0) {
            snprintf(translated, PATH_MAX, "%s_packEtc/%s", DATA_PATH,
                     relative + 9);
            return translated;
        }
        if (strncasecmp(relative, "_packres/", 9) == 0) {
            snprintf(translated, PATH_MAX, "%s_packRes/%s", DATA_PATH,
                     relative + 9);
            return translated;
        }
        snprintf(translated, PATH_MAX, "%s%s", DATA_PATH, relative);
        return translated;
    } else if (strncasecmp(normalized, data_prefix,
                           sizeof(data_prefix) - 1) == 0) {
        const char *relative = normalized + sizeof(data_prefix) - 1;
        while (*relative == '/') relative++;
        if (strncasecmp(relative, "_packetc/", 9) == 0) {
            snprintf(translated, PATH_MAX, "%s_packEtc/%s", DATA_PATH,
                     relative + 9);
            return translated;
        }
        if (strncasecmp(relative, "_packres/", 9) == 0) {
            snprintf(translated, PATH_MAX, "%s_packRes/%s", DATA_PATH,
                     relative + 9);
            return translated;
        }
        snprintf(translated, PATH_MAX, "%s%s", DATA_PATH, relative);
        return translated;
    } else if (strncasecmp(normalized, vita_mc2_prefix,
                           sizeof(vita_mc2_prefix) - 1) == 0) {
        /* This particular libsandstorm2.so contains an old Vita bring-up
         * root.  Keep it as a read/write alias without requiring a second
         * copy of the 370 MB data set. */
        const char *relative = normalized + sizeof(vita_mc2_prefix) - 1;
        while (*relative == '/') relative++;
        /* Some text lookups concatenate the legacy root twice.  Collapse the
         * second copy instead of producing
         * GloftBPHP/ux0:data/mc2/data/texts.pak. */
        while (strncasecmp(relative, vita_mc2_prefix,
                           sizeof(vita_mc2_prefix) - 1) == 0) {
            relative += sizeof(vita_mc2_prefix) - 1;
            while (*relative == '/') relative++;
        }
        snprintf(translated, PATH_MAX, "%sGloftBPHP/%s", DATA_PATH,
                 relative);
        return translated;
    }

    const char *relative = normalized;
    if (strncmp(relative, prefix, sizeof(prefix) - 1) == 0) {
        relative += sizeof(prefix) - 1;
    } else {
        while (strncmp(relative, "./", 2) == 0) relative += 2;
        while (*relative == '/') relative++;

        if (strncasecmp(relative, "GloftBPHP/", 11) == 0) {
            snprintf(translated, PATH_MAX, "%s%s", DATA_PATH, relative);
            return translated;
        }
        if (strncasecmp(relative, "data/", 5) == 0) {
            snprintf(translated, PATH_MAX, "%sGloftBPHP/%s", DATA_PATH,
                     relative);
            return translated;
        }

        // AndroidFileMgr strips the URI scheme before some POSIX calls.
        if (strncmp(relative, "res/", 4) != 0)
            return path;
    }

    while (*relative == '/') relative++;

    int length = snprintf(translated, PATH_MAX, "%sassets/%s",
                          DATA_PATH, relative);
    if (length < 0 || length >= PATH_MAX) {
        l_warn("Bundle path is too long: %s", path);
        return path;
    }

    l_debug("Translated %s to %s", path, translated);
    return translated;
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    const char *original = filename;
    char translated[PATH_MAX];
    filename = translate_bundle_path(filename, translated);

    int skip_known_packaged_miss = 0;
    if (mode && mode[0] == 'r' && has_packaged_prefix(filename) &&
        packaged_path_looks_like_file(filename)) {
        char key[PATH_MAX];
        if (make_packres_key(filename, key) &&
            !find_entry_in_any_obb(key, NULL)) {
            skip_known_packaged_miss = 1;
        }
    }

    int cacheable_obb = mode && mode[0] == 'r' && !strchr(mode, '+') &&
                        is_obb_file_path(filename);
    int reused_obb = 0;
    FILE *ret = NULL;
    if (!skip_known_packaged_miss && cacheable_obb) {
#ifndef USE_SCELIBC_IO
        ret = (FILE *)open_virtual_obb_stream(filename);
#endif
        if (!ret) {
            ret = take_cached_obb_handle(filename);
            reused_obb = ret != NULL;
        }
    }

#ifdef USE_SCELIBC_IO
    static int glsl_config_absent;
    size_t filename_length = strlen(filename);
    int is_glsl_config = filename_length >= 12 &&
        strcmp(filename + filename_length - 12, "/glsl_config") == 0;
    if (!ret && !skip_known_packaged_miss &&
        !(is_glsl_config && glsl_config_absent && mode[0] == 'r')) {
        ret = profiled_fopen(filename, mode);
    }
    if (is_glsl_config && !ret && mode[0] == 'r')
        glsl_config_absent = 1;
#else
    if (!ret && !skip_known_packaged_miss)
        ret = profiled_fopen(filename, mode);
#endif

    if (ret && mode && mode[0] == 'r' && !virtual_obb_from_file(ret)) {
        if (cacheable_obb && !reused_obb) {
            configure_obb_stdio_buffer(ret, filename);
            (void)adopt_cached_obb_handle(ret, filename);
        }
        register_obb_stream(ret, filename);
    }

    if (!ret && mode && mode[0] == 'r') {
        char key[PATH_MAX];
        if (make_packres_key(filename, key)) {
            FILE *fallback = fopen_from_obb(key);
            if (fallback) {
                ret = fallback;
            }
        }
    }

    static unsigned trace_count;
    static unsigned failure_count;
    unsigned call = ++trace_count;
    unsigned failure = ret ? 0 : ++failure_count;
    if (call <= 20 || call % 500 == 0 ||
        (failure != 0 && (failure <= 20 || failure % 500 == 0))) {
        port_trace("IO: fopen #%u %s -> %s mode=%s result=%p", call,
                   original, filename, mode, ret);
    }

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    char translated[PATH_MAX];
    path = translate_bundle_path(path, translated);

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    if (!buf) return -1;
    memset(buf, 0, sizeof(*buf));

    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    port_trace("IO: fstat #%d -> size=%lld res=%d", fd, (long long)buf->st_size, res);
    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (!buf) return -1;
    memset(buf, 0, sizeof(*buf));

    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        // FMOD checks both the return value and the file type before enabling
        // its OpenSL backend. Returning success with an untouched output
        // buffer makes that check fail nondeterministically.
        buf->st_mode = S_IFREG | 0555;
        buf->st_nlink = 1;
        buf->st_size = 1;
        port_trace("stat OpenSL probe: regular file reported");
        l_debug("stat(%s): reporting a regular OpenSLES library", path);
        return 0;
    }

    char translated[PATH_MAX];
    const char *final_path = translate_bundle_path(path, translated);

    int res = stat_cached_obb(final_path, buf);
    char key[PATH_MAX];
    if (res != 0 && make_packres_key(final_path, key)) {
        const ObbEntry *entry = find_entry_in_any_obb(key, NULL);
        if (entry) {
            buf->st_mode = S_IFREG | 0444;
            buf->st_nlink = 1;
            buf->st_size = (int64_t)entry->size;
            res = 0;
        }
    }

    int known_packaged_miss = res != 0 && has_packaged_prefix(final_path) &&
                              packaged_path_looks_like_file(final_path);
    if (res != 0 && !known_packaged_miss) {
        struct stat st;
        res = profiled_stat(final_path, &st, 1);
        if (res == 0) {
            stat_newlib_to_bionic(&st, buf);
            SceIoStat psp_stat;
            if (sceIoGetstat(final_path, &psp_stat) >= 0) {
                buf->st_size = (long long)psp_stat.st_size;
            }
        }
    }

    static unsigned stat_count = 0;
    static unsigned stat_failure_count = 0;
    const unsigned call = ++stat_count;
    const unsigned failure = res == 0 ? 0 : ++stat_failure_count;
    if (call <= 20 || call % 500 == 0 ||
        (failure != 0 && (failure <= 20 || failure % 500 == 0))) {
        port_trace("IO: stat %s -> %s size=%lld mode=0%o res=%d",
                   path, final_path, (long long)buf->st_size, (unsigned int)buf->st_mode, res);
    }
    l_debug("stat(%s): %i", final_path, res);
    return res;
}

int lstat_soloader(const char *path, stat64_bionic *buf) {
    if (!buf) return -1;
    memset(buf, 0, sizeof(*buf));

    char translated[PATH_MAX];
    const char *final_path = translate_bundle_path(path, translated);

    int result = stat_cached_obb(final_path, buf);
    char key[PATH_MAX];
    if (result != 0 && make_packres_key(final_path, key)) {
        const ObbEntry *entry = find_entry_in_any_obb(key, NULL);
        if (entry) {
            buf->st_mode = S_IFREG | 0444;
            buf->st_nlink = 1;
            buf->st_size = (int64_t)entry->size;
            result = 0;
        }
    }

    int known_packaged_miss = result != 0 &&
                              has_packaged_prefix(final_path) &&
                              packaged_path_looks_like_file(final_path);
    if (result != 0 && !known_packaged_miss) {
        struct stat st;
        result = profiled_stat(final_path, &st, 0);
        if (result == 0) {
            stat_newlib_to_bionic(&st, buf);
            SceIoStat psp_stat;
            if (sceIoGetstat(final_path, &psp_stat) >= 0) {
                buf->st_size = (long long)psp_stat.st_size;
            }
        }
    }

    static unsigned lstat_count = 0;
    static unsigned lstat_failure_count = 0;
    const unsigned call = ++lstat_count;
    const unsigned failure = result == 0 ? 0 : ++lstat_failure_count;
    if (call <= 20 || call % 500 == 0 ||
        (failure != 0 && (failure <= 20 || failure % 500 == 0))) {
        port_trace("IO: lstat %s -> %s size=%lld res=%d", path, final_path, (long long)buf->st_size, result);
    }
    l_debug("lstat(%s): %i", final_path, result);
    return result;
}

int chdir_soloader(const char *path) {
    char translated[PATH_MAX];
    path = translate_bundle_path(path, translated);
    int result = chdir(path);
    port_trace("IO: chdir %s -> %d", path, result);
    return result;
}

int mkdir_soloader(const char *path, mode_t mode) {
    char translated[PATH_MAX];
    path = translate_bundle_path(path, translated);
    return mkdir(path, mode);
}

int remove_soloader(const char *path) {
    char translated[PATH_MAX];
    path = translate_bundle_path(path, translated);
    return remove(path);
}

int rename_soloader(const char *old_path, const char *new_path) {
    char translated_old[PATH_MAX];
    char translated_new[PATH_MAX];
    old_path = translate_bundle_path(old_path, translated_old);
    new_path = translate_bundle_path(new_path, translated_new);
    return rename(old_path, new_path);
}

int fclose_soloader(FILE * f) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(f);
    if (virtual_stream) {
        return close_virtual_obb_stream(virtual_stream) ? 0 : EOF;
    }
    unregister_obb_stream(f);
    int cached = release_cached_obb_handle(f);
#ifdef USE_SCELIBC_IO
    int ret = cached ? 0 : sceLibcBridge_fclose(f);
#else
    int ret = cached ? 0 : fclose(f);
#endif
    static unsigned fclose_count = 0;
    if (++fclose_count <= 10 || fclose_count % 200 == 0 || ret != 0) {
        port_trace("IO: fclose #%u %p -> %d cached=%d", fclose_count, f,
                   ret, cached);
    }
    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

size_t fread_soloader(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    SceUID thread = sceKernelGetThreadId();
    unsigned serial = __sync_add_and_fetch(&s_read_serial, 1);
    IoReadDiagnostic *diagnostic =
        &s_recent_reads[(serial - 1) % IO_READ_DIAGNOSTIC_SLOTS];
    diagnostic->completed = 0;
    diagnostic->destination = ptr;
    diagnostic->stream = stream;
    diagnostic->size = size;
    diagnostic->count = nmemb;
    diagnostic->result = 0;
    diagnostic->thread = thread;
    __sync_synchronize();
    diagnostic->serial = serial;

    IoPhysicalStats physical_before;
    io_get_physical_stats(&physical_before);
    uint64_t started = sceKernelGetSystemTimeWide();
    size_t ret;
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    int64_t start_position = virtual_stream ? virtual_stream->position : -1;
    if (virtual_stream) {
        if (!size || !nmemb || nmemb > SIZE_MAX / size) {
            ret = 0;
        } else {
            size_t bytes = read_virtual_obb(virtual_stream, ptr,
                                            size * nmemb);
            ret = bytes / size;
        }
    } else
#if ENABLE_LOOSE_OBB_SHADOW_READS
    int loose_handled = 0;
    ret = fread_from_loose_obb(ptr, size, nmemb, stream, &loose_handled);
    if (!loose_handled)
#endif
    {
#ifdef USE_SCELIBC_IO
        ret = sceLibcBridge_fread(ptr, size, nmemb, stream);
#else
        ret = fread(ptr, size, nmemb, stream);
#endif
    }
    uint64_t elapsed_us = sceKernelGetSystemTimeWide() - started;
    diagnostic->result = ret;
    __sync_synchronize();
    diagnostic->completed = 1;
    if (serial <= 20 || serial % 1000 == 0 || elapsed_us >= 250000) {
        IoPhysicalStats physical_after;
        io_get_physical_stats(&physical_after);
        const ObbEntry *entry = virtual_stream && start_position >= 0 && start_position <= UINT32_MAX
            ? find_entry_at_offset(virtual_stream->archive, (uint32_t)start_position, 1) : NULL;
        port_trace("IO: fread #%u thread=0x%08x resource=%s f=%p dst=%p size=%u nmemb=%u -> %u took=%u us pread(calls=%u us=%u bytes=%u) cache_lock(calls=%u us=%u)",
                   serial, (unsigned)thread, entry ? entry->name : "<stream>",
                   stream, ptr, (unsigned)size, (unsigned)nmemb, (unsigned)ret, (unsigned)elapsed_us,
                   physical_after.pread_calls - physical_before.pread_calls,
                   physical_after.pread_us - physical_before.pread_us,
                   physical_after.pread_bytes - physical_before.pread_bytes,
                   physical_after.cache_lock_calls - physical_before.cache_lock_calls,
                   physical_after.cache_lock_us - physical_before.cache_lock_us);
    }
    return ret;
}

int feof_soloader(FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    return virtual_stream ? virtual_stream->eof_flag : feof(stream);
}

int ferror_soloader(FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    return virtual_stream ? virtual_stream->error_flag : ferror(stream);
}

int fflush_soloader(FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    return virtual_stream ? 0 : fflush(stream);
}

int getc_soloader(FILE *stream) {
    unsigned char character;
    return fread_soloader(&character, 1, 1, stream) == 1
        ? (int)character : EOF;
}

wint_t getwc_soloader(FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (!virtual_stream) return getwc(stream);
    int character = getc_soloader(stream);
    return character == EOF ? WEOF : (wint_t)(unsigned char)character;
}

char *fgets_soloader(char *buffer, int size, FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (!virtual_stream) return fgets(buffer, size, stream);
    if (!buffer || size <= 0) return NULL;
    int written = 0;
    while (written + 1 < size) {
        int character = getc_soloader(stream);
        if (character == EOF) break;
        buffer[written++] = (char)character;
        if (character == '\n') break;
    }
    if (!written) return NULL;
    buffer[written] = '\0';
    return buffer;
}

size_t fwrite_soloader(const void *ptr, size_t size, size_t nmemb,
                       FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) {
        (void)ptr;
        (void)size;
        (void)nmemb;
        virtual_stream->error_flag = 1;
        return 0;
    }
    return fwrite(ptr, size, nmemb, stream);
}

int fputc_soloader(int character, FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) {
        virtual_stream->error_flag = 1;
        return EOF;
    }
    return fputc(character, stream);
}

int putc_soloader(int character, FILE *stream) {
    return fputc_soloader(character, stream);
}

wint_t putwc_soloader(wchar_t character, FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) {
        (void)character;
        virtual_stream->error_flag = 1;
        return WEOF;
    }
    return putwc(character, stream);
}

int fputs_soloader(const char *text, FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) {
        (void)text;
        virtual_stream->error_flag = 1;
        return EOF;
    }
    return fputs(text, stream);
}

void rewind_soloader(FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) {
        virtual_stream->position = 0;
        virtual_stream->eof_flag = 0;
        virtual_stream->error_flag = 0;
        virtual_stream->ungot = EOF;
        return;
    }
    rewind(stream);
}

int setvbuf_soloader(FILE *stream, char *buffer, int mode, size_t size) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) {
        (void)buffer;
        (void)mode;
        (void)size;
        return 0;
    }
    return setvbuf(stream, buffer, mode, size);
}

int ungetc_soloader(int character, FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (!virtual_stream) return ungetc(character, stream);
    if (character == EOF || virtual_stream->ungot != EOF ||
        virtual_stream->position <= 0) return EOF;
    virtual_stream->ungot = (unsigned char)character;
    --virtual_stream->position;
    virtual_stream->eof_flag = 0;
    return (unsigned char)character;
}

wint_t ungetwc_soloader(wint_t character, FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (!virtual_stream) return ungetwc(character, stream);
    if (character == WEOF || (unsigned long)character > UCHAR_MAX)
        return WEOF;
    return ungetc_soloader((unsigned char)character, stream) == EOF
        ? WEOF : character;
}

int vfprintf_soloader(FILE *stream, const char *format, va_list arguments) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) {
        (void)format;
        (void)arguments;
        virtual_stream->error_flag = 1;
        return -1;
    }
    return vfprintf(stream, format, arguments);
}

int fprintf_soloader(FILE *stream, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf_soloader(stream, format, arguments);
    va_end(arguments);
    return result;
}

int fseek_soloader(FILE *stream, long offset, int whence) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) {
        int64_t base;
        if (whence == SEEK_SET) base = 0;
        else if (whence == SEEK_CUR) base = virtual_stream->position;
        else if (whence == SEEK_END) base = virtual_stream->archive->file_size;
        else {
            virtual_stream->error_flag = 1;
            return -1;
        }
        int64_t target = base + (int64_t)offset;
        if (target < 0 || target > virtual_stream->archive->file_size) {
            virtual_stream->error_flag = 1;
            return -1;
        }
        virtual_stream->position = target;
        virtual_stream->eof_flag = 0;
        virtual_stream->ungot = EOF;
        return 0;
    }
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fseek(stream, offset, whence);
#else
    int ret = fseek(stream, offset, whence);
#endif
    static unsigned fseek_count = 0;
    ++fseek_count;
    if (fseek_count <= 20 || fseek_count % 1000 == 0 || ret != 0 || offset < 0) {
        port_trace("IO: fseek #%u f=%p off=%ld whence=%d -> %d", fseek_count, stream, offset, whence, ret);
    }
    return ret;
}

long ftell_soloader(FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) return (long)virtual_stream->position;
#ifdef USE_SCELIBC_IO
    long ret = sceLibcBridge_ftell(stream);
#else
    long ret = ftell(stream);
#endif
    static unsigned ftell_count = 0;
    ++ftell_count;
    if (ftell_count <= 10 || ftell_count % 2000 == 0 || ret < 0) {
        port_trace("IO: ftell #%u f=%p -> %ld", ftell_count, stream, ret);
    }
    return ret;
}

int fseeko_soloader(FILE *stream, off_t offset, int whence) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) {
        if (offset < LONG_MIN || offset > LONG_MAX) return -1;
        return fseek_soloader(stream, (long)offset, whence);
    }
    int ret = fseeko(stream, offset, whence);
    static unsigned fseeko_count = 0;
    ++fseeko_count;
    if (fseeko_count <= 20 || fseeko_count % 1000 == 0 || ret != 0 || offset < 0) {
        port_trace("IO: fseeko #%u f=%p off=%lld whence=%d -> %d", fseeko_count, stream, (long long)offset, whence, ret);
    }
    return ret;
}

off_t ftello_soloader(FILE *stream) {
    VirtualObbStream *virtual_stream = virtual_obb_from_file(stream);
    if (virtual_stream) return (off_t)virtual_stream->position;
    off_t ret = ftello(stream);
    static unsigned ftello_count = 0;
    ++ftello_count;
    if (ftello_count <= 10 || ftello_count % 2000 == 0 || ret < 0) {
        port_trace("IO: ftello #%u f=%p -> %lld", ftello_count, stream, (long long)ret);
    }
    return ret;
}

off_t lseek_soloader(int fd, off_t offset, int whence) {
    off_t ret = lseek(fd, offset, whence);
    static unsigned lseek_count = 0;
    ++lseek_count;
    if (lseek_count <= 20 || lseek_count % 1000 == 0 || ret == (off_t)-1) {
        port_trace("IO: lseek #%u fd=%d off=%lld whence=%d -> %lld", lseek_count, fd, (long long)offset, whence, (long long)ret);
    }
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    char translated[PATH_MAX];
    const char *path = translate_bundle_path(_pathname, translated);
    DIR* ret = opendir(path);
    l_debug("opendir(\"%s\"): %p", path, ret);
    return ret;
}

int access_soloader(const char *path, int mode) {
    char translated[PATH_MAX];
    path = translate_bundle_path(path, translated);

    int ret = access(path, mode);
    if (ret == 0)
        l_debug("access(%s, %x): %i", path, mode, ret);
    else
        l_warn("access(%s, %x): %i", path, mode, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
