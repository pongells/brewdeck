#pragma once
// The core's own data types (network code, parsers, decoders); UIs use the bd_* types of brewdeck_core.h.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "brewdeck_core.h"

// Live machine data (REST /api/system/status, overlaid with fresher WebSocket values in net_get()).
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
  int   scale_type;       // bd_scale_type
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

// Saved profiles (GET /api/profiles/all, details from GET /api/profile/<id>).
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
  ProfileInfo p[BD_MAX_PROFILES];
} ProfileList;

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
typedef struct { uint32_t id, name_hash, dur10, ts; char name[40]; } WsShotRec;
typedef struct { int n; WsShotRec r[BD_HISTORY_MAX]; } WsShotIndex;
uint32_t shot_name_hash(const char *s, size_t n);   // FNV-1a over the printable ASCII bytes (stops at NUL)
