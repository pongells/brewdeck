// The screen's own Wi-Fi: the network in use (saved on the screen, else the one in wifi_config.h), joining one from the
// menu, scans, "Forget", roaming between the access points of one network, rejoining after a drop, and the link status
// the UI shows. All of it runs on the network task (net.cpp calls wifi_step / wifi_info_step every pass); on the ESP32-P4
// every Wi-Fi query is a request to its ESP32-C6 and can block, so the UI task never asks Wi-Fi itself.
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <string.h>
#include "stamp.h"
#include "tlog.h"
#include "wifi_config.h"
#include "net.h"
#include "net_int.h"
#include "board_core.h"

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
  Preferences p;
  g_saved_net = false;
  g_no_builtin = false;
  if (p.begin("wifi", true)) {
    String s = p.getString("ssid", ""), w = p.getString("pass", "");
    g_no_builtin = p.getBool("nobuiltin", false);
    p.end();
    if (s.length() && s.length() < sizeof g_ssid && w.length() < sizeof g_pass) {
      snprintf(g_ssid, sizeof g_ssid, "%s", s.c_str());
      snprintf(g_pass, sizeof g_pass, "%s", w.c_str());
      g_saved_net = true;
      return;
    }
  }
  if (g_no_builtin || !compiled_net()) { g_ssid[0] = 0; g_pass[0] = 0; return; }
  snprintf(g_ssid, sizeof g_ssid, "%s", WIFI_SSID);
  snprintf(g_pass, sizeof g_pass, "%s", WIFI_PASS);
}

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
static volatile uint8_t g_disc_reason = 0;
static volatile uint32_t g_disc_ms = 0;   // when the last disconnect event came (stamp_nz), for the quick first retries
static bool g_ever_joined = false;        // joined at least once since boot
static int wifi_why(uint8_t r) {                   // ESP-IDF wifi_err_reason_t
  if (r == 201 || (r >= 210 && r <= 212)) return BD_WHY_WIFI_NO_AP;           // network not seen
  // auth expired / handshake / auth fail: the password, unless it already worked since boot (a router that blocks or
  // drops the screen answers with the same codes)
  if ((r == 2 || r == 15 || r == 202 || r == 204) && !g_ever_joined) return BD_WHY_WIFI_AUTH;
  return BD_WHY_WIFI_OTHER;
}
bool wifi_down_why(int *why) {
  if (WiFi.status() == WL_CONNECTED) return false;
  *why = wifi_why(g_disc_reason);
  return true;
}

static void wifi_scan_step(void) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool req = g_scan_req;
  g_scan_req = false;
  xSemaphoreGive(g_net_mtx);
  if (req && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    if (WiFi.scanNetworks(true /* async */, false /* no hidden */) == WIFI_SCAN_FAILED) {
      Log.println("wifi: scan failed to start");
    } else {
      xSemaphoreTake(g_net_mtx, portMAX_DELAY);
      g_scanning = true;
      g_nets_seq++;
      xSemaphoreGive(g_net_mtx);
    }
  }
  if (!g_scanning) return;
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  static bd_wifi_net nets[BD_MAX_NETS];
  int k = 0;
  for (int i = 0; i < n; i++) {                   // one row per name (mesh: several APs), strongest kept
    String nm = WiFi.SSID(i);
    if (!nm.length() || nm.length() >= sizeof nets[0].ssid) continue;
    const int rssi = WiFi.RSSI(i);
    int j = 0;
    while (j < k && strcmp(nets[j].ssid, nm.c_str()) != 0) j++;
    if (j < k) { if (rssi > nets[j].rssi) nets[j].rssi = (int8_t)rssi; continue; }
    if (k == BD_MAX_NETS) continue;
    snprintf(nets[k].ssid, sizeof nets[k].ssid, "%s", nm.c_str());
    nets[k].rssi = (int8_t)rssi;
    nets[k].locked = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    k++;
  }
  for (int a = 1; a < k; a++)                     // strongest first
    for (int b = a; b > 0 && nets[b].rssi > nets[b - 1].rssi; b--) {
      bd_wifi_net t = nets[b];
      nets[b] = nets[b - 1];
      nets[b - 1] = t;
    }
  WiFi.scanDelete();
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
// and move to the same network's AP when it is clearly stronger. Pinned to that AP afterwards (WiFi.begin with a
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
  Log.printf("wifi: %s AP %s, channel %d, %d dBm\n", what, WiFi.BSSIDstr().c_str(), (int)WiFi.channel(), (int)WiFi.RSSI());
}

// leave the current AP and join again: any AP of the network (bssid NULL), or that one on its channel
static void rejoin(const uint8_t *bssid, int ch) {
  WiFi.disconnect(false);
  delay(100);
  if (bssid) WiFi.begin(g_ssid, g_pass, ch, bssid);
  else WiFi.begin(g_ssid, g_pass);
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
  // down for 15 s: join again ourselves, every 15 s. The Arduino core's auto-reconnect skips some reasons: reason 8
  // (ASSOC_LEAVE, sent by access points being re-provisioned) left the screen offline for good.
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
    const int r = WiFi.RSSI();
    if (r < 0) g_rssi_avg = g_rssi_avg == 0 ? r : g_rssi_avg * 0.9f + r * 0.1f;
  }
}

// start a roaming scan when asked (debug /roam) or when the signal is weak while idle
static void roam_scan_start(uint32_t now, bool idle) {
  const bool force = g_roam_force;
  const bool due = idle && g_rssi_avg != 0 && g_rssi_avg < ROAM_WEAK_DBM && now - g_join_ms > ROAM_SETTLE_MS &&
                   (!g_roam_last || now - g_roam_last > ROAM_EVERY_MS);
  if (!force && !due) return;
  if (g_scanning || g_scan_req || WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;   // the Wi-Fi page scans
  g_roam_force = false;
  g_roam_last = stamp_nz(now);
  WiFi.scanDelete();
  // active scan, 120 ms per channel (default 300): ~1.6 s for 13 channels, the station goes back to its own
  // channel between them
  if (WiFi.scanNetworks(true, false, false, 120) == WIFI_SCAN_FAILED) {
    Log.println("wifi: roaming scan failed to start");
    return;
  }
  g_roam_scanning = true;
  Log.printf("wifi: roaming scan (%s, average %.0f dBm on %s)\n", force ? "asked" : "weak signal", g_rssi_avg,
             WiFi.BSSIDstr().c_str());
}

// the roaming scan's result: move to the network's strongest other AP if it is ROAM_GAIN_DB stronger
static void roam_scan_done(void) {
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  g_roam_scanning = false;
  uint8_t cur[6];
  memcpy(cur, WiFi.BSSID(), 6);
  // the current AP's level from the same scan: WiFi.RSSI() right after a scan can be off (-72 dBm logged while the
  // scan showed the current AP at -61), and comparing a scan value with it could move to a weaker AP
  int cur_rssi = WiFi.RSSI();
  int best = -1, best_rssi = -127;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != g_ssid) continue;
    const bool same = memcmp(WiFi.BSSID(i), cur, 6) == 0;
    Log.printf("wifi:   %s channel %d %d dBm%s\n", WiFi.BSSIDstr(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i),
               same ? " (current)" : "");
    if (same) cur_rssi = WiFi.RSSI(i);
    else if (WiFi.RSSI(i) > best_rssi) { best = i; best_rssi = WiFi.RSSI(i); }
  }
  if (best >= 0 && best_rssi >= cur_rssi + ROAM_GAIN_DB) {
    uint8_t bssid[6];
    memcpy(bssid, WiFi.BSSID(best), 6);
    const int ch = WiFi.channel(best);
    Log.printf("wifi: moving to AP %s, channel %d (%d dBm, now %d)\n", WiFi.BSSIDstr(best).c_str(), ch, best_rssi,
               cur_rssi);
    WiFi.scanDelete();
    rejoin(bssid, ch);
    g_pinned = true;
    g_down_ms = stamp_nz(millis());
  } else {
    Log.printf("wifi: staying (best other AP %d dBm, this one %d)\n", best_rssi, cur_rssi);
    WiFi.scanDelete();
  }
}

// called every pass of the net task; idle = nothing running on the machine and no review / lookup / user scan
static void roam_step(bool idle) {
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) { roam_down(now); return; }
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
  WiFi.disconnect(false);
  delay(200);
  wifi_info_step(true);                              // the UI shows the old network as gone at once
  g_disc_reason = 0;
  WiFi.begin(ssid, pass);
  const uint32_t t0 = millis();
  bool ok = false;
  while (millis() - t0 < 20000) {
    if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
    const uint8_t r = g_disc_reason;               // 15 4-way handshake timeout, 202 auth fail, 204 handshake timeout
    if (millis() - t0 > 4000 && (r == 15 || r == 202 || r == 204)) break;
    wifi_info_step();                                // this loop holds the network task for up to 20 s
    delay(200);
  }
  g_down_ms = 0;                                     // the 15 s rejoin timer starts again from here
  if (ok) {
    Preferences p;
    if (p.begin("wifi", false)) { p.putString("ssid", ssid); p.putString("pass", pass); p.remove("nobuiltin"); p.end(); }
    g_no_builtin = false;
    xSemaphoreTake(g_net_mtx, portMAX_DELAY);           // g_ssid is read by the core loop (net_wifi_target)
    snprintf(g_ssid, sizeof g_ssid, "%s", ssid);
    xSemaphoreGive(g_net_mtx);
    snprintf(g_pass, sizeof g_pass, "%s", pass);
    g_saved_net = true;
    Log.printf("wifi: joined \"%s\" (saved), %d dBm\n", ssid, (int)WiFi.RSSI());
  } else {
    Log.printf("wifi: could not join \"%s\" (reason %u), back to \"%s\"\n", ssid, (unsigned)g_disc_reason, g_ssid);
    WiFi.disconnect(false);
    delay(200);
    if (g_ssid[0]) WiFi.begin(g_ssid, g_pass);
  }
  memset(pass, 0, sizeof pass);
  wifi_info_step(true);
  join_publish(ok ? BD_JOIN_OK : BD_JOIN_FAILED);
  return true;
}

// requests from the UI task: forget, try again, the built-in network after 30 s, roaming. Network task only.
// idle: nothing running on the machine and no review / shot lookup open. True = the link was restarted (forget).
bool wifi_step(bool idle) {
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
    Preferences p;
    if (p.begin("wifi", false)) { p.remove("ssid"); p.remove("pass"); p.putBool("nobuiltin", true); p.end(); }
    Log.printf("wifi: forgot \"%s\" (saved password deleted, built-in network off)\n", g_ssid);
    xSemaphoreTake(g_net_mtx, portMAX_DELAY);
    g_ssid[0] = 0;
    xSemaphoreGive(g_net_mtx);
    memset(g_pass, 0, sizeof g_pass);
    g_saved_net = false;
    g_no_builtin = true;
    WiFi.disconnect(false);
    restarted = true;
  }
  if (wreq && g_ssid[0] && WiFi.status() != WL_CONNECTED) {
    Log.printf("wifi: trying \"%s\" again (from the screen)\n", g_ssid);
    WiFi.disconnect(false);
    delay(200);
    g_pinned = false;
    WiFi.begin(g_ssid, g_pass);
    g_down_ms = 0;
  }
  roam_step(idle && !joining);
  // saved network not found within 30 s after boot: try the built-in one (once). Only if it was never joined:
  // a later drop (or a roaming move) is not a reason to switch networks
  static bool fell_back = false;
  if (g_saved_net && !g_no_builtin && !fell_back && !g_ever_joined && compiled_net() && WiFi.status() != WL_CONNECTED &&
      millis() - g_start_ms > 30000) {
    fell_back = true;
    Log.printf("wifi: saved network not joined, trying the built-in \"%s\"\n", WIFI_SSID);
    xSemaphoreTake(g_net_mtx, portMAX_DELAY);
    snprintf(g_ssid, sizeof g_ssid, "%s", WIFI_SSID);
    xSemaphoreGive(g_net_mtx);
    snprintf(g_pass, sizeof g_pass, "%s", WIFI_PASS);
    WiFi.disconnect(false);
    WiFi.begin(g_ssid, g_pass);
  }
  return restarted;
}

void wifi_start(void) {
  g_start_ms = millis();
  if (g_ssid[0]) WiFi.begin(g_ssid, g_pass);
  if (g_ssid[0]) Log.printf("wifi: joining \"%s\" (%s)\n", g_ssid, g_saved_net ? "saved on the screen" : "built in");
  else Log.println("wifi: no network set up: choose one on the screen");
}

void wifi_setup(void) {
  load_creds();
  WiFi.persistent(false);
  WiFi.setHostname(board_hostname());   // one name per board kind: both may be on one network
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // no modem sleep: its beacon wake-ups add bursts of flash/PSRAM traffic that disturb the panel
  // local time for the idle screen's clock (NTP from the internet, nothing to/from the machine). Time zone: set
  // GAGGI_TZ in wifi_config.h to override (POSIX TZ string); default Central Europe with summer time.
  configTzTime(GAGGI_TZ, "pool.ntp.org", "time.google.com");
  // several access points can share a network name: by default the ESP32 joins the FIRST one it finds (seen: -64 to
  // -70 dBm right next to an access point, with stalls); scan every channel and take the strongest instead
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {   // debug: why the link dropped
    g_disc_reason = info.wifi_sta_disconnected.reason;
    g_disc_ms = stamp_nz(millis());
    Log.printf("wifi: disconnected, reason %u\n", (unsigned)info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
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
  bool up = WiFi.status() == WL_CONNECTED;
  char ssid[33] = "", ap[18] = "", ip[16] = "";
  int8_t rssi = 0;
  int ch = 0;
  if (up) {
    snprintf(ssid, sizeof ssid, "%s", WiFi.SSID().c_str());
    rssi = (int8_t)WiFi.RSSI();
    ch = WiFi.channel();
    snprintf(ap, sizeof ap, "%s", WiFi.BSSIDstr().c_str());
    snprintf(ip, sizeof ip, "%s", WiFi.localIP().toString().c_str());
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
