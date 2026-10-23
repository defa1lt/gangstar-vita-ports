#ifndef VEGAS_OFFLINE_H
#define VEGAS_OFFLINE_H
#include <stdint.h>
void *vegas_offline_symbol(const char *name);
int vegas_offline_probe(void);
typedef void (*VegasTimeCallback)(int, const void *, int, void *);
int vegas_offline_timestamp(void *, int32_t *, int, VegasTimeCallback, void *);
int vegas_offline_bind_clock(void);
#endif
