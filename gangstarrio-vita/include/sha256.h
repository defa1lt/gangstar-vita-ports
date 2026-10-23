#ifndef GANGSTARRIO_SHA256_H
#define GANGSTARRIO_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct Sha256Context {
    uint8_t block[64];
    uint32_t state[8];
    uint64_t total_size;
    size_t block_size;
} Sha256Context;

void sha256_init(Sha256Context *context);
void sha256_update(Sha256Context *context, const void *data, size_t size);
void sha256_final(Sha256Context *context, uint8_t digest[32]);

#endif

