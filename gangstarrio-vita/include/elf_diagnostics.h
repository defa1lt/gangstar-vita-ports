#ifndef GANGSTARRIO_ELF_DIAGNOSTICS_H
#define GANGSTARRIO_ELF_DIAGNOSTICS_H

typedef enum ElfDiagnosticStatus {
    ELF_DIAGNOSTIC_OK = 0,
    ELF_DIAGNOSTIC_KUBRIDGE_MISSING,
    ELF_DIAGNOSTIC_LOAD_FAILED,
    ELF_DIAGNOSTIC_INVALID_TABLES,
    ELF_DIAGNOSTIC_OUT_OF_MEMORY,
    ELF_DIAGNOSTIC_UNSUPPORTED_RELOCATION,
    ELF_DIAGNOSTIC_RELOCATE_FAILED,
    ELF_DIAGNOSTIC_UNRESOLVED_IMPORTS,
    ELF_DIAGNOSTIC_BIND_FAILED,
    ELF_DIAGNOSTIC_PATCH_FAILED,
} ElfDiagnosticStatus;

ElfDiagnosticStatus elf_diagnostics_run(void);
const char *elf_diagnostic_status_string(ElfDiagnosticStatus status);
int elf_runtime_run(void);
void elf_runtime_handle_key(int keycode, int pressed);
void elf_runtime_handle_touch(int pointer_id, int x, int y, int action);
void elf_runtime_handle_analog(int stick, float x, float y);

#endif
