#ifndef GANGSTARRIO_GAME_PROFILE_H
#define GANGSTARRIO_GAME_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#define RIO_DATA_PATH "ux0:data/gangstarrio/"
#define RIO_GAME_SO_PATH RIO_DATA_PATH "libgl2jni.so"
#define RIO_LOG_PATH RIO_DATA_PATH "loader.log"

#define RIO_GAME_SO_SIZE ((uint64_t)15331160)
#define RIO_GAME_RELOCATION_COUNT 74951
#define RIO_GAME_IMPORT_RELOCATION_COUNT 6830
#define RIO_GAME_IMPORT_COUNT 365
#define RIO_GAME_JNI_EXPORT_COUNT 51
#define RIO_GAME_SO_SHA256_HEX \
    "c7268efa9cc93da048bc44c5a319531bc6b72668a764b779d17c71bff845cb94"

extern const uint8_t RIO_GAME_SO_SHA256[32];

#endif
