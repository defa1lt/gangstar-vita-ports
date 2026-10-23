#include "elf_diagnostics.h"
#include "game_profile.h"
#include "diagnostics.h"
#include <so_util.h>
#include <elf.h>
#include <stdlib.h>
#include <string.h>

so_module so_mod;
static uintptr_t game_exidx;
static int game_exidx_count;
extern const uint32_t __exidx_start[], __exidx_end[];
uintptr_t __gnu_Unwind_Find_exidx(uintptr_t address, int *count) {
    if (!count) return 0;
    if (address>=so_mod.text_base && address-so_mod.text_base<so_mod.text_size) {
        *count=game_exidx_count; return game_exidx;
    }
    *count=(__exidx_end-__exidx_start)/2;
    return (uintptr_t)__exidx_start;
}


static size_t mapped_remaining(uintptr_t address) {
    if (address >= so_mod.text_base && address - so_mod.text_base < so_mod.text_size)
        return so_mod.text_size - (address - so_mod.text_base);
    for (int i = 0; i < so_mod.n_data; ++i)
        if (address >= so_mod.data_base[i] && address - so_mod.data_base[i] < so_mod.data_size[i])
            return so_mod.data_size[i] - (address - so_mod.data_base[i]);
    return 0;
}
static const char *symbol_name(const Elf32_Sym *symbol) {
    uintptr_t base = (uintptr_t)so_mod.dynstr;
    if (symbol->st_name > UINTPTR_MAX - base) return NULL;
    const char *name = (const char *)(base + symbol->st_name);
    return memchr(name, 0, mapped_remaining((uintptr_t)name)) ? name : NULL;
}
static Elf32_Rel *relocation(int i) {
    return i < so_mod.num_reldyn ? &so_mod.reldyn[i] : &so_mod.relplt[i - so_mod.num_reldyn];
}

int elf_diagnostics_run(void *data, size_t size) {
    int result = so_mem_load(&so_mod, data, size, 0x98000000);
    if (result < 0 || !so_mod.text_base || so_mod.n_data < 1 || so_mod.n_data > MAX_DATA_SEG) {
        LOG_ERROR("ELF mapping failed: 0x%x", result); return -1;
    }
    LOG_SUCCESS("ELF mapped: text=0x%08x size=%u data_segments=%d", (unsigned)so_mod.text_base,
                (unsigned)so_mod.text_size, so_mod.n_data);
    if (so_mod.num_dynsym != GAME_ELF_SYMBOLS || so_mod.num_reldyn < 0 || so_mod.num_relplt < 0 ||
        so_mod.num_reldyn + so_mod.num_relplt != GAME_ELF_RELOCATIONS ||
        so_mod.num_init_array != GAME_ELF_CONSTRUCTORS ||
        mapped_remaining((uintptr_t)so_mod.dynsym) < sizeof(Elf32_Sym) * GAME_ELF_SYMBOLS ||
        mapped_remaining((uintptr_t)so_mod.reldyn) < sizeof(Elf32_Rel) * so_mod.num_reldyn ||
        mapped_remaining((uintptr_t)so_mod.relplt) < sizeof(Elf32_Rel) * so_mod.num_relplt ||
        mapped_remaining((uintptr_t)so_mod.init_array) < sizeof(void *) * GAME_ELF_CONSTRUCTORS) {
        LOG_ERROR("mapped ELF tables do not match profile"); return -1;
    }
    const Elf32_Ehdr *file_header=data;
    if (size<sizeof(*file_header) || file_header->e_phentsize!=sizeof(Elf32_Phdr) ||
        file_header->e_phoff>size || file_header->e_phnum>(size-file_header->e_phoff)/sizeof(Elf32_Phdr)) {
        LOG_ERROR("Invalid ELF program headers"); return -1;
    }
    const Elf32_Phdr *file_segments=(const void *)((const unsigned char *)data+file_header->e_phoff);
    for (int i=0;i<file_header->e_phnum;++i) {
        const Elf32_Phdr *header=&file_segments[i];
        if (header->p_type!=0x70000001u) continue;
        uintptr_t address=so_mod.text_base+header->p_vaddr;
        if (game_exidx || header->p_vaddr>UINTPTR_MAX-so_mod.text_base ||
            header->p_memsz%8 || mapped_remaining(address)<header->p_memsz) {
            LOG_ERROR("Invalid ARM exception index"); return -1;
        }
        game_exidx=address; game_exidx_count=header->p_memsz/8;
    }
    if (!game_exidx || !game_exidx_count) { LOG_ERROR("Missing ARM exception index"); return -1; }
    LOG_SUCCESS("ARM exception index retained: address=0x%08x count=%d",(unsigned)game_exidx,game_exidx_count);
    unsigned char *seen = calloc(GAME_ELF_SYMBOLS, 1);
    uint32_t *original = malloc(sizeof(uint32_t) * GAME_ELF_RELOCATIONS);
    if (!seen || !original) { free(seen); free(original); return -1; }
    int imports = 0, import_relocations = 0, jni = 0, internal = 0;
    for (int i = 0; i < GAME_ELF_SYMBOLS; ++i) {
        const char *name = symbol_name(&so_mod.dynsym[i]);
        if (!name) goto invalid;
        if (so_mod.dynsym[i].st_shndx != SHN_UNDEF &&
            (!strcmp(name, "JNI_OnLoad") || !strncmp(name, "Java_", 5))) ++jni;
    }
    for (int i = 0; i < GAME_ELF_RELOCATIONS; ++i) {
        Elf32_Rel *rel = relocation(i);
        unsigned sym_index = ELF32_R_SYM(rel->r_info);
        unsigned type = ELF32_R_TYPE(rel->r_info);
        if (sym_index >= GAME_ELF_SYMBOLS || rel->r_offset > UINTPTR_MAX - so_mod.text_base)
            goto invalid;
        uintptr_t target = so_mod.text_base + rel->r_offset;
        if ((target & 3) || mapped_remaining(target) < 4) goto invalid;
        original[i] = *(const uint32_t *)target;
        Elf32_Sym *sym = &so_mod.dynsym[sym_index];
        if (type == R_ARM_RELATIVE && sym_index == 0) { ++internal; continue; }
        if ((type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) ||
            sym->st_shndx != SHN_UNDEF || !sym->st_name) goto invalid;
        ++import_relocations;
        if (!seen[sym_index]) {
            seen[sym_index] = 1;
            ++imports;
            diagnostics_log("UNBOUND", "%s", symbol_name(sym));
        }
    }
    if (imports != GAME_ELF_IMPORTS || import_relocations != GAME_ELF_IMPORT_RELOCATIONS ||
        jni != GAME_ELF_JNI_EXPORTS || internal + import_relocations != GAME_ELF_RELOCATIONS)
        goto invalid;
    LOG_INFO("ELF audit: relocations=%d imports=%d import_relocations=%d JNI_exports=%d constructors=%d",
             GAME_ELF_RELOCATIONS, imports, import_relocations, jni, so_mod.num_init_array);
    if (so_relocate(&so_mod) < 0) goto invalid;
    for (int i = 0; i < GAME_ELF_RELOCATIONS; ++i) {
        Elf32_Rel *rel = relocation(i);
        uint32_t expected = original[i];
        if (ELF32_R_TYPE(rel->r_info) == R_ARM_RELATIVE) expected += so_mod.text_base;
        if (*(const uint32_t *)(so_mod.text_base + rel->r_offset) != expected) {
            LOG_ERROR("relocation readback failed: index=%d offset=0x%x", i, rel->r_offset);
            goto invalid;
        }
    }
    so_flush_caches(&so_mod);
    LOG_SUCCESS("M1 complete: internal_relocations=%d imports_unbound=%d constructors_not_run=%d",
                internal, import_relocations, so_mod.num_init_array);
    free(seen); free(original); return 0;
invalid:
    LOG_ERROR("ELF audit or relocation failed");
    free(seen); free(original); return -1;
}
