#include "game_file.h"
#include "game_profile.h"
#include "diagnostics.h"
#include "sha256.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdlib.h>
#include <string.h>

void *game_file_read_verified(size_t *size) {
    *size = 0;
    SceIoStat st;
    if (sceIoGetstat(GAME_SO_PATH, &st) < 0 || st.st_size != GAME_SO_SIZE) {
        LOG_ERROR("missing or wrong-size binary: %s; expected %u bytes", GAME_SO_PATH, GAME_SO_SIZE);
        return NULL;
    }
    SceUID fd = sceIoOpen(GAME_SO_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) { LOG_ERROR("open failed: 0x%x", fd); return NULL; }
    unsigned char *data = malloc(GAME_SO_SIZE);
    if (!data) { sceIoClose(fd); LOG_ERROR("cannot allocate source buffer"); return NULL; }
    Sha256Context hash;
    sha256_init(&hash);
    size_t total = 0;
    while (total < GAME_SO_SIZE) {
        size_t chunk = GAME_SO_SIZE - total;
        if (chunk > 1024 * 1024) chunk = 1024 * 1024;
        int count = sceIoRead(fd, data + total, chunk);
        if (count <= 0) {
            LOG_ERROR("read failed at %u: %d", (unsigned)total, count);
            sceIoClose(fd); free(data); return NULL;
        }
        sha256_update(&hash, data + total, count);
        total += count;
    }
    unsigned char extra;
    int tail = sceIoRead(fd, &extra, 1);
    sceIoClose(fd);
    uint8_t digest[32];
    sha256_final(&hash, digest);
    if (tail != 0 || memcmp(digest, GAME_SO_SHA256, sizeof digest)) {
        LOG_ERROR("binary does not match the Google Play 1.7.0g SHA-256 profile");
        free(data); return NULL;
    }
    *size = total;
    LOG_SUCCESS("exact game binary verified: %u bytes", (unsigned)total);
    return data;
}
