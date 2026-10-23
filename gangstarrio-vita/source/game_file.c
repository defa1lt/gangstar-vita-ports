#include "game_file.h"

#include "diagnostics.h"
#include "game_profile.h"
#include "sha256.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stdint.h>
#include <string.h>

#define RIO_VERIFY_CACHE_PATH RIO_DATA_PATH ".libgl2jni-verified"
#define RIO_VERIFY_CACHE_MAGIC 0x47535256u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t size;
    SceDateTime modified;
    uint8_t digest[32];
} GameFileVerifyCache;

static int verification_cache_matches(const SceIoStat *stat) {
    GameFileVerifyCache cache;
    SceUID file = sceIoOpen(RIO_VERIFY_CACHE_PATH, SCE_O_RDONLY, 0);
    if (file < 0) return 0;
    int bytes = sceIoRead(file, &cache, sizeof(cache));
    sceIoClose(file);
    return bytes == (int)sizeof(cache) &&
           cache.magic == RIO_VERIFY_CACHE_MAGIC &&
           cache.version == 1 &&
           cache.size == (uint64_t)stat->st_size &&
           memcmp(&cache.modified, &stat->st_mtime,
                  sizeof(cache.modified)) == 0 &&
           memcmp(cache.digest, RIO_GAME_SO_SHA256,
                  sizeof(cache.digest)) == 0;
}

static void update_verification_cache(const SceIoStat *stat) {
    GameFileVerifyCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.magic = RIO_VERIFY_CACHE_MAGIC;
    cache.version = 1;
    cache.size = (uint64_t)stat->st_size;
    cache.modified = stat->st_mtime;
    memcpy(cache.digest, RIO_GAME_SO_SHA256, sizeof(cache.digest));

    SceUID file = sceIoOpen(RIO_VERIFY_CACHE_PATH,
                           SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0666);
    if (file >= 0) {
        (void)sceIoWrite(file, &cache, sizeof(cache));
        sceIoClose(file);
    }
}

GameFileStatus game_file_verify(void) {
    uint8_t buffer[32 * 1024];
    uint8_t digest[32];
    Sha256Context sha256;
    SceIoStat stat;

    if (sceIoGetstat(RIO_GAME_SO_PATH, &stat) < 0) {
        LOG_ERROR("missing game binary: %s", RIO_GAME_SO_PATH);
        return GAME_FILE_MISSING;
    }

    if ((uint64_t)stat.st_size != RIO_GAME_SO_SIZE) {
        LOG_ERROR("wrong game binary size: %llu (expected %llu)",
                  (unsigned long long)stat.st_size,
                  (unsigned long long)RIO_GAME_SO_SIZE);
        return GAME_FILE_WRONG_SIZE;
    }

    if (verification_cache_matches(&stat)) {
        LOG_SUCCESS("game binary verification cache hit: %s",
                    RIO_GAME_SO_SHA256_HEX);
        return GAME_FILE_OK;
    }

    SceUID file = sceIoOpen(RIO_GAME_SO_PATH, SCE_O_RDONLY, 0);
    if (file < 0) {
        LOG_ERROR("cannot open game binary: 0x%08x", file);
        return GAME_FILE_READ_ERROR;
    }

    sha256_init(&sha256);
    for (;;) {
        int read_size = sceIoRead(file, buffer, sizeof(buffer));
        if (read_size < 0) {
            LOG_ERROR("cannot read game binary: 0x%08x", read_size);
            sceIoClose(file);
            return GAME_FILE_READ_ERROR;
        }
        if (read_size == 0) {
            break;
        }
        sha256_update(&sha256, buffer, (size_t)read_size);
    }
    sceIoClose(file);
    sha256_final(&sha256, digest);

    if (memcmp(digest, RIO_GAME_SO_SHA256, sizeof(digest)) != 0) {
        char actual[65];
        static const char hex[] = "0123456789abcdef";
        size_t index;
        for (index = 0; index < sizeof(digest); ++index) {
            actual[index * 2] = hex[digest[index] >> 4];
            actual[index * 2 + 1] = hex[digest[index] & 0x0f];
        }
        actual[64] = '\0';
        LOG_ERROR("unsupported game binary SHA-256: %s", actual);
        return GAME_FILE_WRONG_HASH;
    }

    update_verification_cache(&stat);
    LOG_SUCCESS("game binary verified: %s", RIO_GAME_SO_SHA256_HEX);
    return GAME_FILE_OK;
}

const char *game_file_status_string(GameFileStatus status) {
    switch (status) {
        case GAME_FILE_OK:
            return "ok";
        case GAME_FILE_MISSING:
            return "missing";
        case GAME_FILE_WRONG_SIZE:
            return "wrong size";
        case GAME_FILE_READ_ERROR:
            return "read error";
        case GAME_FILE_WRONG_HASH:
            return "wrong SHA-256";
        default:
            return "unknown";
    }
}
