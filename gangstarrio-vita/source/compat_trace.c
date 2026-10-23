#include "diagnostics.h"
#include "elf_diagnostics.h"

#include "reimpl/controls.h"

#include <psp2/kernel/clib.h>

#include <stdarg.h>

void port_trace(const char *format, ...) {
    char message[768];
    va_list arguments;

    va_start(arguments, format);
    sceClibVsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    diagnostics_log("COMPAT", "%s", message);
}

void port_trace_flush(void) {
    diagnostics_flush();
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    diagnostics_log("INPUT", "button code=%d action=%s", keycode,
                    action == CONTROLS_ACTION_UP ? "up" : "down");
    elf_runtime_handle_key(keycode, action != CONTROLS_ACTION_UP);
}

void controls_handler_touch(int32_t id, float x, float y,
                            ControlsAction action) {
    elf_runtime_handle_touch(id, (int)x, (int)y, (int)action);
}

void controls_handler_analog(ControlsStickId which, float x, float y,
                             ControlsAction action) {
    static unsigned move_samples[2];
    if (action != CONTROLS_ACTION_MOVE || move_samples[which]++ < 8) {
        diagnostics_log("INPUT", "stick=%s action=%d x=%.3f y=%.3f",
                        which == CONTROLS_STICK_LEFT ? "left" : "right",
                        action, x, y);
    }
    elf_runtime_handle_analog((int)which, x, y);
}
