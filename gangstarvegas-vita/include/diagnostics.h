#pragma once
int diagnostics_init(void);
void diagnostics_log(const char *level, const char *format, ...) __attribute__((format(printf, 2, 3)));
void diagnostics_flush(void);
void diagnostics_close(void);
void diagnostics_watch_start(void);
void diagnostics_watch_stop(void);
int diagnostics_wait_enter(const char *operation, const void *subject, const void *caller);
void diagnostics_wait_leave(int tracked);
#define LOG_INFO(...) diagnostics_log("INFO", __VA_ARGS__)
#define LOG_SUCCESS(...) diagnostics_log("SUCCESS", __VA_ARGS__)
#define LOG_ERROR(...) diagnostics_log("ERROR", __VA_ARGS__)
