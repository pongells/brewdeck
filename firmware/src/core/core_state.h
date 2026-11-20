#pragma once
// The core's state and events, shared by the device core (core.cpp) and the emulator's fake core (sim/emu.cpp):
// takes data in the network code's types (live.h, pb.h) and keeps it as brewdeck_core.h hands it to the UI.
// Portable (no Arduino). Not part of the UI API.
#include "brewdeck_core.h"
#include "live.h"
#include "pb.h"

void cs_take_live(const LiveData *d, int link);         // machine state (bumps bd_seq_machine)
void cs_set_why(int why);                               // WHY_*: kept with the machine state, set with the network info
void cs_take_profiles(const ProfileList *l);
void cs_take_shape(const bd_profile_shape *s);
void cs_take_index(const WsShotIndex *ix);
bd_network *cs_network(void);                           // fill it, then cs_network_done()
void cs_network_done(void);
void cs_core_info(const char *build, bd_reset reset, const char *version = "");   // once at start

// events: each calls bd_ui_event at once (on the caller's task: the UI task)
void cs_shot_point(const WsShot *s);                    // also records the live shot (bd_live_shot)
void cs_notice(int type, const char *msg);
void cs_prev_loaded(int state, const bd_shot *t);     // PREV_* (net.h)
void cs_hist_loaded(int state, const bd_shot *s, const bd_profile_shape *sh, const bd_shot *prev, int prev_state);
void cs_wifi_scan(const bd_wifi_net *n, int count, bool scanning);
void cs_wifi_join(int state);                           // WIFI_JOIN_*

// commands: each command's whole rule set (checks, range, then the answer watched), shared by the device core and the
// emulator's fake core, so the emulator runs the screen's rules. BD_OK = send it; else refused, nothing to send.
// The ranges (BD_STOP_WEIGHT_MAX, BD_BREW_TEMP_MIN / MAX) are also the last check where the message is built (pb.cpp).
bd_result cs_cmd_select(int id, const char **name);                    // name: the profile to watch / save
bd_result cs_cmd_weight(int profile_id, float grams, int *w10, const char **name);
bd_result cs_cmd_temp(int profile_id, int deg, const char **name);
bd_result cs_cmd_flush(bd_flush mode, int *pb_mode);                   // PB_MODE_FLUSH / FLUSH_AUTO / HOME
bd_result cs_cmd_tare(void);
// the parts they are made of (also used by the tests)
bd_result cs_check_idle(void);                          // BD_OK, BD_ERR_OFFLINE, or BD_ERR_BUSY (not idle in HOME)
bd_result cs_check_active(int profile_id);              // BD_ERR_PROFILE_CHANGED if another profile is active now
bd_result cs_check_flush(bd_flush mode);                // on / timed: idle in HOME; off: from a flush mode
bool cs_select_pending(void);                           // a pick still waiting for the machine (BD_ERR_PENDING)
// command answers: a sent change is watched until the machine reports it (BD_EV_COMMAND OK) or its time is up
// (NOT_TAKEN: 5 s, 7 s for weight / temperature, which are sent 1 s after the last change); cs_tick checks, once per
// loop pass, with the UI's clock. A new request for the same command replaces the one watched.
void cs_watch_select(const char *profile);
void cs_watch_flush(const char *mode);                  // the mode the machine should report: FLUSH / FLUSH_AUTO / HOME
void cs_watch_weight(int w10);                          // the active profile's stop weight, x10 (0 = none)
void cs_watch_temp(int deg);                            // the active profile's brew temperature
void cs_watch_tare(void);                               // the scale reading back at ~0
void cs_answer(bd_command cmd, bd_answer a);            // an answer known at once
void cs_tick(uint32_t now_ms);                          // command answers, and a shot that stopped sending points

// the screen cleared to black before the UI starts
void cs_blank_screen(void);

const char *cs_profile_name(int id);                    // NULL = unknown
bd_core_info *cs_info(void);                            // the core fills uptime / memory before handing it out
