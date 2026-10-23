#pragma once
#include <so_util.h>
#include <stdint.h>
extern so_module so_mod;
int runtime_start(void);
void *vegas_symbol(const char *name);
int vegas_java_start(void);
uintptr_t vegas_kernel_helper(uintptr_t address);
unsigned vegas_presentations(void);
int vegas_bind_texture_policy(void);
int vegas_bind_player_probe(void);
void vegas_trace_player_state(unsigned frame);
