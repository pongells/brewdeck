// The screen's own Wi-Fi: the network in use (saved on the screen, else the one in wifi_config.h), joining one from the
// menu, scans, "Forget", roaming between the access points of one network, rejoining after a drop, and the link status
// the UI shows. All of it runs on the network task (net.cpp calls wifi_step / wifi_info_step every pass); on the ESP32-P4
// every Wi-Fi query is a request to its ESP32-C6 and can block, so the UI task never asks Wi-Fi itself.
#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_sntp.h>
#include <esp_heap_caps.h>
#include <time.h>
#include "kv.h"
#include <string.h>
#include "stamp.h"
#include "tlog.h"
#include "wifi_config.h"
#include "net.h"
#include "net_int.h"
#include "board_core.h"
#include "tcp.h"   // ip_str

// Wi-Fi credentials in use: the network saved from the menu (NVS "wifi") if any, else the compiled-in one (none while
// wifi_config.h still has the example's placeholder: the screen then asks for a network, "No Wi-Fi set up").
// "Forget this network" clears the saved one and sets "nobuiltin" so the compiled-in network is not used either (else
// it comes back after a restart); joining a network from the list clears the flag. g_ssid "" = none.
static char g_ssid[33], g_pass[65];
static bool g_saved_net = false, g_no_builtin = false;
static uint32_t g_start_ms = 0;            // when the net task started (wifi_start): the built-in fallback counts from it
static volatile bool g_forget_req = false;
static bool compiled_net(void) { return strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0; }   // a network in wifi_config.h
// local time for the idle clock (POSIX TZ string; wifi_config.h can set GAGGI_TZ): Central Europe with summer time
#ifndef GAGGI_TZ
#define GAGGI_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
static void load_creds(void) {
  g_saved_net = false;
  g_no_builtin = kv_get_bool("wifi", "nobuiltin", false);
  if (kv_get_str("wifi", "ssid", g_ssid, sizeof g_ssid) && g_ssid[0] && kv_get_str("wifi", "pass", g_pass, sizeof g_pass)) {
    g_saved_net = true;
    return;
  }
  if (g_no_builtin || !compiled_net()) { g_ssid[0] = 0; g_pass[0] = 0; return; }
  snprintf(g_ssid, sizeof g_ssid, "%s", WIFI_SSID);
  snprintf(g_pass, sizeof g_pass, "%s", WIFI_PASS);
}

// ---- the station, on ESP-IDF's Wi-Fi driver ------------------------------------------------------------------------
// The same settings the Arduino core used: join the network's strongest AP after a scan of every channel, WPA2 or
// better when there is a password, no power save; RAM-only driver settings (ours are in NVS "wifi").
static esp_netif_t *g_sta;
static volatile bool g_up = false;                 // joined and has an IP address
static volatile uint32_t g_ip = 0;                 // that address (network order)
static volatile bool g_first_try = false;          // a join just started: one quick retry whatever the reason
static volatile uint8_t g_disc_reason = 0;
static volatile uint32_t g_disc_ms = 0;   // when the last disconnect event came (stamp_nz), for the quick first retries

// after a drop the driver joins again by itself for passing reasons (as the Arduino core did); never after we left
// (reason 8, ASSOC_LEAVE: rejoin / join from the menu / forget call sta_join themselves)
static bool reconnectable(uint8_t r) {
  switch (r) {
    case WIFI_REASON_UNSPECIFIED: case WIFI_REASON_AUTH_EXPIRE: case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: case WIFI_REASON_802_1X_AUTH_FAILED: case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_LEAVE: case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA: case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA:
    case WIFI_REASON_ASSOC_NOT_AUTHED: case WIFI_REASON_MIC_FAILURE: case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_INVALID_PMKID: case WIFI_REASON_BEACON_TIMEOUT: case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_ASSOC_FAIL: case WIFI_REASON_CONNECTION_FAIL: case WIFI_REASON_AP_TSF_RESET:
    case WIFI_REASON_ROAMING: return true;
    default: return false;
  }
}

static volatile bool g_scan_done_ev = false;       // WIFI_EVENT_SCAN_DONE came
static volatile bool g_disc_new = false, g_reconnect = false;   // for drop_step on the network task
// ESP-IDF's event task: small stack (2 KB), and on the ESP32-P4 a Wi-Fi call is a request to the ESP32-C6, so only
// flags here; the network task logs and joins again (drop_step), woken at once
static void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const uint8_t r = ((wifi_event_sta_disconnected_t *)data)->reason;
    g_up = false;
    g_disc_reason = r;
    g_disc_ms = stamp_nz(millis());
    g_disc_new = true;
    if (r != WIFI_REASON_ASSOC_LEAVE && (g_first_try || reconnectable(r))) { g_first_try = false; g_reconnect = true; }
    net_wake();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    g_scan_done_ev = true;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    g_ip = ((ip_event_got_ip_t *)data)->ip_info.ip.addr;
    g_up = true;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
    g_up = false;
  }
}

// join ssid (any of its APs, or the one with bssid on channel ch)
static void sta_join(const char *ssid, const char *pass, int ch = 0, const uint8_t *bssid = NULL) {
  wifi_config_t c;
  memset(&c, 0, sizeof c);
  snprintf((char *)c.sta.ssid, sizeof c.sta.ssid, "%s", ssid);
  // several access points can share a network name: by default the ESP32 joins the FIRST one it finds (seen: -64 to
  // -70 dBm right next to an access point, with stalls); scan every channel and take the strongest instead
  c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  c.sta.threshold.rssi = -127;
  c.sta.pmf_cfg.capable = true;
  if (pass && pass[0]) {
    memcpy(c.sta.password, pass, strnlen(pass, sizeof c.sta.password));
    c.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  }
  if (bssid) { c.sta.bssid_set = true; memcpy(c.sta.bssid, bssid, 6); c.sta.channel = (uint8_t)ch; }
  g_first_try = true;
  esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &c);
  if (e == ESP_OK) e = esp_wifi_connect();
  memset(&c, 0, sizeof c);
  if (e != ESP_OK) Log.printf("wifi: join not started (%s)\n", esp_err_to_name(e));
}
static void sta_leave(void) { g_first_try = false; g_reconnect = false; esp_wifi_disconnect(); }
// network task: a drop's log line and the automatic retry
static void drop_step(void) {
  if (g_disc_new) { g_disc_new = false; Log.printf("wifi: disconnected, reason %u\n", (unsigned)g_disc_reason); }
  if (g_reconnect) { g_reconnect = false; esp_wifi_connect(); }
}
static bool sta_up(void) { return g_up; }

// the AP in use: false when not joined (on the ESP32-P4 each call is a request to the ESP32-C6)
static bool ap_now(wifi_ap_record_t *a) { return g_up && esp_wifi_sta_get_ap_info(a) == ESP_OK; }
static int rssi_now(void) { wifi_ap_record_t a; return ap_now(&a) ? a.rssi : 0; }
static void mac_str(const uint8_t *m, char *out, size_t cap) {
  snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// scans (the Wi-Fi page and roaming share one): start, then poll until done; the records stay until the next start
#define SCAN_MAX 40
static wifi_ap_record_t *g_recs;                    // SCAN_MAX records in PSRAM (~3.5 KB)
static int g_nrec = 0;
enum { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE };
static int g_scan = SCAN_IDLE;
static bool scan_begin(uint32_t ms_per_channel) {
  if (g_scan == SCAN_RUNNING) return false;
  wifi_scan_config_t c;
  memset(&c, 0, sizeof c);
  c.show_hidden = false;
  c.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  c.scan_time.active.min = 100;
  c.scan_time.active.max = ms_per_channel;
  g_scan_done_ev = false;
  if (esp_wifi_scan_start(&c, false) != ESP_OK) return false;
  g_scan = SCAN_RUNNING;
  return true;
}
static int scan_state(void) {
  if (g_scan == SCAN_RUNNING && g_scan_done_ev) {
    uint16_t n = SCAN_MAX;
    if (!g_recs) g_recs = (wifi_ap_record_t *)heap_caps_malloc(SCAN_MAX * sizeof *g_recs, MALLOC_CAP_SPIRAM);
    if (!g_recs || esp_wifi_scan_get_ap_records(&n, g_recs) != ESP_OK) { n = 0; esp_wifi_clear_ap_list(); }
    g_nrec = n;
    g_scan = SCAN_DONE;
  }
  return g_scan;
}
static void scan_clear(void) { if (g_scan == SCAN_DONE) { g_scan = SCAN_IDLE; g_nrec = 0; } }
bool wifi_scan_running(void) { return scan_state() == SCAN_RUNNING; }

static bool g_retry_req = false;                   // "Try again" on the offline screen (under g_net_mtx)
static volatile bool g_roam_force = false;
// scan / join requests from the UI task, results back to it (all under g_net_mtx)
static bool g_scan_req = false, g_scanning = false;
static bd_wifi_net g_nets[BD_MAX_NETS];
static int g_nets_n = 0;
static uint32_t g_nets_seq = 0, g_nets_seen = 0;
static bool g_join_req = false;
static char g_join_ssid[33], g_join_pass[65];
static int g_join_state = BD_JOIN_IDLE;
static uint32_t g_join_seq = 0, g_join_seen = 0;
static bool g_ever_joined = false;        // joined at least once since boot
static int wifi_why(uint8_t r) {                   // ESP-IDF wifi_err_reason_t
  if (r == 201 || (r >= 210 && r <= 212)) return BD_WHY_WIFI_NO_AP;           // network not seen
  // auth expired / handshake / auth fail: the password, unless it already worked since boot (a router that blocks or
  // drops the screen answers with the same codes)
  if ((r == 2 || r == 15 || r == 202 || r == 204) && !g_ever_joined) return BD_WHY_WIFI_AUTH;
  return BD_WHY_WIFI_OTHER;
}
bool wifi_down_why(int *why) {
  if (sta_up()) return false;
  *why = wifi_why(g_disc_reason);
  return true;
}

static void wifi_scan_step(void) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool req = g_scan_req;
  g_scan_req = false;
  xSemaphoreGive(g_net_mtx);
  if (req && scan_state() != SCAN_RUNNING) {
    if (!scan_begin(300)) {
      Log.println("wifi: scan failed to start");
    } else {
      xSemaphoreTake(g_net_mtx, portMAX_DELAY);
      g_scanning = true;
      g_nets_seq++;
      xSemaphoreGive(g_net_mtx);
    }
  }
  if (!g_scanning) return;
  if (scan_state() == SCAN_RUNNING) return;
  const int n = g_nrec;
  static bd_wifi_net nets[BD_MAX_NETS];
  int k = 0;
  for (int i = 0; i < n; i++) {                   // one row per name (mesh: several APs), strongest kept
    const char *nm = (const char *)g_recs[i].ssid;
    if (!nm[0] || strlen(nm) >= sizeof nets[0].ssid) continue;
    const int rssi = g_recs[i].rssi;
    int j = 0;
    while (j < k && strcmp(nets[j].ssid, nm) != 0) j++;
    if (j < k) { if (rssi > nets[j].rssi) nets[j].rssi = (int8_t)rssi; continue; }
    if (k == BD_MAX_NETS) continue;
    snprintf(nets[k].ssid, sizeof nets[k].ssid, "%s", nm);
    nets[k].rssi = (int8_t)rssi;
    nets[k].locked = g_recs[i].authmode != WIFI_AUTH_OPEN;
    k++;
  }
  for (int a = 1; a < k; a++)                     // strongest first
    for (int b = a; b > 0 && nets[b].rssi > nets[b - 1].rssi; b--) {
      bd_wifi_net t = nets[b];
      nets[b] = nets[b - 1];
      nets[b - 1] = t;
    }
  scan_clear();
  Log.printf("wifi: scan found %d networks (%d names)\n", n < 0 ? 0 : n, k);
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  memcpy(g_nets, nets, sizeof nets);
  g_nets_n = k;
  g_scanning = false;
  g_nets_seq++;
  xSemaphoreGive(g_net_mtx);
}

static void join_publish(int st) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_join_state = st;
  g_join_seq++;
  xSemaphoreGive(g_net_mtx);
}
// ---- roaming between the access points of one network (mesh) -------------------------------------------------
// The ESP32 picks an AP only when it joins and never roams: after some boots it sat on a far AP (-65..-70 dBm, the
// random "Offline": WS "no data for 5 s", REST failing) next to one at -45. While idle and weak, scan now and then
// and move to the same network's AP when it is clearly stronger. Pinned to that AP afterwards (joined with its
// BSSID), so if the link stays down for 15 s the pin is dropped and any AP is taken again.
#define ROAM_WEAK_DBM   -60       // average below this: look for a better AP
#define ROAM_GAIN_DB    8         // move only for at least this much more signal
#define ROAM_EVERY_MS   300000    // at most one roaming scan per 5 min (scans pause traffic ~1-2 s)
#define ROAM_SETTLE_MS  60000     // no scan in the first minute after a join
static bool g_roam_scanning = false, g_pinned = false;
static uint32_t g_roam_last = 0, g_join_ms = 0, g_down_ms = 0;
static float g_rssi_avg = 0;

void net_roam_check(void) { g_roam_force = true; }

static void log_ap(const char *what) {
  wifi_ap_record_t a;
  char m[18] = "?";
  if (!ap_now(&a)) memset(&a, 0, sizeof a); else mac_str(a.bssid, m, sizeof m);
  Log.printf("wifi: %s AP %s, channel %d, %d dBm\n", what, m, (int)a.primary, (int)a.rssi);
}

// leave the current AP and join again: any AP of the network (bssid NULL), or that one on its channel
static void rejoin(const uint8_t *bssid, int ch) {
  sta_leave();
  delay(100);
  sta_join(g_ssid, g_pass, ch, bssid);
}

// not connected: retry a dropped first join quickly, otherwise join again every 15 s
static void roam_down(uint32_t now) {
  if (!g_down_ms) g_down_ms = stamp_nz(now);
  // the first join after boot dropped: try again after 1 s, up to 3 times, instead of waiting 15 s (an ESP32-P4
  // board whose ESP32-C6 runs older ESP-Hosted firmware drops the first join at every boot)
  static uint8_t quick = 0;
  if (!g_ever_joined && g_disc_ms && now - g_disc_ms > 1000 && quick < 3 && g_ssid[0]) {
    quick++;
    g_disc_ms = 0;
    Log.printf("wifi: first join dropped (reason %u), trying again now (%u of 3)\n", (unsigned)g_disc_reason,
               (unsigned)quick);
    rejoin(NULL, 0);
    g_down_ms = stamp_nz(now);
    return;
  }
  // down for 15 s: join again ourselves, every 15 s. The automatic retry (on_event) skips some reasons: reason 8
  // (ASSOC_LEAVE, also sent by access points being re-provisioned) left the screen offline for good.
  if (now - g_down_ms > 15000 && g_ssid[0]) {
    Log.printf("wifi: not connected for 15 s (last reason %u)%s, joining again\n", (unsigned)g_disc_reason,
               g_pinned ? ", leaving the pinned AP" : "");
    g_pinned = false;
    rejoin(NULL, 0);
    g_down_ms = stamp_nz(now);
  }
  g_rssi_avg = 0;
}

// connected: note a (re)join, and keep a smoothed signal level (one sample per 2 s)
static void roam_signal(uint32_t now) {
  if (g_down_ms || !g_join_ms) {                    // (re)joined
    g_ever_joined = true;
    g_down_ms = 0;
    g_join_ms = stamp_nz(now);
    log_ap("joined");
  }
  static uint32_t t_s;
  if (now - t_s >= 2000) {
    t_s = now;
    const int r = rssi_now();
    if (r < 0) g_rssi_avg = g_rssi_avg == 0 ? r : g_rssi_avg * 0.9f + r * 0.1f;
  }
}

// start a roaming scan when asked (debug /roam) or when the signal is weak while idle
static void roam_scan_start(uint32_t now, bool idle) {
  const bool force = g_roam_force;
  const bool due = idle && g_rssi_avg != 0 && g_rssi_avg < ROAM_WEAK_DBM && now - g_join_ms > ROAM_SETTLE_MS &&
                   (!g_roam_last || now - g_roam_last > ROAM_EVERY_MS);
  if (!force && !due) return;
  if (g_scanning || g_scan_req || scan_state() == SCAN_RUNNING) return;   // the Wi-Fi page scans
  g_roam_force = false;
  g_roam_last = stamp_nz(now);
  scan_clear();
  // active scan, 120 ms per channel (default 300): ~1.6 s for 13 channels, the station goes back to its own
  // channel between them
  if (!scan_begin(120)) {
    Log.println("wifi: roaming scan failed to start");
    return;
  }
  g_roam_scanning = true;
  wifi_ap_record_t a;
  char m[18] = "?";
  if (ap_now(&a)) mac_str(a.bssid, m, sizeof m);
  Log.printf("wifi: roaming scan (%s, average %.0f dBm on %s)\n", force ? "asked" : "weak signal", g_rssi_avg, m);
}

// the roaming scan's result: move to the network's strongest other AP if it is ROAM_GAIN_DB stronger
static void roam_scan_done(void) {
  if (scan_state() == SCAN_RUNNING) return;
  g_roam_scanning = false;
  wifi_ap_record_t a;
  if (!ap_now(&a)) { scan_clear(); return; }        // dropped meanwhile: roam_down takes over
  // the current AP's level from the same scan: the driver's value right after a scan can be off (-72 dBm logged
  // while the scan showed the current AP at -61), and comparing a scan value with it could move to a weaker AP
  int cur_rssi = a.rssi;
  int best = -1, best_rssi = -127;
  char m[18];
  for (int i = 0; i < g_nrec; i++) {
    const wifi_ap_record_t *r = &g_recs[i];
    if (strcmp((const char *)r->ssid, g_ssid) != 0) continue;
    const bool same = memcmp(r->bssid, a.bssid, 6) == 0;
    mac_str(r->bssid, m, sizeof m);
    Log.printf("wifi:   %s channel %d %d dBm%s\n", m, (int)r->primary, (int)r->rssi, same ? " (current)" : "");
    if (same) cur_rssi = r->rssi;
    else if (r->rssi > best_rssi) { best = i; best_rssi = r->rssi; }
  }
  if (best >= 0 && best_rssi >= cur_rssi + ROAM_GAIN_DB) {
    uint8_t bssid[6];
    memcpy(bssid, g_recs[best].bssid, 6);
    const int ch = g_recs[best].primary;
    mac_str(bssid, m, sizeof m);
    Log.printf("wifi: moving to AP %s, channel %d (%d dBm, now %d)\n", m, ch, best_rssi, cur_rssi);
    scan_clear();
    rejoin(bssid, ch);
    g_pinned = true;
    g_down_ms = stamp_nz(millis());
  } else {
    Log.printf("wifi: staying (best other AP %d dBm, this one %d)\n", best_rssi, cur_rssi);
    scan_clear();
  }
}

// called every pass of the net task; idle = nothing running on the machine and no review / lookup / user scan
static void roam_step(bool idle) {
  const uint32_t now = millis();
  if (!sta_up()) { roam_down(now); return; }
  roam_signal(now);
  if (!g_roam_scanning) roam_scan_start(now, idle);
  else roam_scan_done();
}

static bool wifi_join_step(void) {
  static char ssid[33], pass[65];
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool req = g_join_req;
  if (req) {
    memcpy(ssid, g_join_ssid, sizeof ssid);
    memcpy(pass, g_join_pass, sizeof pass);
    memset(g_join_pass, 0, sizeof g_join_pass);
    g_join_req = false;
  }
  xSemaphoreGive(g_net_mtx);
  if (!req) return false;
  g_pinned = false;                                  // a new network: any of its APs
  join_publish(BD_JOIN_BUSY);
  Log.printf("wifi: joining \"%s\" from the menu\n", ssid);
  sta_leave();
  delay(200);
  wifi_info_step(true);                              // the UI shows the old network as gone at once
  g_disc_reason = 0;
  sta_join(ssid, pass);
  const uint32_t t0 = millis();
  bool ok = false;
  while (millis() - t0 < 20000) {
    drop_step();
    if (sta_up()) { ok = true; break; }
    const uint8_t r = g_disc_reason;               // 15 4-way handshake timeout, 202 auth fail, 204 handshake timeout
    if (millis() - t0 > 4000 && (r == 15 || r == 202 || r == 204)) break;
    wifi_info_step();                                // this loop holds the network task for up to 20 s
    delay(200);
  }
  g_down_ms = 0;                                     // the 15 s rejoin timer starts again from here
  if (ok) {
    kv_set_str("wifi", "ssid", ssid);
    kv_set_str("wifi", "pass", pass);
    kv_erase("wifi", "nobuiltin");
    g_no_builtin = false;
    xSemaphoreTake(g_net_mtx, portMAX_DELAY);           // g_ssid is read by the core loop (net_wifi_target)
    snprintf(g_ssid, sizeof g_ssid, "%s", ssid);
    xSemaphoreGive(g_net_mtx);
    snprintf(g_pass, sizeof g_pass, "%s", pass);
    g_saved_net = true;
    Log.printf("wifi: joined \"%s\" (saved), %d dBm\n", ssid, rssi_now());
  } else {
    Log.printf("wifi: could not join \"%s\" (reason %u), back to \"%s\"\n", ssid, (unsigned)g_disc_reason, g_ssid);
    sta_leave();
    delay(200);
    if (g_ssid[0]) sta_join(g_ssid, g_pass);
  }
  memset(pass, 0, sizeof pass);
  wifi_info_step(true);
  join_publish(ok ? BD_JOIN_OK : BD_JOIN_FAILED);
  return true;
}

// requests from the UI task: forget, try again, the built-in network after 30 s, roaming. Network task only.
// idle: nothing running on the machine and no review / shot lookup open. True = the link was restarted (forget).
bool wifi_step(bool idle) {
  drop_step();
  wifi_info_step();
  wifi_scan_step();
  if (wifi_join_step()) return true;
  bool restarted = false;
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool wreq = g_retry_req, joining = g_join_req;
  g_retry_req = false;
  xSemaphoreGive(g_net_mtx);
  if (g_forget_req) {
    g_forget_req = false;
    kv_erase("wifi", "ssid");
    kv_erase("wifi", "pass");
    kv_set_bool("wifi", "nobuiltin", true);
    Log.printf("wifi: forgot \"%s\" (saved password deleted, built-in network off)\n", g_ssid);
    xSemaphoreTake(g_net_mtx, portMAX_DELAY);
    g_ssid[0] = 0;
    xSemaphoreGive(g_net_mtx);
    memset(g_pass, 0, sizeof g_pass);
    g_saved_net = false;
    g_no_builtin = true;
    sta_leave();
    restarted = true;
  }
  if (wreq && g_ssid[0] && !sta_up()) {
    Log.printf("wifi: trying \"%s\" again (from the screen)\n", g_ssid);
    sta_leave();
    delay(200);
    g_pinned = false;
    sta_join(g_ssid, g_pass);
    g_down_ms = 0;
  }
  roam_step(idle && !joining);
  // saved network not found within 30 s after boot: try the built-in one (once). Only if it was never joined:
  // a later drop (or a roaming move) is not a reason to switch networks
  static bool fell_back = false;
  if (g_saved_net && !g_no_builtin && !fell_back && !g_ever_joined && compiled_net() && !sta_up() &&
      millis() - g_start_ms > 30000) {
    fell_back = true;
    Log.printf("wifi: saved network not joined, trying the built-in \"%s\"\n", WIFI_SSID);
    xSemaphoreTake(g_net_mtx, portMAX_DELAY);
    snprintf(g_ssid, sizeof g_ssid, "%s", WIFI_SSID);
    xSemaphoreGive(g_net_mtx);
    snprintf(g_pass, sizeof g_pass, "%s", WIFI_PASS);
    sta_leave();
    sta_join(g_ssid, g_pass);
  }
  return restarted;
}

void wifi_start(void) {
  g_start_ms = millis();
  if (g_ssid[0]) sta_join(g_ssid, g_pass);
  if (g_ssid[0]) Log.printf("wifi: joining \"%s\" (%s)\n", g_ssid, g_saved_net ? "saved on the screen" : "built in");
  else Log.println("wifi: no network set up: choose one on the screen");
}

static bool wifi_fail(const char *what, esp_err_t e) {
  Log.printf("wifi: %s failed (%s): no Wi-Fi\n", what, esp_err_to_name(e));
  return false;
}
static bool wifi_driver_start(void) {
  esp_err_t e = esp_netif_init();
  if (e != ESP_OK) return wifi_fail("esp_netif_init", e);
  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return wifi_fail("event loop", e);   // INVALID_STATE: exists
  if (!board_wifi_init()) return false;
  g_sta = esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
#if CONFIG_ESP_HOSTED_ENABLED
  cfg.nvs_enable = false;                           // ESP-Hosted: the driver's NVS lives on the ESP32-C6
#endif
  // buffers as the Arduino core set them (measured with these: internal RAM, Wi-Fi throughput): TX dynamic only
  cfg.static_tx_buf_num = 0;
  cfg.dynamic_tx_buf_num = 32;
  cfg.tx_buf_type = 1;
  cfg.cache_tx_buf_num = 4;
  cfg.static_rx_buf_num = 4;
  cfg.dynamic_rx_buf_num = 32;
  if ((e = esp_wifi_init(&cfg)) != ESP_OK) return wifi_fail("esp_wifi_init", e);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL);
  esp_netif_set_hostname(g_sta, board_hostname());   // one name per board kind: both may be on one network
  if ((e = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return wifi_fail("esp_wifi_set_mode", e);
  if ((e = esp_wifi_start()) != ESP_OK) return wifi_fail("esp_wifi_start", e);
  // no modem sleep: its beacon wake-ups add bursts of flash/PSRAM traffic that disturb the panel
  esp_wifi_set_ps(WIFI_PS_NONE);
  return true;
}

void wifi_setup(void) {
  load_creds();
  if (!wifi_driver_start()) return;
  // local time for the idle screen's clock (NTP from the internet, nothing to/from the machine). Time zone: set
  // GAGGI_TZ in wifi_config.h to override (POSIX TZ string); default Central Europe with summer time.
  setenv("TZ", GAGGI_TZ, 1);
  tzset();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.google.com");
  esp_sntp_init();
}

// ---- the UI task's side (net.h) -----------------------------------------------------------------------------------
void net_wifi_forget(void) {
  g_forget_req = true;
  net_wake();
}
void net_wifi_retry(void) {
  if (!g_net_mtx) return;
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_retry_req = true;
  xSemaphoreGive(g_net_mtx);
}
void net_wifi_target(char *out, size_t n) {
  if (!n) return;
  out[0] = 0;
  if (!g_net_mtx) return;
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  snprintf(out, n, "%s", g_ssid);
  xSemaphoreGive(g_net_mtx);
}
void net_wifi_scan(void) {
  if (!g_net_mtx) return;
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_scan_req = true;
  xSemaphoreGive(g_net_mtx);
}
bool net_get_wifi_scan(bd_wifi_net *out, int *n, bool *scanning) {
  if (!g_net_mtx) return false;
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool ch = g_nets_seq != g_nets_seen;
  g_nets_seen = g_nets_seq;
  memcpy(out, g_nets, sizeof g_nets);
  *n = g_nets_n;
  *scanning = g_scanning || g_scan_req;
  xSemaphoreGive(g_net_mtx);
  return ch;
}
void net_wifi_join(const char *ssid, const char *pass) {
  if (!g_net_mtx) return;
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  snprintf(g_join_ssid, sizeof g_join_ssid, "%s", ssid);
  snprintf(g_join_pass, sizeof g_join_pass, "%s", pass);
  g_join_req = true;
  g_join_state = BD_JOIN_BUSY;
  g_join_seq++;
  xSemaphoreGive(g_net_mtx);
  net_wake();
}
bool net_get_wifi_join(int *state) {
  if (!g_net_mtx) return false;
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool ch = g_join_seq != g_join_seen;
  g_join_seen = g_join_seq;
  *state = g_join_state;
  xSemaphoreGive(g_net_mtx);
  return ch;
}

// the joined network, read by the network task (wifi_info_step), handed to the UI task under g_net_mtx
static struct { bool up; char ssid[33]; int8_t rssi; int channel; char ap[18]; char ip[16]; } g_winfo;
void wifi_info_step(bool now) {   // now: read at once (a join started or ended), else at most once a second
  static uint32_t last = 0;
  if (!now && last && millis() - last < 1000) return;
  last = stamp_nz(millis());
  const uint32_t t0 = millis();
  wifi_ap_record_t a;
  bool up = ap_now(&a);
  char ssid[33] = "", ap[18] = "", ip[16] = "";
  int8_t rssi = 0;
  int ch = 0;
  if (up) {
    snprintf(ssid, sizeof ssid, "%s", (const char *)a.ssid);
    rssi = a.rssi;
    ch = a.primary;
    mac_str(a.bssid, ap, sizeof ap);
    ip_str(g_ip, ip, sizeof ip);
  }
  const uint32_t took = millis() - t0;
  static uint32_t last_log = 0;
  if (took > 200 && (!last_log || millis() - last_log > 30000)) {
    last_log = stamp_nz(millis());
    Log.printf("wifi: reading the link status took %u ms\n", (unsigned)took);
  }
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_winfo.up = up;
  memcpy(g_winfo.ssid, ssid, sizeof ssid);
  g_winfo.rssi = rssi;
  g_winfo.channel = ch;
  memcpy(g_winfo.ap, ap, sizeof ap);
  memcpy(g_winfo.ip, ip, sizeof ip);
  xSemaphoreGive(g_net_mtx);
}
bool net_wifi_info(char *ssid, size_t scap, int8_t *rssi, int *channel, char *ap, size_t acap) {
  if (!g_net_mtx) { if (scap) ssid[0] = 0; if (acap) ap[0] = 0; *rssi = 0; *channel = 0; return false; }
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool up = g_winfo.up;
  snprintf(ssid, scap, "%s", up ? g_winfo.ssid : "");
  snprintf(ap, acap, "%s", up ? g_winfo.ap : "");
  *rssi = up ? g_winfo.rssi : 0;
  *channel = up ? g_winfo.channel : 0;
  xSemaphoreGive(g_net_mtx);
  return up;
}

bool net_wifi_up(int8_t *rssi) {
  if (!g_net_mtx) return false;
  NetLock l;
  if (rssi) *rssi = g_winfo.up ? g_winfo.rssi : 0;
  return g_winfo.up;
}

bool wifi_screen_ip(char *out, size_t cap) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool up = g_winfo.up;
  snprintf(out, cap, "%s", up ? g_winfo.ip : "");
  xSemaphoreGive(g_net_mtx);
  return up;
}
