// The Brewdeck core (brewdeck_core.h) on the device. It runs the firmware's setup and loop: Wi-Fi and the machine
// (net.cpp, ws.cpp: polling, WebSocket, the command rules), settings storage, and the UI through bd_ui_*. The state,
// events, notice filtering and command answers are in core_state.cpp (shared with the emulator).
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <nvs.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include "brewdeck_core.h"
#if __has_include("brewdeck_version.h")    // written by ui_select.py at build time (git describe)
#include "brewdeck_version.h"
#endif
#ifndef BREWDECK_VERSION
#define BREWDECK_VERSION ""
#endif
#include "core_internal.h"
#include "core_state.h"
#include "board_core.h"
#include "tlog.h"
#include "net.h"
#include "ws.h"
#include "debug_http.h"

void bd_log(const char *fmt, ...) {
  char b[192];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  Log.println(b);
}

// ---- state (core_state.cpp) and what only the device knows ---------------------------------------------------------
static Preferences g_store;
static bool g_store_ok = false;

const bd_core_info *bd_core(void) {
  bd_core_info *i = cs_info();
  i->uptime_s = millis() / 1000;
  i->free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  i->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  return i;
}
bool bd_local_time(int *h, int *m, int *wday) {
  struct tm tm;
  if (!getLocalTime(&tm, 0) || tm.tm_year <= 120) return false;   // NTP not synced yet
  if (h) *h = tm.tm_hour;
  if (m) *m = tm.tm_min;
  if (wday) *wday = tm.tm_wday;
  return true;
}
static void cpy(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s ? s : ""); }
static void take_network(void) {
  bd_network *n = cs_network();
  char wip[16];
  cs_set_why(net_offline_reason(wip, sizeof wip));
  cpy(n->why_ip, sizeof n->why_ip, wip);
  net_addr_info(n->screen_ip, sizeof n->screen_ip, n->machine_where, sizeof n->machine_where);
  net_wifi_target(n->target, sizeof n->target);
  net_get_machine(n->machine_addr, sizeof n->machine_addr);
  net_wifi_info(n->ssid, sizeof n->ssid, &n->rssi, &n->channel, n->ap, sizeof n->ap);   // read by the network task
  cs_network_done();
}

// ---- commands (rules in core_state.cpp, then checked again just before sending by net.cpp / ws.cpp on the latest data)
bd_result bd_select_profile(int id, bool save) {
  const char *name;
  const bd_result r = cs_cmd_select(id, &name);
  if (r != BD_OK) return r;
  net_select_profile(id);
  if (save) ws_save_pick(name);     // boot profile once the machine reports it active
  return BD_OK;
}
bd_result bd_set_stop_weight(int profile_id, float grams, bool save) {
  int w10;
  const char *name;
  const bd_result r = cs_cmd_weight(profile_id, grams, &w10, &name);
  if (r == BD_OK) ws_set_weight(name, w10, save);   // sent 1 s after the last change
  return r;
}
bd_result bd_set_brew_temp(int profile_id, int deg, bool save) {
  const char *name;
  const bd_result r = cs_cmd_temp(profile_id, deg, &name);
  if (r == BD_OK) ws_set_temp(name, deg, save);
  return r;
}
bd_result bd_flush_mode(bd_flush mode) {
  int pm;
  const bd_result r = cs_cmd_flush(mode, &pm);
  if (r == BD_OK) ws_flush(pm);
  return r;
}
bd_result bd_tare(void) {
  const bd_result r = cs_cmd_tare();
  if (r == BD_OK) ws_tare();
  return r;
}
// stored shots: always queued; the network task sends them once the machine is online and idle (the previous shot is
// asked for right at the end of a shot, before the machine is back in HOME, and the link can drop then)
bd_result bd_load_previous_shot(const char *profile, int dur10) { net_request_prev(profile, dur10); return BD_OK; }
bd_result bd_load_stored_shot(uint32_t id) { net_request_hist(id); return BD_OK; }
void bd_wifi_scan(void) { net_wifi_scan(); }
void bd_wifi_join(const char *ssid, const char *pass) { net_wifi_join(ssid, pass); }
void bd_wifi_forget(void) { net_wifi_forget(); }
void bd_wifi_retry(void) { net_wifi_retry(); }
void bd_set_machine_address(const char *host) { net_set_machine(host); }

// ---- the UI's storage (NVS, namespace = BD_UI.name) ---------------------------------------------------------------
int32_t bd_store_get_int(const char *key, int32_t def) {
  if (!g_store_ok || !g_store.isKey(key)) return def;
  switch (g_store.getType(key)) {   // keys written by older firmware keep their own type (bool, u32)
    case PT_I32: return g_store.getInt(key, def);
    case PT_U32: return (int32_t)g_store.getUInt(key, (uint32_t)def);
    case PT_U8:  return g_store.getUChar(key, (uint8_t)def);
    case PT_I8:  return g_store.getChar(key, (int8_t)def);
    case PT_U16: return g_store.getUShort(key, (uint16_t)def);
    case PT_I16: return g_store.getShort(key, (int16_t)def);
    default:     return def;
  }
}
void bd_store_set_int(const char *key, int32_t v) {
  if (!g_store_ok) return;
  if (g_store.isKey(key) && g_store.getType(key) != PT_I32) g_store.remove(key);   // NVS keys are typed
  g_store.putInt(key, v);
}
size_t bd_store_get_blob(const char *key, void *buf, size_t cap) {
  if (!g_store_ok || !g_store.isKey(key)) return 0;
  const size_t n = g_store.getBytesLength(key);
  if (buf && cap >= n) g_store.getBytes(key, buf, n);   // Preferences copies nothing when the buffer is too small
  return n;
}
void bd_store_set_blob(const char *key, const void *buf, size_t n) { if (g_store_ok) g_store.putBytes(key, buf, n); }
void bd_store_erase(const char *key) { if (g_store_ok && g_store.isKey(key)) g_store.remove(key); }

// One-time move of the per-profile originals from their old namespace ("orig", u32 values) into the UI's store.
static void migrate_orig(void) {
  if (strcmp(BD_UI.name, "screen") != 0 || bd_store_get_int("orig_moved", 0)) return;   // only Brewdeck's UI kept them
  nvs_iterator_t it = NULL;
  int moved = 0;
  esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "orig", NVS_TYPE_U32, &it);
  Preferences old;
  const bool open = old.begin("orig", true);
  while (e == ESP_OK && it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    if (open && !g_store.isKey(info.key)) { bd_store_set_int(info.key, (int32_t)old.getUInt(info.key, 0)); moved++; }
    e = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  if (open) old.end();
  bd_store_set_int("orig_moved", 1);
  if (moved) Log.printf("core: %d kept profile originals moved into the UI's store\n", moved);
}

// ---- setup and loop -----------------------------------------------------------------------------------------------
// the loop's copies of stored shots and the history index, in PSRAM (~40 KB): allocated once in setup
static struct { bd_shot *prev, *hs, *hp; bd_profile_shape *hsh; WsShotIndex *ix; } g_buf;
template <typename T> static T *psram(const char *what) {
  T *p = (T *)heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM);
  if (!p) Log.printf("core: no PSRAM for %s (%u B): it stays off\n", what, (unsigned)sizeof(T));
  return p;
}
static void alloc_buffers(void) {
  g_buf.prev = psram<bd_shot>("the previous shot");
  g_buf.hs = psram<bd_shot>("a history shot");
  g_buf.hp = psram<bd_shot>("a history shot's previous");
  g_buf.hsh = psram<bd_profile_shape>("a history shot's profile");
  g_buf.ix = psram<WsShotIndex>("the shot history");
}

static bd_reset reset_of(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return BD_RESET_POWER_ON;
    case ESP_RST_EXT:       return BD_RESET_PIN;
    case ESP_RST_SW:        return BD_RESET_SOFTWARE;
    case ESP_RST_PANIC:     return BD_RESET_CRASH;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:  return BD_RESET_WATCHDOG;   // our code hung
    // ESP_RST_WDT (the RTC / system watchdog) is not a freeze of ours: on the ESP32-P4 every flash ends with it
    // (esptool's reset, then this watchdog ~3 s into the first boot; a read-only esptool reset gives ESP_RST_USB)
    case ESP_RST_WDT:       return BD_RESET_OTHER;
    case ESP_RST_BROWNOUT:  return BD_RESET_BROWNOUT;
    case ESP_RST_DEEPSLEEP: return BD_RESET_DEEP_SLEEP;
    case ESP_RST_USB:       return BD_RESET_USB;
    default:                return BD_RESET_OTHER;
  }
}

void bd_core_setup(void) {
  board_serial_begin();   // the board's log port (src/board/<board>)
  delay(200);
  const esp_reset_reason_t r = esp_reset_reason();
  cs_core_info(__DATE__ " " __TIME__, reset_of(r), BREWDECK_VERSION);
  Log.printf("boot: Brewdeck %s, built %s %s, started (reset reason %d)\n", BREWDECK_VERSION[0] ? BREWDECK_VERSION : "?",
             __DATE__, __TIME__, (int)r);
  net_begin();                      // before the panel starts (see net.h)
  Log.println(board_begin() ? "touch ok" : "touch NOT found");
  cs_blank_screen();
  Log.printf("core: API %d, UI \"%s\" %s written for API %d\n", BD_API_VERSION, BD_UI.name, BD_UI.version,
             BD_UI.api_version);
  if (BD_UI.api_version > BD_API_VERSION)
    Log.printf("core: WARNING: the UI wants a newer API (%d) than this core has (%d)\n", BD_UI.api_version, BD_API_VERSION);
  alloc_buffers();
  g_store_ok = g_store.begin(BD_UI.name, false);
  migrate_orig();
  bd_ui_init();
  Log.printf("internal RAM free %u, largest block %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#ifdef BREWDECK_DEBUG_HTTP
  debug_http_begin();                     // debug web server (debug_http.cpp: screen.bmp, heap, tasks, flash, roam, touch, bl, the UI's route): env ui-debug only
#endif
  ws_begin();
  Log.printf("ui \"%s\" %s ready\n", BD_UI.name, BD_UI.version);
}

void bd_debug_route(const char *path, bd_debug_fn fn) {
#ifdef BREWDECK_DEBUG_HTTP
  debug_http_route(path, fn);
#else
  (void)path; (void)fn;
#endif
}

void bd_core_loop(void) {
  static uint32_t last_net = 0, last_info = 0;
  // shot snapshots first, as soon as they are here (~7/s; polling at 150 ms made the chart jerky): the last points of a
  // shot must be in before the state that says it ended
  static uint32_t pf_n = 0, pf_max = 0, pf_sum = 0, pf_shots = 0, pf_t = 0;
  WsShot shot;
  while (net_get_shot(&shot)) { cs_shot_point(&shot); pf_shots++; }
  mem_watch("loop: shot points");
  if (millis() - last_net > 150) {
    last_net = millis();
    LiveData d;
    int link;
    if (net_get(&d, &link)) cs_take_live(&d, link);
    if (millis() - last_info > 1000) { last_info = millis(); take_network(); }
    {
      static bd_wifi_net nets[BD_MAX_NETS];
      int nn, js;
      bool scanning;
      if (net_get_wifi_scan(nets, &nn, &scanning)) cs_wifi_scan(nets, nn, scanning);
      if (net_get_wifi_join(&js)) cs_wifi_join(js);
    }
    static ProfileList pl;          // ~1.8 KB, off the loop stack
    if (net_get_profiles(&pl)) cs_take_profiles(&pl);
    static bd_profile_shape sh;
    if (net_get_shape(&sh)) cs_take_shape(&sh);
    mem_watch("loop: network data");
  }
  if (g_buf.prev) {
    bool changed;
    const int st = net_get_prev(g_buf.prev, &changed);
    if (changed) cs_prev_loaded(st, g_buf.prev);
  }
  {
    WsNotif nt;                     // machine notifications; cs_notice filters and classifies them
    if (net_get_notif(&nt)) cs_notice(nt.type, nt.msg);
  }
  if (g_buf.ix && net_get_index(g_buf.ix)) cs_take_index(g_buf.ix);
  if (g_buf.hs && g_buf.hp && g_buf.hsh) {
    bool changed;
    int pst;
    const int st = net_get_hist(g_buf.hs, g_buf.hsh, g_buf.hp, &pst, &changed);
    if (changed) cs_hist_loaded(st, g_buf.hs, g_buf.hsh, g_buf.hp, pst);
  }
  mem_watch("loop: stored shots, notices");
  cs_tick(millis());
#ifdef BREWDECK_DEBUG_HTTP
  debug_http_service();                   // a UI debug route asked for (bd_debug_route): answered here, on the UI task
#endif
  bd_display_stats(NULL, NULL, true);
  const uint32_t t0 = millis();
  bd_ui_tick(t0);
  const uint32_t dt = millis() - t0;
  mem_watch("loop: UI drawing");
  uint32_t fus, fpx;
  bd_display_stats(&fus, &fpx, false);
  // slow frames (e.g. the brewing screen opening): how much of it was copying into the frame buffer
  if (dt >= 40) Log.printf("ui: slow frame %u ms: flush %u ms for %u px, layout + drawing %u ms\n", (unsigned)dt,
                           (unsigned)(fus / 1000), (unsigned)fpx, (unsigned)(dt - fus / 1000));
  if (pf_shots) { pf_n++; pf_sum += dt; if (dt > pf_max) pf_max = dt; }
  if (millis() - pf_t > 5000) {     // render time while shot data arrives: one line per 5 s of a shot
    if (pf_shots) Log.printf("ui: 5 s: %u shot points, bd_ui_tick avg %u ms max %u ms\n", (unsigned)pf_shots,
                             (unsigned)(pf_n ? pf_sum / pf_n : 0), (unsigned)pf_max);
    pf_n = pf_max = pf_sum = pf_shots = 0;
    pf_t = millis();
  }
  delay(5);
}
