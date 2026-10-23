#include "utils/filecache.h"
#include "utils/logger.h"

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>

#define FC_TABLE_SIZE 16384
#define FC_NAME_SIZE 128

enum { FC_EMPTY, FC_KNOWN, FC_DIRTY_TREE, FC_DIRTY_METADATA };
typedef struct {
    uint32_t hash, size;
    uint16_t mode, state;
    char name[FC_NAME_SIZE];
} entry_t;
static entry_t *table;
static SceKernelLwMutexWork mutex;
static int complete;
static unsigned count;

static uint32_t hash_name(const char *name) {
    uint32_t h = 2166136261u;
    for (; *name; name++) {
        unsigned char c = *name;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        h = (h ^ c) * 16777619u;
    }
    return h;
}

/* The filesystem is case insensitive. Keep invalidated entries in the probe
 * chain: deleting an occupied slot could conceal a colliding asset. */
static entry_t *entry(const char *name, int create) {
    uint32_t hash = hash_name(name), slot = hash & (FC_TABLE_SIZE - 1);
    for (unsigned n = 0; n < FC_TABLE_SIZE; n++, slot = (slot + 1) & (FC_TABLE_SIZE - 1)) {
        entry_t *e = &table[slot];
        if (e->state == FC_EMPTY) {
            if (!create) return NULL;
            e->hash = hash;
            strcpy(e->name, name);
            count++;
            return e;
        }
        if (e->hash == hash && !strcasecmp(e->name, name)) return e;
    }
    return NULL;
}

static const char *device_path(const char *path) {
    /* Both ux0:data and ux0:/data identify the same Vita device path. */
    const char *colon = strchr(path, ':');
    return colon ? colon + 1 + (colon[1] == '/') : NULL;
}

/* 0 = outside this root, -1 = uncertain alias, 1 = canonical in-root. */
static int relative_path(const char *path, char *out) {
    if (!path) return 0;
    const char *root = GAME_DATA_PATH;
    const char *colon = strchr(path, ':'), *root_colon = strchr(root, ':');
    if (!colon || !root_colon || colon - path != root_colon - root ||
        strncasecmp(path, root, (size_t)(root_colon - root + 1))) return 0;
    const char *p = device_path(path), *r = device_path(root);
    if (strchr(p, '\\')) return -1;
    for (const char *part = p; *part;) {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') ||
            (size == 2 && part[0] == '.' && part[1] == '.')) return -1;
        if (!end) break;
        part = end + 1;
    }
    size_t n = strlen(r);
    while (n && r[n - 1] == '/') n--;
    if (strncasecmp(p, r, n) || (p[n] && p[n] != '/')) return 0;
    p += n;
    if (*p == '/') p++;
    size_t len = strlen(p);
    if (len && p[len - 1] == '/') len--;
    if (len >= FC_NAME_SIZE) return -1;
    memcpy(out, p, len);
    out[len] = 0;
    return 1;
}

static int insert(const char *name, const SceIoStat *st) {
    if (strlen(name) >= FC_NAME_SIZE || st->st_size > UINT32_MAX) return 0;
    entry_t *e = entry(name, 1);
    if (!e) return 0;
    unsigned type = SCE_S_ISDIR(st->st_mode) ? S_IFDIR :
                    SCE_S_ISREG(st->st_mode) ? S_IFREG : 0;
    if (!type) return 0;
    e->mode = type | (st->st_mode & 0777);
    e->size = (uint32_t)st->st_size;
    e->state = FC_KNOWN;
    return 1;
}

static int scan(const char *path, const char *prefix) {
    SceUID fd = sceIoDopen(path);
    if (fd < 0) return 0;
    SceIoDirent d;
    int result, ok = 1;
    for (;;) {
        memset(&d, 0, sizeof(d));
        result = sceIoDread(fd, &d);
        if (result <= 0) break;
        if (!strcmp(d.d_name, ".") || !strcmp(d.d_name, "..")) continue;
        char name[FC_NAME_SIZE], full[1024];
        int n = snprintf(name, sizeof(name), "%s%s%s", prefix, *prefix ? "/" : "", d.d_name);
        int f = snprintf(full, sizeof(full), "%s/%s", path, d.d_name);
        if (n < 0 || n >= sizeof(name) || f < 0 || f >= sizeof(full) || !insert(name, &d.d_stat)) {
            ok = 0;
            break;
        }
        if (SCE_S_ISDIR(d.d_stat.st_mode) && !scan(full, name)) { ok = 0; break; }
    }
    if (result < 0) ok = 0;
    if (sceIoDclose(fd) < 0) ok = 0;
    return ok;
}

void filecache_init(void) {
    if (table) return;
    if (sceKernelCreateLwMutex(&mutex, "asset_index", 0, 0, NULL) < 0) return;
    table = calloc(FC_TABLE_SIZE, sizeof(*table));
    if (!table) return;
    uint64_t start = sceKernelGetProcessTimeWide();
    SceIoStat root;
    complete = sceIoGetstat(GAME_DATA_PATH, &root) >= 0 && insert("", &root) && scan(GAME_DATA_PATH, "");
    l_note("[IO] asset index: entries=%u complete=%d time_ms=%llu", count, complete,
           (unsigned long long)((sceKernelGetProcessTimeWide() - start) / 1000));
}

int filecache_lookup(const char *path, uint32_t *size, uint16_t *mode) {
    char name[FC_NAME_SIZE];
    if (!table || relative_path(path, name) != 1) return -1;
    sceKernelLockLwMutex(&mutex, 1, NULL);
    int result = -1;
    if (!complete) goto done;
    entry_t *root = entry("", 0);
    if (root && root->state == FC_DIRTY_TREE) goto done;
    for (char *p = name; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        entry_t *parent = entry(name, 0);
        *p = '/';
        if (parent && parent->state == FC_DIRTY_TREE) goto done;
    }
    entry_t *e = entry(name, 0);
    if (!e) { result = 0; goto done; }
    if (e->state != FC_KNOWN) goto done;
    size_t path_len = strlen(path);
    if (path_len && path[path_len - 1] == '/' && !S_ISDIR(e->mode)) goto done;
    if (size) *size = e->size;
    if (mode) *mode = e->mode;
    result = 1;
done:
    sceKernelUnlockLwMutex(&mutex, 1);
    return result;
}

void filecache_invalidate(const char *path) {
    if (!table) return;
    char name[FC_NAME_SIZE];
    int scope = relative_path(path, name);
    if (!scope) return;
    if (scope < 0) {
        /* An uncertain path might alias an indexed asset via '..'. */
        sceKernelLockLwMutex(&mutex, 1, NULL);
        complete = 0;
        sceKernelUnlockLwMutex(&mutex, 1);
        return;
    }
    sceKernelLockLwMutex(&mutex, 1, NULL);
    entry_t *e = entry(name, 1);
    if (!e) complete = 0;
    else e->state = FC_DIRTY_TREE;
    while (*name) {
        char *slash = strrchr(name, '/');
        if (slash) *slash = 0; else *name = 0;
        e = entry(name, 1);
        if (!e) { complete = 0; break; }
        if (e->state != FC_DIRTY_TREE) e->state = FC_DIRTY_METADATA;
    }
    sceKernelUnlockLwMutex(&mutex, 1);
}
