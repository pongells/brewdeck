#pragma once
// Live machine data (REST /api/system/status, overlaid with fresher WebSocket values in net_get()).
#include <stdbool.h>
#include <stdint.h>
#include "brewdeck_core.h"
#include <stddef.h>

// The API's enum values under the names the network code and Brewdeck's screens use.
enum { LINK_CONNECTING = BD_LINK_CONNECTING, LINK_NO_MACHINE = BD_LINK_NO_MACHINE, LINK_ONLINE = BD_LINK_ONLINE };
// why the link is down (offline screen text): from the Wi-Fi disconnect reason, the name lookup and the REST replies
enum { WHY_NONE = BD_WHY_NONE, WHY_WIFI_NO_AP = BD_WHY_WIFI_NO_AP, WHY_WIFI_AUTH = BD_WHY_WIFI_AUTH,
       WHY_WIFI_OTHER = BD_WHY_WIFI_OTHER, WHY_NAME_UNKNOWN = BD_WHY_NAME_UNKNOWN, WHY_NO_REPLY = BD_WHY_NO_REPLY };
// scale type from the machine's scale settings (pb_scale_cfg): unknown until d_settings arrived, or both kinds on
enum { SCALE_UNKNOWN = BD_SCALE_UNKNOWN, SCALE_BT = BD_SCALE_BT, SCALE_HW = BD_SCALE_WIRED,
       SCALE_PREDICTIVE = BD_SCALE_PREDICTIVE };

typedef struct {
  float temperature;      // boiler, deg C
  float target;           // the boiler's current target (REST targetTemperature)
  float pressure;         // bar
  float weight;           // scale, g
  int   water_level;      // percent
  int   warmup_remaining; // seconds
  // machine's readiness (REST readinessState / WS dashboardReadinessState; names from the web UI bundle):
  // 0 WAITING, 1 WARMING ("WARMING UP" + countdown), 2 HEATING ("Waiting for temperature"), 3 READY, 4 STANDBY,
  // 5 STEAMING, 6 FLUSHING, 7 MAINTENANCE; -1 = not reported (then the screen's temperature rule is used)
  int   readiness;
  bool  brew_active;
  bool  steam_active;
  bool  hot_water_active;
  bool  scales_present;
  int   scale_type;       // SCALE_*
  int   steam_set;        // steam set point in deg C (d_settings boiler.steamSetPoint), 0 = not known
  float act_weight;       // target weight of the machine's ACTIVE profile (d_act_prof), 0 = not known yet
  float act_temp;         // its brew temperature (waterTemperature), 0 = not known / heater off
  char  act_profile[48];  // its name (raw)
  char  profile[48];
  char  machine[24];
  char  mode[20];         // HOME, BREW_AUTO, STEAM, FLUSH, DESCALE ...
  char  core_ver[16];     // machine's core firmware (REST coreVersion, e.g. "c929939"), "" = not known
  char  core_type[28];    // its board (REST coreType, e.g. "STM32U585-PCB-PCA")
  char  front_ver[28];    // its built-in screen ("Embedded UI", d_settings versions.frontVersion), "" = not known
  char  static_ver[28];   // its web UI (d_settings versions.staticVersion)
} LiveData;

// Wi-Fi networks found by a scan (menu > Wi-Fi), strongest first; join states (passed on as BD_EV_WIFI_JOIN)
#define MAX_NETS BD_MAX_NETS
typedef bd_wifi_net WifiNet;
enum { WIFI_JOIN_IDLE = BD_JOIN_IDLE, WIFI_JOIN_BUSY = BD_JOIN_BUSY, WIFI_JOIN_OK = BD_JOIN_OK,
       WIFI_JOIN_FAILED = BD_JOIN_FAILED };

// Saved profiles (GET /api/profiles/all, details from GET /api/profile/<id>).
#define MAX_PROFILES BD_MAX_PROFILES
typedef struct {
  int   id;
  char  name[48];         // as the machine reports it, e.g. "Leva 9 SINGLE v0.9 (community)"
  bool  selected;         // the machine's active profile
  bool  detail;           // phases/weight/temp below are filled in
  int   phases;
  float weight;           // global stop weight, g (0 = none)
  float temp;             // water temperature, deg C
} ProfileInfo;

typedef struct {
  int count;
  ProfileInfo p[MAX_PROFILES];
} ProfileList;

// The API's types (brewdeck_core.h), under the names the network code and Brewdeck's screens use: one definition.
// A profile's phases (preview graph, stop weight; skipped phases left out), one point of a shot, a whole shot (live
// points from the WebSocket d_shot_snap and stored shots from GET /api/shots/<id> alike; ~11.5 KB: PSRAM on the device).
enum { PHASE_PRESSURE = BD_PHASE_PRESSURE, PHASE_FLOW = BD_PHASE_FLOW };
enum { CURVE_INSTANT = BD_CURVE_INSTANT, CURVE_LINEAR, CURVE_EASE_IN, CURVE_EASE_OUT, CURVE_EASE_IN_OUT };
#define MAX_PHASES BD_MAX_PHASES
#define SHOT_MAX BD_SHOT_MAX    // points kept per shot (~100 s at ~7/s); longer shots are thinned
typedef bd_phase PhaseShape;
typedef bd_profile_shape ProfileShape;   // id 0 = nothing loaded yet
typedef bd_shot_point ShotPoint;
typedef bd_shot ShotTrace;

// ---- shot data from the WebSocket (decoded by pb.cpp) ----
typedef struct {                // ShotSnapshotDto  (action d_shot_snap, ~7/s while a shot runs). All x10 integers.
  uint32_t time10;              // timeInShot, 0.1 s
  uint32_t pressure10, pump_flow10, weight_flow10, temperature10, weight10, water10;
  uint32_t target_temp10, target_flow10, target_pressure10;
  uint32_t phase;               // 0-based phase index
} WsShot;

// ShotHistoryIndexDto (action d_shot_hist_index, on connect and after each shot): the newest 50 shots, newest first.
// name_hash: compare with shot_name_hash of a profile name; name: printable ASCII only, cut at 39 chars (shot history
// list); ts: Unix time of the shot (small = machine clock not set, a Gaggiuino quirk). ~2.8 KB: keep copies in PSRAM.
#define WS_SHOT_INDEX_MAX BD_HISTORY_MAX
typedef struct { uint32_t id, name_hash, dur10, ts; char name[40]; } WsShotRec;
typedef struct { int n; WsShotRec r[WS_SHOT_INDEX_MAX]; } WsShotIndex;
uint32_t shot_name_hash(const char *s, size_t n);   // FNV-1a over the printable ASCII bytes (stops at NUL)
