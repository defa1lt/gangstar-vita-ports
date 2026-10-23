#ifndef GANGSTARRIO_DIAGNOSTICS_H
#define GANGSTARRIO_DIAGNOSTICS_H

#include <stdint.h>

int diagnostics_init(void);
void diagnostics_close(void);
void diagnostics_flush(void);
void diagnostics_log(const char *level, const char *format, ...);
void diagnostics_render_start(void);
void diagnostics_render_frame(unsigned frame, uint64_t step_us, uint64_t swap_us);
void diagnostics_render_progress(void);

#ifdef HAVE_SHARK_LOG
void diagnostics_gpu_progress(unsigned limit);
void diagnostics_compiler_trace_begin(void);
void diagnostics_compiler_trace_end(void);
void diagnostics_compiler_checkpoint(const char *phase, int completed);
#endif

#define LOG_INFO(...) diagnostics_log("INFO", __VA_ARGS__)
#define LOG_ERROR(...) diagnostics_log("ERROR", __VA_ARGS__)
#define LOG_SUCCESS(...) diagnostics_log("SUCCESS", __VA_ARGS__)

#endif
