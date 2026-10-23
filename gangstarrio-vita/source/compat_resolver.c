#include "compat_resolver.h"

#include "diagnostics.h"
#include "rio_missing_imports.h"

#include <elf.h>
#include <kubridge.h>

#include <stdlib.h>
#include <string.h>

extern void *dlsym_soloader(void *handle, const char *symbol);

void *compat_symbol(const char *name) {
    for (size_t index = 0; index < rio_missing_dynlib_count; ++index) {
        if (strcmp(name, rio_missing_dynlib[index].symbol) == 0) {
            return (void *)rio_missing_dynlib[index].func;
        }
    }
    return dlsym_soloader(NULL, name);
}

int compat_validate_imports(so_module *module, int *resolved, int *missing) {
    int resolved_count = 0;
    int missing_count = 0;
    unsigned char *seen;

    if (module == NULL || module->dynsym == NULL || module->dynstr == NULL ||
        module->num_dynsym <= 0) {
        return -1;
    }

    seen = calloc((size_t)module->num_dynsym, sizeof(*seen));
    if (seen == NULL) {
        return -1;
    }

    for (int index = 0; index < module->num_reldyn + module->num_relplt;
         ++index) {
        Elf32_Rel *relocation = index < module->num_reldyn
                                    ? &module->reldyn[index]
                                    : &module->relplt[index - module->num_reldyn];
        unsigned int symbol_index = ELF32_R_SYM(relocation->r_info);
        if (symbol_index >= (unsigned int)module->num_dynsym) {
            free(seen);
            return -1;
        }

        Elf32_Sym *symbol = &module->dynsym[symbol_index];
        if (symbol->st_shndx != SHN_UNDEF || symbol->st_name == 0 ||
            seen[symbol_index]) {
            continue;
        }

        seen[symbol_index] = 1;
        const char *name = module->dynstr + symbol->st_name;
        void *address = compat_symbol(name);
        if (address == NULL) {
            ++missing_count;
            diagnostics_log("UNRESOLVED", "%s", name);
        } else {
            ++resolved_count;
            diagnostics_log("RESOLVED", "%s -> %p", name, address);
        }
    }

    free(seen);
    if (resolved != NULL) {
        *resolved = resolved_count;
    }
    if (missing != NULL) {
        *missing = missing_count;
    }
    return missing_count == 0 ? 0 : -1;
}

int compat_bind_imports(so_module *module, int *bound_relocations) {
    uintptr_t *addresses;
    int bound = 0;

    if (module == NULL || module->dynsym == NULL || module->dynstr == NULL ||
        module->num_dynsym <= 0) {
        return -1;
    }

    addresses = calloc((size_t)module->num_dynsym, sizeof(*addresses));
    if (addresses == NULL) {
        return -1;
    }

    for (int index = 0; index < module->num_reldyn + module->num_relplt;
         ++index) {
        Elf32_Rel *relocation = index < module->num_reldyn
                                    ? &module->reldyn[index]
                                    : &module->relplt[index - module->num_reldyn];
        unsigned int type = ELF32_R_TYPE(relocation->r_info);
        unsigned int symbol_index = ELF32_R_SYM(relocation->r_info);

        if (symbol_index >= (unsigned int)module->num_dynsym) {
            free(addresses);
            return -1;
        }
        if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT &&
            type != R_ARM_JUMP_SLOT) {
            continue;
        }

        Elf32_Sym *symbol = &module->dynsym[symbol_index];
        if (symbol->st_shndx != SHN_UNDEF || symbol->st_name == 0) {
            continue;
        }

        if (addresses[symbol_index] == 0) {
            const char *name = module->dynstr + symbol->st_name;
            addresses[symbol_index] = (uintptr_t)compat_symbol(name);
            if (addresses[symbol_index] == 0) {
                LOG_ERROR("cannot bind unresolved import: %s", name);
                free(addresses);
                return -1;
            }
        }

        uintptr_t *target =
            (uintptr_t *)(module->text_base + relocation->r_offset);
        uintptr_t value = type == R_ARM_ABS32
                              ? *target + addresses[symbol_index]
                              : addresses[symbol_index];
        if (kuKernelCpuUnrestrictedMemcpy(target, &value, sizeof(value)) < 0) {
            LOG_ERROR("kubridge failed to bind relocation %d", index);
            free(addresses);
            return -1;
        }
        ++bound;
    }

    free(addresses);
    if (bound_relocations != NULL) {
        *bound_relocations = bound;
    }
    return 0;
}
