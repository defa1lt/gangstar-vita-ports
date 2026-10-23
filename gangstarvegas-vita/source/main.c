#include "diagnostics.h"
#include "game_file.h"
#include "elf_diagnostics.h"
#include "runtime.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/devctl.h>
#include <psp2/vshbridge.h>
#include <stdlib.h>

int _newlib_heap_size_user = 224 * 1024 * 1024;
int sceUserMainThreadStackSize = 512 * 1024;

int main(void) {
    if (diagnostics_init() < 0) return 1;
    LOG_INFO("Gangstar Vegas Vita 00.05: Vegas runtime");
    SceIoDevInfo storage = {0};
    int status = sceIoDevctl("ux0:", 0x3001, NULL, 0, &storage, sizeof storage);
    if (status >= 0)
        LOG_INFO("ux0 free bytes=%llu", (unsigned long long)storage.free_size);
    else
        LOG_ERROR("ux0 space query failed: 0x%x", status);
    int result = 1;
    int work[2] = {0, 0};
    if (_vshKernelSearchModuleByName("kubridge", work) < 0) {
        LOG_ERROR("kubridge.skprx is not loaded");
    } else {
        size_t size;
        void *data = game_file_read_verified(&size);
        if (data) {
            result = elf_diagnostics_run(data, size) == 0 ? 0 : 1;
            free(data);
            if (result == 0) result = runtime_start() == 0 ? 0 : 1;
        }
    }
    LOG_INFO("runtime result=%d", result);
    diagnostics_close();
    sceKernelDelayThread(5 * 1000 * 1000);
    return result;
}
