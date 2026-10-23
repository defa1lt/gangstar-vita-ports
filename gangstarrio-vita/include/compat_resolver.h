#ifndef GANGSTARRIO_COMPAT_RESOLVER_H
#define GANGSTARRIO_COMPAT_RESOLVER_H

#include <so_util.h>

void *compat_symbol(const char *name);
int compat_validate_imports(so_module *module, int *resolved, int *missing);
int compat_bind_imports(so_module *module, int *bound_relocations);

#endif
