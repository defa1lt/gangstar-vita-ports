#pragma once
#include <stddef.h>
#include <stdint.h>
void vegas_md5(const void *data, size_t size, uint8_t digest[16]);
void vegas_md5_hex(const void *data, size_t size, char hex[33]);
