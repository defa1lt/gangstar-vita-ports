#include "utils/dialog.h"

#include "diagnostics.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>

void fatal_error(const char *format, ...) {
    char message[768];
    va_list arguments;

    va_start(arguments, format);
    sceClibVsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    LOG_ERROR("so_util fatal error: %s", message);
    diagnostics_close();
    sceKernelDelayThread(5 * 1000 * 1000);
    sceKernelExitProcess(1);

    for (;;) {
        sceKernelDelayThread(1 * 1000 * 1000);
    }
}
