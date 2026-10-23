#include "sha256.h"
#include <stdio.h>
int main(void) {
    Sha256Context context;
    unsigned char block[4096], digest[32];
    sha256_init(&context);
    size_t count;
    while ((count = fread(block, 1, sizeof block, stdin)) != 0)
        sha256_update(&context, block, count);
    if (ferror(stdin)) return 1;
    sha256_final(&context, digest);
    for (unsigned i = 0; i < sizeof digest; ++i) printf("%02x", digest[i]);
    puts("");
    return 0;
}
