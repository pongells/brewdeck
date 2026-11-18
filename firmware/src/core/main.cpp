// Firmware entry points: everything runs in the Brewdeck core (src/core/core.cpp), which starts the board, the
// network and the UI (Brewdeck's own in src/ui: ui_bridge.cpp, ui.cpp and screens/; another UI
// implements the same bd_ui_* functions) through brewdeck_core.h.
#include <Arduino.h>
#include "core_internal.h"

// LVGL 9 draws nested objects recursively (lv_obj_redraw), ~10 levels deep on our screens plus the label renderer:
// the default 8 KB loop-task stack overflowed (stack canary) a few seconds after boot. Measured peak ~8.2 KB (debug
// GET /tasks, every screen redrawn): 12 KB (internal RAM).
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

void setup() { bd_core_setup(); }
void loop() { bd_core_loop(); }
