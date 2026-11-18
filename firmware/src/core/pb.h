#pragma once
// Minimal protobuf decoding of the Gaggiuino WebSocket frames the screen reads (field numbers from Gaggiuino's
// WebSocket docs, plus the few undocumented ones listed in docs/ws/README.md). Pure C++, no allocation, no Arduino:
// host-testable. Unknown fields are skipped.
// proto3 leaves out zero/false values, so every struct starts zeroed and absent fields stay 0.
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "live.h"   // WsShot, WsShotIndex, shot_name_hash: plain data the UI uses too

typedef struct {                // SensorStateSnapshotDto  (action d_sensor_snap, ~3/s)
  bool  brew_active, steam_active, hot_water;
  float temperature, pressure, pump_flow, weight_flow, weight;
  int   water_level;            // percent
} WsSensor;

typedef struct {                // SystemStateDto  (action d_sys_state, ~3/s)
  bool     scales_present, tare_pending;
  int      mode;                // OperationModeDto, see ws_mode_name
  uint32_t warmup_remaining;    // seconds
  uint32_t readiness;           // dashboardReadinessState (see LiveData.readiness)
  uint32_t persisted_profile;   // boot profile id
} WsSystem;

// WebSocketMessageDto { string action = 1; bytes data = 2; }. action is copied (truncated) into act.
bool pb_envelope(const uint8_t *b, size_t n, char *act, size_t act_cap, const uint8_t **data, size_t *dlen);
bool pb_sensor(const uint8_t *b, size_t n, WsSensor *o);
bool pb_system(const uint8_t *b, size_t n, WsSystem *o);
bool pb_shot(const uint8_t *b, size_t n, WsShot *o);
// d_shot_hist_index -> WsShotIndex (live.h)
bool pb_shot_index(const uint8_t *b, size_t n, WsShotIndex *o);
// NotificationDto (action d_notif): type (0 INFO, 1 SUCCESS, 2 WARN, 3 ERROR) and text, e.g. WARN "Fill the water tank!"
// (repeated ~1.4/s, a Gaggiuino quirk), "Releasing pressure!" around the end of a shot. msg: printable ASCII only, cut to fit.
enum { NOTIF_INFO = 0, NOTIF_SUCCESS, NOTIF_WARN, NOTIF_ERROR };
typedef struct { int type; char msg[64]; } WsNotif;
bool pb_notif(const uint8_t *b, size_t n, WsNotif *o);
const char *ws_mode_name(int mode);   // "HOME", "BREW_AUTO", ... (the strings the REST status uses), "" if unknown

// Tare: WebSocketMessageDto { action: "c_tare_pend",
// data: UpdateSystemStateCommandDto { tarePending: true } }, byte for byte what the machine's web UI sends.
extern const uint8_t PB_TARE_CMD[];
extern const size_t PB_TARE_CMD_LEN;   // 17
// Save: WebSocketMessageDto { action: "c_save_act_prof" } and nothing else, as the web UI's
// persistActiveProfile(). Saves the ACTIVE profile's values into the stored profile (CONFIRMED 2026-11-16: active
// 91 degC, stored 92 -> 91) and, by the web UI's wording ("Profile set as default"), makes it the boot profile.
extern const uint8_t PB_SAVE_PROF_CMD[];
extern const size_t PB_SAVE_PROF_CMD_LEN;   // 17
// Flush: WebSocketMessageDto { action: "c_opmode", data: UpdateSystemStateCommandDto
// { operationMode } }, as the web UI's Flush / Auto buttons. ONLY these modes are ever built; anything else -> 0.
enum { PB_MODE_FLUSH = 2, PB_MODE_FLUSH_AUTO = 5, PB_MODE_HOME = 7 };
size_t pb_opmode_cmd(int mode, uint8_t *out, size_t cap);   // bytes written (14), 0 = refused

// d_settings (GaggiaSettingsDto, once on connect) carries MQTT credentials and API tokens. The ONLY thing taken from
// it is the scale type: GaggiaSettingsDto.scales (field 4) ->
// ScalesSettingsDto { forcePredictive = 1, hwScalesEnabled = 2, btScalesEnabled = 5 }. Every other field is skipped
// without being copied; the caller must not log or keep the frame.
typedef struct { bool valid, force_predictive, hw_enabled, bt_enabled; } WsScaleCfg;
bool pb_scale_cfg(const uint8_t *b, size_t n, WsScaleCfg *o);
// d_settings: ONLY boiler.steamSetPoint; 0 when absent. Nothing else read.
bool pb_steam_set(const uint8_t *b, size_t n, uint32_t *deg);
// d_settings: ONLY versions.frontVersion (the machine's built-in screen) and versions.staticVersion (its web UI),
// for Menu > About: GaggiaSettingsDto.versions (field 7) -> VersionsContainerDto { 2, 3 }.
// Printable ASCII only, cut to the buffer; "" when absent. Nothing else read. False when versions is missing / broken.
bool pb_versions(const uint8_t *b, size_t n, char *front, size_t fn, char *stat, size_t sn);

// Target weight: the web UI's dashboard sends the whole active profile back with
// only globalStopConditions.weight changed (c_upd_act_prof + ProfileDto). The screen does the same, byte for byte on
// the machine's own d_act_prof (also a ProfileDto): every field is copied unchanged except ProfileDto.3
// (GlobalStopConditionsDto), in which field 2 (float weight) is replaced (or added). Pure, host-tested.
// pb_profile_info: id (field 6), name (field 1, truncated) and the current target weight (0 if none).
bool pb_profile_info(const uint8_t *b, size_t n, uint32_t *id, char *name, size_t ncap, float *weight);
// Builds the complete WebSocketMessageDto { action "c_upd_act_prof", data <patched profile> } into out.
// Returns its length, 0 if the profile is malformed or out is too small.
size_t pb_weight_cmd(const uint8_t *prof, size_t n, float weight, uint8_t *out, size_t cap);
// pb_temp_cmd: the same c_upd_act_prof envelope with only ProfileDto.waterTemperature (field 4) set to temp
// (1-105 degC; 0 = heater off is never built; per-phase temperatures are left alone, as the web UI's dashboard
// does). 0 on error / out of range.
size_t pb_temp_cmd(const uint8_t *prof, size_t n, float temp, uint8_t *out, size_t cap);
// pb_profile_temp: ProfileDto.waterTemperature (0 if absent = brew heater off)
bool pb_profile_temp(const uint8_t *b, size_t n, float *temp);

// WebSocketResponseDto (d_resp, the machine's answer to a command): action, result (0 SUCCESS, 1 ERROR), errorMessage
bool pb_resp(const uint8_t *b, size_t n, char *act, size_t acap, int *result, char *err, size_t ecap);
