#include "diagnostics.h"
#include "elf_diagnostics.h"
#include "game_file.h"
#include "game_profile.h"
#include "kubridge.h"
#include "fios.h"
#include <reimpl/io.h>
#ifdef HAVE_SHARK_LOG
#include <reimpl/pthr_test.h>
#endif

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <string.h>

int _newlib_heap_size_user = 224 * 1024 * 1024;

/* The first runtime dump showed the default 256 KiB main-thread stack fully
 * consumed during native/VitaGL startup. */
int sceUserMainThreadStackSize = 4 * 1024 * 1024;

static void log_loaded_modules(void) {
    SceUID modlist[128];
    SceSize modcount = sizeof(modlist);
    if (sceKernelGetModuleList(0xff, modlist, &modcount) >= 0) {
        LOG_INFO("Loaded system modules (%u total):", (unsigned int)modcount);
        for (SceSize i = 0; i < modcount; i++) {
            SceKernelModuleInfo info;
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelGetModuleInfo(modlist[i], &info) >= 0) {
                LOG_INFO("  [%u] %s (text=0x%08x size=0x%x)",
                         (unsigned int)modlist[i],
                         info.module_name,
                         (unsigned int)info.segments[0].vaddr,
                         (unsigned int)info.segments[0].memsz);
            }
        }
    }
}

static void app_abort_handler(KuKernelAbortContext *ctx) {
    static int abort_counter = 0;
    ++abort_counter;

    diagnostics_log("ABORT", "CPU Exception #%d abortType=%u pc=0x%08x lr=0x%08x sp=0x%08x",
                    abort_counter, ctx->abortType, ctx->pc, ctx->lr, ctx->sp);
    diagnostics_log("ABORT", "FSR=0x%08x FAR=0x%08x SPSR=0x%08x FPSCR=0x%08x FPEXC=0x%08x",
                    ctx->FSR, ctx->FAR, ctx->SPSR, ctx->FPSCR, ctx->FPEXC);
    diagnostics_log("ABORT", "r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x",
                    ctx->r0, ctx->r1, ctx->r2, ctx->r3);
    diagnostics_log("ABORT", "r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x",
                    ctx->r4, ctx->r5, ctx->r6, ctx->r7);
    diagnostics_log("ABORT", "r8=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x r12=0x%08x",
                    ctx->r8, ctx->r9, ctx->r10, ctx->r11, ctx->r12);

    SceUID tid = sceKernelGetThreadId();
    SceKernelThreadInfo thread_info;
    memset(&thread_info, 0, sizeof(thread_info));
    thread_info.size = sizeof(thread_info);
    if (sceKernelGetThreadInfo(tid, &thread_info) >= 0) {
        diagnostics_log("ABORT", "thread uid=0x%08x name=%s stack=0x%08x size=0x%x",
                        tid, thread_info.name, (unsigned int)thread_info.stack,
                        (unsigned int)thread_info.stackSize);
    } else {
        diagnostics_log("ABORT", "thread uid=0x%08x (info unavailable)", tid);
    }

    /* Worker stacks are commonly allocated above 0x97000000.  The previous
     * main-thread-only range silently skipped the stack for pthread crashes. */
    if (ctx->sp >= 0x80000000 && ctx->sp < 0xA0000000) {
        uint32_t *stack = (uint32_t *)ctx->sp;
        for (int i = 0; i < 64; i += 4) {
            diagnostics_log("ABORT", "sp[+%02x]: 0x%08x 0x%08x 0x%08x 0x%08x",
                            i * 4, stack[i], stack[i+1], stack[i+2], stack[i+3]);
        }
    }
    io_dump_recent_reads();
    diagnostics_flush();

    /* The supported binary's CPU task runner returns to 0x9881e3c0 after
     * calling ITask::execute through vtable slot 4.  Preserve the corrupt
     * task identity in the log before handing the exception back to Vita. */
    if (ctx->abortType == KU_KERNEL_ABORT_TYPE_PREFETCH_ABORT &&
        ctx->lr == 0x9881e3c0) {
        diagnostics_log("ABORT",
                        "invalid CPU task callback: handler=0x%08x task=0x%08x "
                        "vtable=0x%08x callback=0x%08x",
                        ctx->r4, ctx->r0, ctx->r3, ctx->pc);
    }

    /* Resuming at LR after a failed virtual call leaves the task object in a
     * corrupt state.  The next ITask::done access then faults forever while
     * the audio thread keeps running, which presented as a black screen.
     * Release our logger so the original exception is reported as a normal
     * Vita crash instead of being converted into an infinite loop. */
    diagnostics_log("ABORT", "fatal exception: releasing diagnostic handler");
    diagnostics_flush();
    kuKernelReleaseAbortHandler();
}

int main(void) {
    /* Match the stable Gameloft loader setup used by 9mm and MC2. */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    (void)diagnostics_init();
    int abort_ret = kuKernelRegisterAbortHandler(app_abort_handler, NULL);
    LOG_INFO("kuKernelRegisterAbortHandler result: %d", abort_ret);
    LOG_INFO("Gangstar Rio Vita runtime bring-up %s (JNI reference lifetime repair)",
             RIO_VERSION);
    LOG_INFO("VitaGL backend: indexed client range copies");
    LOG_INFO("data path: %s", RIO_DATA_PATH);

    /* The FIOS2 RAM-cache filter corrupts its cached read state under Rio's
     * concurrent texture streaming. The failure presents in sceClibMemcpy
     * with pointers in FIOS's 0x62000000 mapping. It did not improve the
     * measured load time on Vita storage, so keep ordinary buffered I/O. */
    LOG_INFO("FIOS: RAM cache disabled (concurrent streaming safety)");

    log_loaded_modules();

    SceKernelFreeMemorySizeInfo mem_info;
    mem_info.size = sizeof(mem_info);
    if (sceKernelGetFreeMemorySize(&mem_info) >= 0) {
        LOG_INFO("startup memory: free_user=%d KB, free_cdram=%d KB, free_phycont=%d KB",
                 mem_info.size_user / 1024,
                 mem_info.size_cdram / 1024,
                 mem_info.size_phycont / 1024);
    }
    diagnostics_flush();

#ifdef HAVE_SHARK_LOG
    if (pthr_run_native_tests() != 0) {
        LOG_ERROR("native pthread checks failed; game startup stopped");
        diagnostics_flush();
        return 1;
    }
#endif

    GameFileStatus status = game_file_verify();
    if (status != GAME_FILE_OK) {
        LOG_ERROR("preflight stopped: %s", game_file_status_string(status));
        diagnostics_close();
        sceKernelDelayThread(5 * 1000 * 1000);
        return 1;
    }

    ElfDiagnosticStatus elf_status = elf_diagnostics_run();
    if (elf_status != ELF_DIAGNOSTIC_OK) {
        LOG_ERROR("M1 stopped: %s", elf_diagnostic_status_string(elf_status));
        diagnostics_close();
        sceKernelDelayThread(5 * 1000 * 1000);
        return 1;
    }

    return elf_runtime_run();
}
