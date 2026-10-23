#ifndef GMV_FILECACHE_H
#define GMV_FILECACHE_H

#include <stdint.h>

/* Boot snapshot of GAME_DATA_PATH, including directories and hidden files.
 * A failed or incomplete scan never makes absence authoritative. */
void filecache_init(void);

/* Absolute Vita path: 1 = cached metadata, 0 = known absent, -1 = unknown.
 * Unknown includes paths outside the asset root and mutated subtrees. */
int filecache_lookup(const char *path, uint32_t *size, uint16_t *mode);

/* Call before filesystem mutations. Invalidates the path/subtree and parent
 * directory metadata without breaking hash probe chains or unrelated assets. */
void filecache_invalidate(const char *path);

#endif
