/*
 * Compatibility functions for Gangstar Rio imports absent from the pinned
 * MC2 loader table. The public layouts match Android 2.3 ARM Bionic.
 */

#include "rio_missing_imports.h"

#include "diagnostics.h"

#include <psp2/io/devctl.h>
#include <psp2/io/fcntl.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

typedef uint32_t bionic_uid_t;
typedef uint32_t bionic_gid_t;

typedef struct BionicPasswd {
    char *pw_name;
    char *pw_passwd;
    bionic_uid_t pw_uid;
    bionic_gid_t pw_gid;
    char *pw_dir;
    char *pw_shell;
} BionicPasswd;

typedef struct BionicFsid {
    int32_t value[2];
} BionicFsid;

typedef struct BionicStatfs {
    uint32_t f_type;
    uint32_t f_bsize;
    uint32_t f_blocks;
    uint32_t f_bfree;
    uint32_t f_bavail;
    uint32_t f_files;
    uint32_t f_ffree;
    BionicFsid f_fsid;
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_spare[5];
} BionicStatfs;

_Static_assert(sizeof(BionicPasswd) == 24, "Android ARM passwd ABI changed");
_Static_assert(sizeof(BionicStatfs) == 64, "Android ARM statfs ABI changed");

enum {
    RIO_ANDROID_APP_ID = 10000,
    RIO_STATFS_BLOCK_SIZE = 4096,
};

static char syslog_ident[64];

static bionic_uid_t rio_getuid(void) {
    return RIO_ANDROID_APP_ID;
}

static bionic_uid_t rio_geteuid(void) {
    return RIO_ANDROID_APP_ID;
}

static bionic_gid_t rio_getgid(void) {
    return RIO_ANDROID_APP_ID;
}

static bionic_gid_t rio_getegid(void) {
    return RIO_ANDROID_APP_ID;
}

static BionicPasswd *rio_getpwuid(bionic_uid_t uid) {
    static char name[] = "app_0";
    static char password[] = "x";
    static char home[] = "/data/data/com.gameloft.android.ANMP.GloftG4HM";
    static char shell[] = "/system/bin/sh";
    static BionicPasswd passwd_entry = {
        name, password, RIO_ANDROID_APP_ID, RIO_ANDROID_APP_ID, home, shell,
    };

    passwd_entry.pw_uid = uid;
    return &passwd_entry;
}

static int rio_pread(int fd, void *buffer, uint32_t count, int32_t offset) {
    return sceIoPread(fd, buffer, count, offset);
}

static uint32_t blocks_from_bytes(uint64_t bytes) {
    uint64_t blocks = bytes / RIO_STATFS_BLOCK_SIZE;
    return blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks;
}

static int rio_statfs(const char *path, BionicStatfs *status) {
    SceIoDevInfo device;
    int result;

    if (path == NULL || status == NULL) {
        return -1;
    }

    memset(&device, 0, sizeof(device));
    result = sceIoDevctl("ux0:", 0x3001, NULL, 0, &device, sizeof(device));
    if (result < 0) {
        LOG_ERROR("statfs(%s): sceIoDevctl failed: 0x%08x", path, result);
        return -1;
    }

    memset(status, 0, sizeof(*status));
    status->f_type = 0xef53;
    status->f_bsize = RIO_STATFS_BLOCK_SIZE;
    status->f_blocks = blocks_from_bytes((uint64_t)device.max_size);
    status->f_bfree = blocks_from_bytes((uint64_t)device.free_size);
    status->f_bavail = status->f_bfree;
    status->f_files = UINT32_MAX;
    status->f_ffree = UINT32_MAX;
    status->f_namelen = 255;
    status->f_frsize = RIO_STATFS_BLOCK_SIZE;
    return 0;
}

static void rio_openlog(const char *ident, int option, int facility) {
    (void)option;
    (void)facility;
    if (ident == NULL) {
        syslog_ident[0] = '\0';
        return;
    }
    snprintf(syslog_ident, sizeof(syslog_ident), "%s", ident);
}

static void rio_closelog(void) {
    syslog_ident[0] = '\0';
}

static void rio_syslog(int priority, const char *format, ...) {
    char message[640];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    diagnostics_log("ANDROID", "%s%s%s (priority=%d)",
                    syslog_ident[0] != '\0' ? syslog_ident : "syslog",
                    syslog_ident[0] != '\0' ? ": " : "",
                    message, priority);
}

static const char *rio_z_error(int error) {
    return zError(error);
}

static const char *rio_zlib_version(void) {
    return zlibVersion();
}

static void rio_cxa_pure_virtual(void) {
    diagnostics_log("COMPAT", "WARNING: __cxa_pure_virtual called - suppressed abort, safely returning");
    diagnostics_flush();
}

static void rio_terminate(void) {
    diagnostics_log("COMPAT", "WARNING: std::terminate() called - suppressed abort, safely returning");
    diagnostics_flush();
}

so_default_dynlib rio_missing_dynlib[] = {
    {"__cxa_deleted_virtual", (uintptr_t)&rio_cxa_pure_virtual},
    {"__cxa_pure_virtual", (uintptr_t)&rio_cxa_pure_virtual},
    {"_ZSt9terminatev", (uintptr_t)&rio_terminate},
    {"closelog", (uintptr_t)&rio_closelog},
    {"getegid", (uintptr_t)&rio_getegid},
    {"geteuid", (uintptr_t)&rio_geteuid},
    {"getgid", (uintptr_t)&rio_getgid},
    {"getpwuid", (uintptr_t)&rio_getpwuid},
    {"getuid", (uintptr_t)&rio_getuid},
    {"openlog", (uintptr_t)&rio_openlog},
    {"pread", (uintptr_t)&rio_pread},
    {"statfs", (uintptr_t)&rio_statfs},
    {"syslog", (uintptr_t)&rio_syslog},
    {"zError", (uintptr_t)&rio_z_error},
    {"zlibVersion", (uintptr_t)&rio_zlib_version},
};

const size_t rio_missing_dynlib_count =
    sizeof(rio_missing_dynlib) / sizeof(rio_missing_dynlib[0]);
