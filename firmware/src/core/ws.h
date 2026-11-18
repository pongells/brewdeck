#pragma once
// WebSocket client for ws://<machine>/ws (protocol: docs/ws/README.md). Receives live data; the ONLY messages it
// sends are tare (c_tare_pend), the stop weight and the brew temperature (c_upd_act_prof, one field each), the save
// (c_save_act_prof) and the flush modes (c_opmode), besides the protocol's pong and close.
// Holds 1 of the machine's 3 slots.
void ws_begin(void);   // starts the task (after net_begin); no-op without Wi-Fi config
// flush: mode PB_MODE_FLUSH / PB_MODE_FLUSH_AUTO (only idle in HOME) or PB_MODE_HOME (only from
// a flush mode); anything else is refused (pb_opmode_cmd builds only these three)
void ws_flush(int mode);
void ws_tare(void);    // ask for a tare; sent within ~10 ms if connected, a scale is present and the machine is idle
                       // in HOME (checked in the WS task on its own latest data); otherwise dropped (logged)
void ws_set_weight(const char *profile, int weight10, bool save = true);   // new target weight for the named (active)
                                                          // profile; sent if idle in HOME and that profile is still active
void ws_set_temp(const char *profile, int deg, bool save = true);           // new brew temperature (1-105, never 0), same rules
// save = false: only for now (not stored on the machine; a later save of this profile then refuses, as for a
// change made elsewhere, so the unsaved value is never stored by accident)
// Weight / temperature changes sent from here are saved on the machine 3 s after the last one (c_save_act_prof).
// ws_save_pick: a profile picked on the screen is saved (boot profile) once the machine reports
// it active (within 10 s). Only while idle in HOME.
void ws_save_pick(const char *profile);
