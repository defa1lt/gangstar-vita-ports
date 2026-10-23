/*
 * fios.c -- use FIOS2 for optimized I/O caching
 *
 * Adapted from 9mm-vita / Andy Nguyen
 */

#include "fios.h"
#include "diagnostics.h"

#include <malloc.h>
#include <string.h>

#define MAX_PATH_LENGTH 256
#define RAMCACHEBLOCKSIZE (128 * 1024)
#define RAMCACHEBLOCKNUM 64

static int64_t g_OpStorage[SCE_FIOS_OP_STORAGE_SIZE(64, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_ChunkStorage[SCE_FIOS_CHUNK_STORAGE_SIZE(1024) / sizeof(int64_t) + 1];
static int64_t g_FHStorage[SCE_FIOS_FH_STORAGE_SIZE(1024, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_DHStorage[SCE_FIOS_DH_STORAGE_SIZE(32, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];

static SceFiosRamCacheContext g_RamCacheContext = SCE_FIOS_RAM_CACHE_CONTEXT_INITIALIZER;
static char *g_RamCacheWorkBuffer = NULL;

int fios_init(const char * path) {
    int res;

    SceFiosParams params = SCE_FIOS_PARAMS_INITIALIZER;
    params.opStorage.pPtr = g_OpStorage;
    params.opStorage.length = sizeof(g_OpStorage);
    params.chunkStorage.pPtr = g_ChunkStorage;
    params.chunkStorage.length = sizeof(g_ChunkStorage);
    params.fhStorage.pPtr = g_FHStorage;
    params.fhStorage.length = sizeof(g_FHStorage);
    params.dhStorage.pPtr = g_DHStorage;
    params.dhStorage.length = sizeof(g_DHStorage);
    params.pathMax = MAX_PATH_LENGTH;

    params.threadAffinity[SCE_FIOS_IO_THREAD] = 0x20000;
    params.threadAffinity[SCE_FIOS_CALLBACK_THREAD] = 0;
    params.threadAffinity[SCE_FIOS_DECOMPRESSOR_THREAD] = 0;

    params.threadPriority[SCE_FIOS_IO_THREAD] = 64;
    params.threadPriority[SCE_FIOS_CALLBACK_THREAD] = 191;
    params.threadPriority[SCE_FIOS_DECOMPRESSOR_THREAD] = 191;

    res = sceFiosInitialize(&params);
    if (res < 0) {
        LOG_ERROR("FIOS: sceFiosInitialize failed: 0x%08x", res);
        return res;
    }

    g_RamCacheWorkBuffer = memalign(8, RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE);
    if (!g_RamCacheWorkBuffer) {
        LOG_ERROR("FIOS: cannot allocate work buffer (%d KB)", (RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE) / 1024);
        return -1;
    }

    g_RamCacheContext.pPath = path;
    g_RamCacheContext.pWorkBuffer = g_RamCacheWorkBuffer;
    g_RamCacheContext.workBufferSize = RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE;
    g_RamCacheContext.blockSize = RAMCACHEBLOCKSIZE;
    res = sceFiosIOFilterAdd(0, sceFiosIOFilterCache, &g_RamCacheContext);
    if (res < 0) {
        LOG_ERROR("FIOS: sceFiosIOFilterAdd failed: 0x%08x", res);
        return res;
    }

    LOG_SUCCESS("FIOS: 8 MB RAM cache active for path: %s", path);
    return 0;
}

void fios_terminate(void) {
    sceFiosTerminate();
    if (g_RamCacheWorkBuffer) {
        free(g_RamCacheWorkBuffer);
        g_RamCacheWorkBuffer = NULL;
    }
}
