#include "runtime.h"
#include "offline.h"
#include "diagnostics.h"
#include "game_profile.h"
#include <elf.h>
#include "reimpl/egl.h"
#include <kubridge.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <poll.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern void gl_preload(void);
extern void gl_init(void);
FILE __sF_fake[3];
static void *network_memory;
static int bind_kernel_helpers(void) {
    static const struct { uint32_t offset, address; } patches[]=GAME_KERNEL_HELPERS;
    size_t count=sizeof(patches)/sizeof(patches[0]);
    for (size_t i=0;i<count;++i) {
        if ((patches[i].offset & 3) || so_mod.text_size<4 || patches[i].offset>so_mod.text_size-4 ||
            *(const uint32_t *)(so_mod.text_base+patches[i].offset)!=patches[i].address ||
            !vegas_kernel_helper(patches[i].address)) {
            LOG_ERROR("Kernel helper profile mismatch at 0x%x",patches[i].offset); return -1;
        }
    }
    for (size_t i=0;i<count;++i) {
        uint32_t *target=(void *)(so_mod.text_base+patches[i].offset);
        uint32_t value=vegas_kernel_helper(patches[i].address);
        if (kuKernelCpuUnrestrictedMemcpy(target,&value,sizeof(value))<0 || *target!=value) {
            LOG_ERROR("Kernel helper binding failed at 0x%x",patches[i].offset); return -1;
        }
    }
    LOG_SUCCESS("Linux ARM kernel helpers bound: %u literal pools",(unsigned)count);
    return 0;
}
static int initialize_network(void) {
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET)<0) {
        int result=sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (result<0) { LOG_ERROR("Net module load failed: 0x%x",result); return -1; }
    }
    int status=sceNetShowNetstat();
    if (status==(int)SCE_NET_ERROR_ENOTINIT) {
        network_memory=malloc(1024*1024);
        if (!network_memory) return -1;
        SceNetInitParam parameters={network_memory,1024*1024,0};
        status=sceNetInit(&parameters);
    }
    if (status<0) { LOG_ERROR("Network initialization failed: 0x%x",status); return -1; }
    int descriptors[2];
    if (pipe(descriptors)<0) { LOG_ERROR("Network pipe probe failed"); return -1; }
    struct pollfd descriptor={descriptors[0],POLLIN,0};
    int result=poll(&descriptor,1,0);
    close(descriptors[0]); close(descriptors[1]);
    if (result!=0) { LOG_ERROR("Network poll probe failed: %d",result); return -1; }
    LOG_SUCCESS("Net module initialized; pipe/poll probe passed");
    return 0;
}

int runtime_start(void) {
    if (initialize_network()<0 || vegas_offline_probe()<0) return -1;
    LOG_SUCCESS("Offline mode: game sockets and DNS disabled; local file/pipe polling available");
    __sF_fake[0] = *stdin; __sF_fake[1] = *stdout; __sF_fake[2] = *stderr;
    uintptr_t *addresses = calloc(so_mod.num_dynsym,sizeof(*addresses));
    if (!addresses) return -1;
    int unresolved=0;
    for (int i=0;i<so_mod.num_reldyn+so_mod.num_relplt;++i) {
        Elf32_Rel *rel=i<so_mod.num_reldyn?&so_mod.reldyn[i]:&so_mod.relplt[i-so_mod.num_reldyn];
        unsigned index=ELF32_R_SYM(rel->r_info);
        Elf32_Sym *symbol=&so_mod.dynsym[index];
        if (symbol->st_shndx || !symbol->st_name || addresses[index]) continue;
        const char *name=so_mod.dynstr+symbol->st_name;
        addresses[index]=(uintptr_t)vegas_symbol(name);
        if (!addresses[index]) { LOG_ERROR("UNRESOLVED: %s",name); ++unresolved; }
    }
    if (unresolved) { free(addresses); LOG_ERROR("runtime imports missing=%d",unresolved); return -1; }
    int bound=0;
    for (int i=0;i<so_mod.num_reldyn+so_mod.num_relplt;++i) {
        Elf32_Rel *rel=i<so_mod.num_reldyn?&so_mod.reldyn[i]:&so_mod.relplt[i-so_mod.num_reldyn];
        unsigned index=ELF32_R_SYM(rel->r_info),type=ELF32_R_TYPE(rel->r_info);
        Elf32_Sym *symbol=&so_mod.dynsym[index];
        if (symbol->st_shndx || !symbol->st_name) continue;
        uintptr_t *target=(void *)(so_mod.text_base+rel->r_offset);
        uintptr_t value=addresses[index]+(type==R_ARM_ABS32?*target:0);
        if (kuKernelCpuUnrestrictedMemcpy(target,&value,sizeof(value))<0 || *target!=value) {
            free(addresses); LOG_ERROR("import relocation failed: %d",i); return -1;
        }
        ++bound;
    }
    free(addresses);
    LOG_SUCCESS("Runtime imports bound: %d",bound);
    if (bind_kernel_helpers()<0 || vegas_bind_texture_policy()<0 || vegas_offline_bind_clock()<0) return -1;
    scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);
    so_flush_caches(&so_mod);
    LOG_INFO("Initialize graphics");
    gl_preload();
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!bbr_eglInitialize(display,NULL,NULL)) return -1;
    EGLConfig config; EGLint count;
    if (!bbr_eglChooseConfig(display,NULL,&config,1,&count) || !count) return -1;
    EGLContext context = bbr_eglCreateContext(display,config,EGL_NO_CONTEXT,NULL);
    EGLSurface surface = bbr_eglCreateWindowSurface(display,config,NULL,NULL);
    if (!context || !surface || !bbr_eglMakeCurrent(display,surface,surface,context)) return -1;
    LOG_SUCCESS("Graphics initialized and current");
    for (int i=0;i<so_mod.num_init_array;++i) {
        uintptr_t constructor=(uintptr_t)so_mod.init_array[i];
        if (!constructor || constructor==UINT32_MAX) continue;
        LOG_INFO("Constructor %d/%d address=0x%08x",i+1,so_mod.num_init_array,(unsigned)constructor);
        so_mod.init_array[i]();
    }
    LOG_SUCCESS("ELF constructors complete");
    return vegas_java_start();
}
