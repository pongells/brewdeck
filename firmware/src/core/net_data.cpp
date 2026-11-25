// What the network task (net.cpp) and the WebSocket task (ws.cpp) hand to the core loop: the machine's live data
// (REST, overlaid with fresher WebSocket values in net_get), the profile list and the active profile's phases, the shot
// snapshots, notices and the shot history index. Each is copied in and out under the one lock (net_int.h); a sequence
// number tells the core loop whether anything changed since it last looked.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "tlog.h"
#include "net.h"
#include "net_ws.h"
#include "net_int.h"
#include "tcp.h"   // ip_str

SemaphoreHandle_t g_net_mtx = NULL;

static LiveData g_data;
static int g_link = BD_LINK_CONNECTING;
static uint32_t g_seq = 0, g_seen = 0;
static ProfileList g_prof;
static uint32_t g_pseq = 0, g_pseen = 0;
static bd_profile_shape g_shape;
static uint32_t g_sseq = 0, g_sseen = 0;
static int g_why = BD_WHY_NONE;                       // offline reason (WHY_*)
static char g_why_ip[16] = "";                     // address that did not answer (BD_WHY_NO_REPLY)
// WebSocket data (ws.cpp): overlaid on the REST values in net_get() while fresh
static WsSensor g_wss;
static WsSystem g_wsy;
static uint32_t g_wss_ms = 0, g_wsy_ms = 0;
static bool g_wss_ok = false, g_wsy_ok = false;
static WsShot g_shotq[32];           // shot snapshots in arrival order (~7/s; the core loop drains it every pass)
static uint32_t g_shot_w = 0, g_shot_r = 0;
static volatile uint32_t g_last_shot_ms = 0;
static const uint32_t WS_FRESH_MS = 2000;
static WsShotIndex *g_index = NULL;   // newest 50 shots, from the WebSocket (d_shot_hist_index); PSRAM
static bool g_index_ok = false;
static uint32_t g_index_seq = 0, g_index_seen = 0;

static float g_act_w, g_act_t;          // active profile (d_act_prof): stop weight, brew temperature, name
static char g_act_name[48];
static bool g_act_session = false;   // d_act_prof received in the current WebSocket session
static char g_front_ver[28], g_static_ver[28];   // pb_versions: kept across reconnects
static uint32_t g_steam_set = 0;   // steam set point (pb_steam_set), 0 = not known yet
static WsScaleCfg g_scfg;   // the 3 scale flags only (see pb_scale_cfg; set by the WS task); kept across reconnects

void data_init(void) {
  g_net_mtx = xSemaphoreCreateMutex();
  g_index = (WsShotIndex *)heap_caps_calloc(1, sizeof(WsShotIndex), MALLOC_CAP_SPIRAM);
  memset(&g_data, 0, sizeof g_data);
  memset(&g_prof, 0, sizeof g_prof);
  memset(&g_shape, 0, sizeof g_shape);
}

// ---- the network task's side ---------------------------------------------------------------------------------------
void data_publish(const LiveData *d, int link) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  if (d) g_data = *d;
  g_link = link;
  g_seq++;
  xSemaphoreGive(g_net_mtx);
}

void data_set_why(int why, uint32_t ip) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_why = why;
  ip_str(ip, g_why_ip, sizeof g_why_ip);
  xSemaphoreGive(g_net_mtx);
}

void data_publish_profiles(const ProfileList *l) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_prof = *l;
  g_pseq++;
  xSemaphoreGive(g_net_mtx);
}

void data_publish_shape(const bd_profile_shape *s) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  if (memcmp(&g_shape, s, sizeof *s) != 0) {        // only wake the UI when the graph really changes
    g_shape = *s;
    g_sseq++;
  }
  xSemaphoreGive(g_net_mtx);
}

bool data_index(WsShotIndex *out) {
  NetLock l;
  if (!g_index_ok || !g_index || !out) return false;
  *out = *g_index;
  return true;
}
uint32_t data_newest_shot(void) {
  NetLock l;
  return g_index_ok && g_index && g_index->n > 0 ? g_index->r[0].id : 0;
}
uint32_t data_last_shot_ms(void) { return g_last_shot_ms; }

// ---- the core loop's side (net.h) ----------------------------------------------------------------------------------
int net_offline_reason(char *ip, size_t cap) {
  if (!g_net_mtx) { if (cap) ip[0] = 0; return BD_WHY_NONE; }
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const int w = g_why;
  snprintf(ip, cap, "%s", g_why_ip);
  xSemaphoreGive(g_net_mtx);
  return w;
}

bool net_get(LiveData *out, int *link) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  *out = g_data;
  *link = g_link;
  const uint32_t now = millis();
  // REST requests can time out while the machine is busy (seen at the start of a shot): fresh WebSocket data
  // alone keeps the machine "online" (with the last REST values for the rest)
  if (g_link != BD_LINK_ONLINE && g_wss_ok && now - g_wss_ms < WS_FRESH_MS && g_data.mode[0]) *link = BD_LINK_ONLINE;
  if (*link == BD_LINK_ONLINE && g_wss_ok && now - g_wss_ms < WS_FRESH_MS) {   // ~3/s instead of ~1/s
    out->temperature = g_wss.temperature;
    out->pressure = g_wss.pressure;
    out->weight = g_wss.weight;
    out->water_level = g_wss.water_level;
    out->brew_active = g_wss.brew_active;
    out->steam_active = g_wss.steam_active;
    out->hot_water_active = g_wss.hot_water;
  }
  if (*link == BD_LINK_ONLINE && g_wsy_ok && now - g_wsy_ms < WS_FRESH_MS) {
    out->scales_present = g_wsy.scales_present;
    out->warmup_remaining = (int)g_wsy.warmup_remaining;
    out->readiness = (int)g_wsy.readiness;
    const char *m = ws_mode_name(g_wsy.mode);
    if (m[0]) snprintf(out->mode, sizeof out->mode, "%s", m);
  }
  out->act_weight = g_act_w;
  out->act_temp = g_act_t;
  snprintf(out->act_profile, sizeof out->act_profile, "%s", g_act_name);
  out->commands_ready = g_act_session;
  out->steam_set = (int)g_steam_set;
  snprintf(out->front_ver, sizeof out->front_ver, "%s", g_front_ver);
  snprintf(out->static_ver, sizeof out->static_ver, "%s", g_static_ver);
  if (!g_scfg.valid) out->scale_type = BD_SCALE_UNKNOWN;
  else if (g_scfg.force_predictive || (!g_scfg.bt_enabled && !g_scfg.hw_enabled)) out->scale_type = BD_SCALE_PREDICTIVE;
  else if (g_scfg.bt_enabled && !g_scfg.hw_enabled) out->scale_type = BD_SCALE_BT;
  else if (g_scfg.hw_enabled && !g_scfg.bt_enabled) out->scale_type = BD_SCALE_WIRED;
  else out->scale_type = BD_SCALE_UNKNOWN;             // both enabled: which one is connected is not reported
  {   // the machine's readiness: log every change with the temperatures (to see when it switches)
    static int last_rd = -2;
    if (*link == BD_LINK_ONLINE && out->readiness != last_rd) {
      static const char *const RN[] = {"WAITING", "WARMING", "HEATING", "READY", "STANDBY", "STEAMING", "FLUSHING",
                                       "MAINTENANCE"};
      Log.printf("net: readiness %d %s (boiler %.1f, target %.1f, pressure %.2f bar, warmup %d s left, mode %s)\n",
                    out->readiness, out->readiness >= 0 && out->readiness <= 7 ? RN[out->readiness] : "?",
                    out->temperature, out->target, out->pressure, out->warmup_remaining, out->mode);
      last_rd = out->readiness;
    }
  }
  {   // pressure while idle in HOME, logged as a range per minute (to set the pressure warning's limits from real values)
    static float pmin = 1e9f, pmax = -1e9f;
    static uint32_t pt = 0;
    const bool idle = *link == BD_LINK_ONLINE && !strcmp(out->mode, "HOME") && !out->brew_active &&
                      !out->steam_active && !out->hot_water_active;
    if (idle) {
      if (out->pressure < pmin) pmin = out->pressure;
      if (out->pressure > pmax) pmax = out->pressure;
    }
    if (millis() - pt > 60000) {
      pt = millis();
      if (pmax >= pmin) Log.printf("net: idle pressure last min: %.2f - %.2f bar (now %.2f)\n", pmin, pmax, out->pressure);
      pmin = 1e9f;
      pmax = -1e9f;
    }
  }
  static int shown = -1;                            // log every change of what the screen shows
  bool changed = g_seq != g_seen || *link != shown;
  if (*link != shown) {
    static const char *const LN[] = {"connecting", "no machine", "online"};
    Log.printf("net: link %s (rest %s, ws data %ld ms old)\n",
                  *link >= 0 && *link < 3 ? LN[*link] : "?", g_link == BD_LINK_ONLINE ? "ok" : "failing",
                  g_wss_ok ? (long)(now - g_wss_ms) : -1L);
    shown = *link;
  }
  g_seen = g_seq;
  xSemaphoreGive(g_net_mtx);
  return changed;
}

bool net_get_profiles(ProfileList *out) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  bool changed = g_pseq != g_pseen;
  if (changed) *out = g_prof;
  g_pseen = g_pseq;
  xSemaphoreGive(g_net_mtx);
  return changed;
}

bool net_get_shape(bd_profile_shape *out) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  bool changed = g_sseq != g_sseen;
  if (changed) *out = g_shape;
  g_sseen = g_sseq;
  xSemaphoreGive(g_net_mtx);
  return changed;
}

bool net_get_shot(WsShot *out) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool any = g_shot_r != g_shot_w;
  if (any) *out = g_shotq[g_shot_r++ % 32];
  xSemaphoreGive(g_net_mtx);
  return any;
}

bool net_get_index(WsShotIndex *out) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool ch = g_index_ok && g_index_seq != g_index_seen;
  if (ch) { *out = *g_index; g_index_seen = g_index_seq; }
  xSemaphoreGive(g_net_mtx);
  return ch;
}

// ---- the WebSocket task's side (net_ws.h) --------------------------------------------------------------------------
void net_ws_sensor(const WsSensor *s) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_wss = *s;
  g_wss_ms = millis();
  g_wss_ok = true;
  g_seq++;
  xSemaphoreGive(g_net_mtx);
}
void net_ws_system(const WsSystem *s) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_wsy = *s;
  g_wsy_ms = millis();
  g_wsy_ok = true;
  g_seq++;
  xSemaphoreGive(g_net_mtx);
}
void net_ws_shot(const WsShot *s) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_shotq[g_shot_w++ % 32] = *s;
  g_last_shot_ms = millis();
  if (g_shot_w - g_shot_r > 32) g_shot_r = g_shot_w - 32;   // UI stalled: drop the oldest
  xSemaphoreGive(g_net_mtx);
}
void net_ws_act_profile(const char *name, float weight, float temp) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  if (g_act_w != weight || g_act_t != temp || strcmp(g_act_name, name) != 0) g_seq++;
  g_act_w = weight;
  g_act_t = temp;
  snprintf(g_act_name, sizeof g_act_name, "%s", name);
  if (!g_act_session) g_seq++;
  g_act_session = true;
  xSemaphoreGive(g_net_mtx);
}
void net_ws_versions(const char *front, const char *stat) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  if (strcmp(g_front_ver, front) || strcmp(g_static_ver, stat)) g_seq++;
  snprintf(g_front_ver, sizeof g_front_ver, "%s", front);
  snprintf(g_static_ver, sizeof g_static_ver, "%s", stat);
  xSemaphoreGive(g_net_mtx);
}
void net_ws_steam_set(uint32_t deg) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  if (g_steam_set != deg) g_seq++;
  g_steam_set = deg;
  xSemaphoreGive(g_net_mtx);
}
void net_ws_scale_cfg(const WsScaleCfg *c) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_scfg = *c;
  g_seq++;
  xSemaphoreGive(g_net_mtx);
}
static WsNotif g_notif;
static uint32_t g_notif_seq = 0, g_notif_seen = 0;
void net_ws_notif(const WsNotif *n) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_notif = *n;
  g_notif_seq++;
  xSemaphoreGive(g_net_mtx);
}
bool net_get_notif(WsNotif *out) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool ch = g_notif_seq != g_notif_seen;
  if (ch) { *out = g_notif; g_notif_seen = g_notif_seq; }
  xSemaphoreGive(g_net_mtx);
  return ch;
}
bool net_rest_online(void) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool on = g_link == BD_LINK_ONLINE;
  xSemaphoreGive(g_net_mtx);
  return on;
}
void net_ws_lost(void) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_wss_ok = g_wsy_ok = false;
  g_act_session = false;
  g_seq++;
  xSemaphoreGive(g_net_mtx);
}

void net_ws_index(const WsShotIndex *ix) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  if (g_index) { *g_index = *ix; g_index_ok = true; g_index_seq++; }
  xSemaphoreGive(g_net_mtx);
}

