#ifndef GANGSTARRIO_RUNTIME_PATCH_H
#define GANGSTARRIO_RUNTIME_PATCH_H

#include <so_util.h>

int runtime_patch_android_kuser(so_module *module);
int runtime_patch_install_trampoline(uintptr_t address, uintptr_t target);
int runtime_patch_game(so_module *module);
void runtime_patch_dump_state(void);

#endif
