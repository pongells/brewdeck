#pragma once
// The core's entry points for the firmware's main.cpp (not part of the UI API).
void bd_core_setup(void);   // Arduino setup(): network, board, storage, then the UI (bd_ui_init)
void bd_core_loop(void);    // Arduino loop(): machine data, events and bd_ui_tick, once per pass
