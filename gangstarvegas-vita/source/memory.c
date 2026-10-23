#include "reimpl/mem.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Mapping {
    void *address;
    void *allocation;
    size_t size;
    int fd;
    off_t offset;
    int shared;
    struct Mapping *next;
} Mapping;
static Mapping *mappings;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;


void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset) {
    (void)address;
    if (!length || length > SIZE_MAX - 4095 || (uint64_t)offset + length > INT32_MAX || offset < 0 || (flags & ~0x23) || (protection & ~3) ||
        ((flags & 3) != 1 && (flags & 3) != 2)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    Mapping *mapping = calloc(1, sizeof(*mapping));
    if (!mapping) return MAP_FAILED;
    mapping->allocation = malloc(length + 4095);
    mapping->address = mapping->allocation ? (void *)(((uintptr_t)mapping->allocation + 4095) & ~(uintptr_t)4095) : NULL;
    if (!mapping->address) { free(mapping); errno = ENOMEM; return MAP_FAILED; }
    memset(mapping->address, 0, length);
    mapping->size = length;
    mapping->offset = offset;
    mapping->shared = (flags & 3) == 1 && (protection & 2);
    mapping->fd = -1;
    if (!(flags & 0x20)) {
        mapping->fd = dup(fd);
        if (mapping->fd < 0) goto fail;
        size_t total = 0;
        while (total < length) {
            ssize_t count = pread(mapping->fd, (char *)mapping->address + total,
                                  length - total, offset + total);
            if (count < 0) { if (errno == EINTR) continue; goto fail; }
            if (!count) break;
            total += count;
        }
    }
    pthread_mutex_lock(&lock);
    mapping->next = mappings;
    mappings = mapping;
    pthread_mutex_unlock(&lock);
    return mapping->address;
fail:
    if (mapping->fd >= 0) close(mapping->fd);
    free(mapping->allocation); free(mapping); return MAP_FAILED;
}

static int flush_mapping(Mapping *mapping, void *address, size_t length) {
    if (!mapping->shared || mapping->fd < 0) return 0;
    size_t total = 0;
    while (total < length) {
        ssize_t count = pwrite(mapping->fd, (char *)address + total, length - total,
                              mapping->offset + ((char *)address - (char *)mapping->address) + total);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        total += count;
    }
    return fsync(mapping->fd);
}

int vegas_msync(void *address, size_t length, int flags) {
    if ((flags & ~7) || (flags & 1 && flags & 4)) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&lock);
    int result = -1;
    errno = ENOMEM;
    for (Mapping *m = mappings; m; m = m->next) {
        uintptr_t displacement = (uintptr_t)address - (uintptr_t)m->address;
        if (displacement <= m->size && length <= m->size - displacement) {
            result = flush_mapping(m, address, length);
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

int munmap(void *address, size_t length) {
    pthread_mutex_lock(&lock);
    Mapping **link = &mappings;
    while (*link && (*link)->address != address) link = &(*link)->next;
    if (!*link || (*link)->size != length) { pthread_mutex_unlock(&lock); errno = EINVAL; return -1; }
    Mapping *m = *link;
    int result = flush_mapping(m, address, length);
    if (result == 0) {
        *link = m->next;
        if (m->fd >= 0) close(m->fd);
        free(m->allocation); free(m);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
