// Connects to Wi-Fi and polls the Gaggiuino REST API (live values also arrive ~3/s over WebSocket, see ws.cpp):
//   /api/system/status   about once a second
//   /api/profiles/all    on first contact, every 30 s, and soon after the active profile changes
//   /api/profile/<id>    once per profile, for the summary line (phases, stop weight, temperature), and the
//                        active profile again whenever the list is refreshed (preview graph + target weight)
// One command: POST /api/profile-select/<id> when a profile is tapped, sent only
// while the machine is idle in HOME. Same effect as a switch in the web UI: active now, the boot profile is not
// changed. DANGER: DELETE on the same URL deletes the profile, so http() can only emit GET or POST.
#include "stamp.h"
#include <Arduino.h>
#include "tlog.h"
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <string.h>
#include <stdlib.h>
#include "wifi_config.h"
#include "net.h"
#include "net_ws.h"
#include "parse.h"
#include <Preferences.h>

static LiveData g_data;
static int g_link = LINK_CONNECTING;
static uint32_t g_seq = 0, g_seen = 0;
static ProfileList g_prof;
static uint32_t g_pseq = 0, g_pseen = 0;
static ProfileShape g_shape;
static uint32_t g_sseq = 0, g_sseen = 0;
static int g_select_req = 0;          // profile id to select (0 = none), set by the UI task
static uint32_t g_select_ms = 0;      // when it was asked for: dropped after 5 s (the core answers
                                      // NOT_TAKEN then; a select queued during a shot would fire after it)
static bool g_force_list = false;     // re-read the profile list on the next cycle (after a select)
static volatile int g_redetail_id = 0; // a profile saved from this screen: re-read its stored values (net_profile_saved)
static SemaphoreHandle_t g_mtx;
static TaskHandle_t g_task = NULL;
static IPAddress g_ip;                // machine address for the WebSocket task
static bool g_have_ip = false;
// WebSocket data (ws.cpp): overlaid on the REST values in net_get() while fresh
static WsSensor g_wss;
static WsSystem g_wsy;
static uint32_t g_wss_ms = 0, g_wsy_ms = 0;
static bool g_wss_ok = false, g_wsy_ok = false;
static WsShot g_shotq[32];           // shot snapshots in arrival order (~7/s; the core loop drains it every pass)
static uint32_t g_shot_w = 0, g_shot_r = 0;
static const uint32_t WS_FRESH_MS = 2000;
// previous shot for the shot review (GET /api/shots/latest + /api/shots/<id>, sent only after a
// shot while the machine is idle)
static WsShotIndex *g_index = NULL;   // newest 50 shots, from the WebSocket (d_shot_hist_index); PSRAM, net_begin
static bool g_index_ok = false;
static uint32_t g_index_seq = 0, g_index_seen = 0;
// Shot history (menu > Shot history): on a tap, GET /api/shots/<id> of that shot and of the next
// older stored shot of the same profile (its "vs Previous shot"), only while the machine is idle. PSRAM.
static uint32_t g_hist_req = 0;       // id asked for, 0 = none
static int g_hist_state = PREV_NONE, g_hist_prev_state = PREV_NONE;
static ShotTrace *g_hist = NULL, *g_hist_prev = NULL;
static ProfileShape *g_hist_shape = NULL;
static uint32_t g_hist_seq = 0, g_hist_seen = 0;
static char *g_shot_buf = NULL;       // one reply of /api/shots/<id> (~6 KB per 20 s), PSRAM
static const size_t SHOT_BUF_CAP = 64 * 1024;
// newest stored shot id seen while idle (frozen while a shot runs and while its lookup is pending): after the shot,
// any newer stored shot of the same profile is this one. If none appears while retrying, the machine did not store
// it (empty shots of 3.5 and 4.3 s were not stored, a 10.5 s one was): then the newest stored shot of
// the profile IS the previous one. Duration (within 1 s) is used only when the id before the shot is unknown.
static uint32_t g_before_id = 0;
static uint32_t g_prev_hash = 0, g_prev_dur10 = 0;
static int g_prev_tries = 0;          // > 0: a request is pending
static int g_prev_state = PREV_NONE;
static ShotTrace *g_prev = NULL;      // PSRAM
static uint32_t g_prev_seq = 0, g_prev_seen = 0;

// Wi-Fi credentials in use: the network saved from the menu (NVS "wifi") if any, else the compiled-in one (none while
// wifi_config.h still has the example's placeholder: the screen then asks for a network, "No Wi-Fi set up").
// "Forget this network" clears the saved one and sets "nobuiltin" so the compiled-in network is not used either (else
// it comes back after a restart); joining a network from the list clears the flag. g_ssid "" = none.
static char g_ssid[33], g_pass[65];
static bool g_saved_net = false, g_no_builtin = false;
static volatile bool g_forget_req = false;
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
  if (g_no_builtin || !strcmp(WIFI_SSID, "YOUR_WIFI_NAME")) { g_ssid[0] = 0; g_pass[0] = 0; return; }
  snprintf(g_ssid, sizeof g_ssid, "%s", WIFI_SSID);
  snprintf(g_pass, sizeof g_pass, "%s", WIFI_PASS);
}
static bool compiled_net(void) { return strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0; }   // a network in wifi_config.h

// scan / join requests from the UI task, results back to it (all under g_mtx)
static bool g_scan_req = false, g_scanning = false;
static WifiNet g_nets[MAX_NETS];
static int g_nets_n = 0;
static uint32_t g_nets_seq = 0, g_nets_seen = 0;
static bool g_join_req = false;
static char g_join_ssid[33], g_join_pass[65];
static int g_join_state = WIFI_JOIN_IDLE;
static uint32_t g_join_seq = 0, g_join_seen = 0;
static volatile uint8_t g_disc_reason = 0;
static int g_why = WHY_NONE;                       // offline reason (WHY_*), under g_mtx
static char g_why_ip[16] = "";                     // address that did not answer (WHY_NO_REPLY)
static void set_why(int why, IPAddress ip);

static void publish(const LiveData *d, int link) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (d) g_data = *d;
  g_link = link;
  g_seq++;
  xSemaphoreGive(g_mtx);
}

static void set_why(int why, IPAddress ip) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_why = why;
  snprintf(g_why_ip, sizeof g_why_ip, "%s", (uint32_t)ip ? ip.toString().c_str() : "");
  xSemaphoreGive(g_mtx);
}
static int wifi_why(uint8_t r) {                   // ESP-IDF wifi_err_reason_t
  if (r == 201 || (r >= 210 && r <= 212)) return WHY_WIFI_NO_AP;           // network not seen
  if (r == 2 || r == 15 || r == 202 || r == 204) return WHY_WIFI_AUTH;     // auth expired / handshake / auth fail
  return WHY_WIFI_OTHER;
}
int net_offline_reason(char *ip, size_t cap) {
  if (!g_mtx) { if (cap) ip[0] = 0; return WHY_NONE; }
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const int w = g_why;
  snprintf(ip, cap, "%s", g_why_ip);
  xSemaphoreGive(g_mtx);
  return w;
}

static void publish_profiles(const ProfileList *l) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_prof = *l;
  g_pseq++;
  xSemaphoreGive(g_mtx);
}

static void publish_shape(const ProfileShape *s) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (memcmp(&g_shape, s, sizeof *s) != 0) {        // only wake the UI when the graph really changes
    g_shape = *s;
    g_sseq++;
  }
  xSemaphoreGive(g_mtx);
}

// machine address: an IP ("192.168.1.50") or a name ("gaggiuino" / "gaggiuino.local", found by mDNS). Set from the
// menu (NVS "machine"/"host"), else MACHINE_IP / MACHINE_HOST from wifi_config.h.
static char g_mhost[64];
static IPAddress g_last_ip;            // last address that answered: kept when mDNS does not answer
static bool g_mhost_req = false;
static char g_mhost_new[64];
static bool g_wifi_retry_req = false;
static bool host_is_ip(const char *h) { IPAddress t; return t.fromString(h); }
static void load_mhost(void) {
  Preferences p;
  if (p.begin("machine", true)) {
    String h = p.getString("host", "");
    p.end();
    if (h.length() && h.length() < sizeof g_mhost) { snprintf(g_mhost, sizeof g_mhost, "%s", h.c_str()); return; }
  }
  if (strlen(MACHINE_IP) > 0) snprintf(g_mhost, sizeof g_mhost, "%s", MACHINE_IP);
  else snprintf(g_mhost, sizeof g_mhost, "%s.local", MACHINE_HOST);
}
static bool resolve(IPAddress *ip) {
  if (ip->fromString(g_mhost)) return true;
  char name[64];
  snprintf(name, sizeof name, "%s", g_mhost);
  char *dot = strstr(name, ".local");
  if (dot && dot[6] == 0) *dot = 0;               // mDNS wants the bare name
  IPAddress r = MDNS.queryHost(name, 2000);
  if ((uint32_t)r == 0) return false;
  *ip = r;
  return true;
}

// GET path; on success buf holds the body (NUL-terminated). A reply larger than cap is cut off (parsers detect it).
// post: false = GET, true = POST with an empty body. No other method can be sent (see the header comment).
static bool http(const IPAddress &ip, bool post, const char *path, char *buf, size_t cap) {
  WiFiClient c;
  c.setTimeout(2000);   // milliseconds (Stream::setTimeout), not seconds
  if (!c.connect(ip, 80, 2000)) return false;
  if (post) c.printf("POST %s HTTP/1.0\r\nHost: gaggiuino.local\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", path);
  else c.printf("GET %s HTTP/1.0\r\nHost: gaggiuino.local\r\nConnection: close\r\n\r\n", path);
  uint32_t t0 = millis();
  size_t n = 0, want = 0;                           // want: header + Content-Length, once the header is in
  // stop at Content-Length: the machine answers HTTP/1.1 and keeps the connection open after the body, so waiting for
  // the close always ran into the 3 s limit (every GET /api/shots/<id> took ~3.1 s instead of ~0.2 s)
  while ((c.connected() || c.available()) && millis() - t0 < 3000 && n + 1 < cap && (!want || n < want)) {
    int a = c.available();
    if (a <= 0) { delay(2); continue; }
    int r = c.read((uint8_t *)buf + n, min((size_t)a, cap - 1 - n));
    if (r <= 0) continue;
    n += r;
    if (!want) {
      buf[n] = 0;
      const char *he = strstr(buf, "\r\n\r\n");
      if (he) {
        const char *cl = strcasestr(buf, "\r\nContent-Length:");
        if (cl && cl < he) want = (size_t)(he + 4 - buf) + (size_t)strtoul(cl + 17, NULL, 10);
      }
    }
  }
  buf[n] = 0;
  c.stop();
  mem_watch(post ? "http POST" : "http GET", path);
  if (strncmp(buf, "HTTP/1.", 7) != 0 || strncmp(buf + 8, " 200", 4) != 0) return false;   // the status line only
  char *body = strstr(buf, "\r\n\r\n");
  if (!body) return false;
  memmove(buf, body + 4, strlen(body + 4) + 1);
  return buf[0] != 0;
}
static bool fetch(const IPAddress &ip, const char *path, char *buf, size_t cap) { return http(ip, false, path, buf, cap); }

static const char *selected_name(const ProfileList *l) {
  for (int i = 0; i < l->count; i++)
    if (l->p[i].selected) return l->p[i].name;
  return "";
}

// One profile step per poll cycle, so the 1 s status updates never stall for long.
static void profiles_step(const IPAddress &ip, char *buf, size_t cap, const LiveData *d) {
  static ProfileList cur, next;
  static bool have_list = false;
  static uint32_t last_list = 0;
  static uint8_t tries[MAX_PROFILES];
  static bool stale[MAX_PROFILES];                  // details to read again (saved from this screen)
  static int shape_id = 0;                          // profile id the published shape belongs to (0 = fetch again)
  static uint8_t shape_tries = 0;
  uint32_t now = millis();
  bool changed_on_machine = have_list && d->profile[0] && strcmp(d->profile, selected_name(&cur)) != 0;
  const bool forced = g_force_list;
  if (!have_list || forced || now - last_list > 30000 || (changed_on_machine && now - last_list > 5000)) {
    g_force_list = false;
    last_list = now;
    if (!fetch(ip, "/api/profiles/all", buf, cap) || parse_profile_list(buf, &next) == 0) return;
    for (int i = 0; i < next.count; i++)            // keep details we already have (same id and name)
      for (int j = 0; j < cur.count; j++)
        if (cur.p[j].detail && cur.p[j].id == next.p[i].id && strcmp(cur.p[j].name, next.p[i].name) == 0) {
          next.p[i].detail = true;
          next.p[i].phases = cur.p[j].phases;
          next.p[i].weight = cur.p[j].weight;
          next.p[i].temp = cur.p[j].temp;
        }
    cur = next;
    memset(tries, 0, sizeof tries);
    memset(stale, 0, sizeof stale);
    shape_id = 0;                                   // re-read the active profile too (it may have been edited)
    shape_tries = 0;
    have_list = true;
    Log.printf("profiles: %d\n", cur.count);
    publish_profiles(&cur);
    if (!forced) return;                            // after a select: fetch the new graph in this same cycle
  }
  for (int i = 0; i < cur.count; i++) {             // the active profile's phases come first
    if (!cur.p[i].selected || cur.p[i].id == shape_id || shape_tries >= 3) continue;
    shape_tries++;
    char path[32];
    snprintf(path, sizeof path, "/api/profile/%d", cur.p[i].id);
    static ProfileShape sh;                         // ~1.2 KB, off the task stack
    if (fetch(ip, path, buf, cap) && parse_profile_shape(buf, &sh)) {
      sh.id = cur.p[i].id;
      shape_id = sh.id;
      publish_shape(&sh);
      if (!cur.p[i].detail && parse_profile_detail(buf, &cur.p[i])) publish_profiles(&cur);
    }
    return;
  }
  if (g_redetail_id) {                              // keep showing the old values until the new ones are read
    for (int i = 0; i < cur.count; i++) if (cur.p[i].id == g_redetail_id) { stale[i] = true; tries[i] = 0; }
    g_redetail_id = 0;
  }
  for (int i = 0; i < cur.count; i++) {
    if ((cur.p[i].detail && !stale[i]) || tries[i] >= 3) continue;
    tries[i]++;
    char path[32];
    snprintf(path, sizeof path, "/api/profile/%d", cur.p[i].id);
    ProfileInfo pi = cur.p[i];
    if (fetch(ip, path, buf, cap) && parse_profile_detail(buf, &pi)) {
      cur.p[i] = pi;
      stale[i] = false;
      publish_profiles(&cur);
    }
    return;
  }
}

static void prev_publish(int state) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_prev_state = state;
  g_prev_seq++;
  xSemaphoreGive(g_mtx);
}

// After a shot: find the previous shot of the same profile and load it. The newest stored shot must be newer than
// g_before_id (the shot just shown); until the machine has stored it and re-sent the index, retry on the next cycles.
// If nothing new is stored by the last try, the machine did not keep the shot: take the newest of the profile.
static void prev_step(const IPAddress &ip, const LiveData *d) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const int tries = g_prev_tries;
  if (tries <= 0) { xSemaphoreGive(g_mtx); return; }   // nothing asked for: no copy of the index
  const uint32_t hash = g_prev_hash, dur = g_prev_dur10;
  const bool have_ix = g_index_ok;
  static WsShotIndex *ixp = NULL;
  if (!ixp) ixp = (WsShotIndex *)heap_caps_malloc(sizeof *ixp, MALLOC_CAP_SPIRAM);
  if (have_ix && ixp) *ixp = *g_index;
  xSemaphoreGive(g_mtx);
  if (d->brew_active || d->steam_active || d->hot_water_active) return;   // only while idle
  char *const buf = g_shot_buf;
  const size_t CAP = SHOT_BUF_CAP;
  if (!g_prev) g_prev = (ShotTrace *)heap_caps_malloc(sizeof(ShotTrace), MALLOC_CAP_SPIRAM);
  if (!buf || !g_prev || !ixp) { g_prev_tries = 0; prev_publish(PREV_FAILED); return; }
  const WsShotIndex &ix = *ixp;
  bool last_try = false;
  auto retry = [&](const char *why) {
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    const int left = --g_prev_tries;
    xSemaphoreGive(g_mtx);
    Log.printf("previous shot: %s%s\n", why, left > 0 ? ", retrying" : ", giving up");
    if (left <= 0) prev_publish(PREV_FAILED);
  };
  if (!have_ix) { retry("no shot index yet"); return; }
  int latest = 0;
  if (fetch(ip, "/api/shots/latest", buf, CAP)) latest = parse_shots_latest(buf);
  if (latest <= 0) { retry("GET /api/shots/latest failed"); return; }
  int at = -1;
  for (int i = 0; i < ix.n; i++) if ((int)ix.r[i].id == latest) { at = i; break; }
  const int dd = at < 0 ? 0 : (int)ix.r[at].dur10 - (int)dur;
  const bool newer = g_before_id ? (uint32_t)latest > g_before_id : (dd >= -10 && dd <= 10);
  int from = at + 1;                                // search older shots of this profile from here
  if (at < 0 || ix.r[at].name_hash != hash || !newer) {
    if (tries > 1 || !g_before_id || (uint32_t)latest != g_before_id) { retry("this shot not stored yet"); return; }
    last_try = true;                                // nothing new stored: the machine did not keep this shot
    from = 0;
    Log.println("previous shot: this shot was not stored by the machine, using the newest stored one");
  }
  if (!last_try && (dd < -10 || dd > 10))
    Log.printf("previous shot: stored shot %d is %u.%u s, the screen saw %u.%u s\n", latest,
                  (unsigned)(ix.r[at].dur10 / 10), (unsigned)(ix.r[at].dur10 % 10), (unsigned)(dur / 10), (unsigned)(dur % 10));
  int prev = -1;
  for (int i = from; i < ix.n; i++) if (ix.r[i].name_hash == hash) { prev = i; break; }
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_prev_tries = 0;
  xSemaphoreGive(g_mtx);
  if (prev < 0) { Log.printf("previous shot: none for this profile in the last %d\n", ix.n); prev_publish(PREV_MISSING); return; }
  char path[32];
  snprintf(path, sizeof path, "/api/shots/%u", (unsigned)ix.r[prev].id);
  const uint32_t t0 = millis();
  const bool ok = fetch(ip, path, buf, CAP) && parse_shot(buf, g_prev);
  Log.printf("previous shot %u: %s (%u points, %u ms)\n", (unsigned)ix.r[prev].id, ok ? "loaded" : "failed",
                ok ? (unsigned)g_prev->n : 0u, (unsigned)(millis() - t0));
  prev_publish(ok ? PREV_READY : PREV_FAILED);
}

// Shot history: the tapped shot (+ its profile, for the targets and the planned rest) and the next older stored shot of
// the same profile. Only while idle; one answer for both.
static void hist_publish(int st, int pst) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_hist_state = st;
  g_hist_prev_state = pst;
  g_hist_seq++;
  xSemaphoreGive(g_mtx);
}
static void hist_step(const IPAddress &ip, const LiveData *d) {
  static WsShotIndex *ixp = NULL;
  if (!ixp) ixp = (WsShotIndex *)heap_caps_malloc(sizeof *ixp, MALLOC_CAP_SPIRAM);
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const uint32_t id = g_hist_req;
  g_hist_req = 0;
  const bool have_ix = g_index_ok && ixp;
  if (have_ix) *ixp = *g_index;
  xSemaphoreGive(g_mtx);
  if (!id) return;
  if (d->brew_active || d->steam_active || d->hot_water_active) {
    Log.printf("history: shot %u not loaded, the machine is busy\n", (unsigned)id);
    hist_publish(PREV_FAILED, PREV_NONE);
    return;
  }
  if (!g_hist) g_hist = (ShotTrace *)heap_caps_malloc(sizeof(ShotTrace), MALLOC_CAP_SPIRAM);
  if (!g_hist_prev) g_hist_prev = (ShotTrace *)heap_caps_malloc(sizeof(ShotTrace), MALLOC_CAP_SPIRAM);
  if (!g_hist_shape) g_hist_shape = (ProfileShape *)heap_caps_malloc(sizeof(ProfileShape), MALLOC_CAP_SPIRAM);
  if (!g_shot_buf || !g_hist || !g_hist_prev || !g_hist_shape || !have_ix) { hist_publish(PREV_FAILED, PREV_NONE); return; }
  char path[32];
  snprintf(path, sizeof path, "/api/shots/%u", (unsigned)id);
  uint32_t t0 = millis();
  const bool ok = fetch(ip, path, g_shot_buf, SHOT_BUF_CAP) && parse_shot(g_shot_buf, g_hist);
  if (ok && !parse_profile_shape(g_shot_buf, g_hist_shape)) memset(g_hist_shape, 0, sizeof *g_hist_shape);
  Log.printf("history: shot %u %s (%u points, %u ms)\n", (unsigned)id, ok ? "loaded" : "failed",
                ok ? (unsigned)g_hist->n : 0u, (unsigned)(millis() - t0));
  if (!ok) { hist_publish(PREV_FAILED, PREV_NONE); return; }
  int at = -1, prev = -1;
  for (int i = 0; i < ixp->n; i++) if (ixp->r[i].id == id) { at = i; break; }
  if (at >= 0) for (int i = at + 1; i < ixp->n; i++) if (ixp->r[i].name_hash == ixp->r[at].name_hash) { prev = i; break; }
  if (prev < 0) { hist_publish(PREV_READY, PREV_MISSING); return; }
  snprintf(path, sizeof path, "/api/shots/%u", (unsigned)ixp->r[prev].id);
  t0 = millis();
  const bool pok = fetch(ip, path, g_shot_buf, SHOT_BUF_CAP) && parse_shot(g_shot_buf, g_hist_prev);
  Log.printf("history: its previous shot %u %s (%u ms)\n", (unsigned)ixp->r[prev].id, pok ? "loaded" : "failed",
                (unsigned)(millis() - t0));
  hist_publish(PREV_READY, pok ? PREV_READY : PREV_FAILED);
}

void net_profile_saved(int id) { g_redetail_id = id; }
bool net_stored_profile(int id, float *weight, float *temp) {
  static char *buf;                                 // PSRAM: profiles are 0.7-3 KB (internal RAM is for Wi-Fi)
  const size_t cap = 6144;
  if (!buf) buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  IPAddress ip;
  if (!buf || id <= 0 || !net_machine_ip(&ip)) return false;
  char path[32];
  snprintf(path, sizeof path, "/api/profile/%d", id);
  if (!http(ip, false, path, buf, cap)) return false;
  ProfileInfo pi;
  memset(&pi, 0, sizeof pi);
  if (!parse_profile_detail(buf, &pi)) return false;
  *weight = pi.weight;
  *temp = pi.temp;
  return true;
}

// Sends a queued profile select, re-checking on the status just read that the machine is idle in HOME.
static void select_step(const IPAddress &ip, char *buf, size_t cap, const LiveData *d) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const int id = g_select_req;
  const uint32_t at = g_select_ms;
  g_select_req = 0;
  xSemaphoreGive(g_mtx);
  if (id <= 0) return;
  if (millis() - at > 5000) { Log.printf("select %d dropped (request too old)\n", id); return; }
  const bool idle = !d->brew_active && !d->steam_active && !d->hot_water_active && strcmp(d->mode, "HOME") == 0;
  if (!idle) { Log.printf("select %d refused: machine busy (%s)\n", id, d->mode); return; }
  char path[40];
  snprintf(path, sizeof path, "/api/profile-select/%d", id);
  const bool ok = http(ip, true, path, buf, cap);
  Log.printf("select %d: %s\n", id, ok ? "OK" : "failed");
  g_force_list = true;                              // show the new selection (check mark, graph) right away
}

static void wifi_scan_step(void) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const bool req = g_scan_req;
  g_scan_req = false;
  xSemaphoreGive(g_mtx);
  if (req && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    if (WiFi.scanNetworks(true /* async */, false /* no hidden */) == WIFI_SCAN_FAILED) {
      Log.println("wifi: scan failed to start");
    } else {
      xSemaphoreTake(g_mtx, portMAX_DELAY);
      g_scanning = true;
      g_nets_seq++;
      xSemaphoreGive(g_mtx);
    }
  }
  if (!g_scanning) return;
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  static WifiNet nets[MAX_NETS];
  int k = 0;
  for (int i = 0; i < n; i++) {                   // one row per name (mesh: several APs), strongest kept
    String nm = WiFi.SSID(i);
    if (!nm.length() || nm.length() >= sizeof nets[0].ssid) continue;
    const int rssi = WiFi.RSSI(i);
    int j = 0;
    while (j < k && strcmp(nets[j].ssid, nm.c_str()) != 0) j++;
    if (j < k) { if (rssi > nets[j].rssi) nets[j].rssi = (int8_t)rssi; continue; }
    if (k == MAX_NETS) continue;
    snprintf(nets[k].ssid, sizeof nets[k].ssid, "%s", nm.c_str());
    nets[k].rssi = (int8_t)rssi;
    nets[k].lock = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    k++;
  }
  for (int a = 1; a < k; a++)                     // strongest first
    for (int b = a; b > 0 && nets[b].rssi > nets[b - 1].rssi; b--) { WifiNet t = nets[b]; nets[b] = nets[b - 1]; nets[b - 1] = t; }
  WiFi.scanDelete();
  Log.printf("wifi: scan found %d networks (%d names)\n", n < 0 ? 0 : n, k);
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  memcpy(g_nets, nets, sizeof nets);
  g_nets_n = k;
  g_scanning = false;
  g_nets_seq++;
  xSemaphoreGive(g_mtx);
}

static void join_publish(int st) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_join_state = st;
  g_join_seq++;
  xSemaphoreGive(g_mtx);
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
static volatile uint32_t g_last_shot_ms = 0;     // last shot snapshot (the review stays open up to 60 s after it)
static volatile bool g_roam_force = false;       // GET /roam on the screen (debug): scan now, move if better
static bool g_roam_scanning = false, g_pinned = false;
static uint32_t g_roam_last = 0, g_join_ms = 0, g_down_ms = 0;
static float g_rssi_avg = 0;

void net_roam_check(void) { g_roam_force = true; }

static void log_ap(const char *what) {
  Log.printf("wifi: %s AP %s, channel %d, %d dBm\n", what, WiFi.BSSIDstr().c_str(), (int)WiFi.channel(), (int)WiFi.RSSI());
}

// called every pass of the net task; idle = nothing running on the machine and no review / lookup / user scan
static void roam_step(bool idle) {
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (!g_down_ms) g_down_ms = stamp_nz(now);
    // down for 15 s: join again ourselves, every 15 s. The Arduino core's auto-reconnect skips some reasons: reason 8
    // (ASSOC_LEAVE, sent by access points being re-provisioned) left the screen offline for good.
    if (now - g_down_ms > 15000 && g_ssid[0]) {
      Log.printf("wifi: not connected for 15 s (last reason %u)%s, joining again\n", (unsigned)g_disc_reason,
                    g_pinned ? ", leaving the pinned AP" : "");
      g_pinned = false;
      WiFi.disconnect(false);
      delay(100);
      WiFi.begin(g_ssid, g_pass);
      g_down_ms = stamp_nz(now);
    }
    g_rssi_avg = 0;
    return;
  }
  if (g_down_ms || !g_join_ms) {                    // (re)joined
    g_down_ms = 0;
    g_join_ms = stamp_nz(now);
    log_ap("joined");
  }
  static uint32_t t_s;
  if (now - t_s >= 2000) {                          // smoothed signal, one sample per 2 s
    t_s = now;
    const int r = WiFi.RSSI();
    if (r < 0) g_rssi_avg = g_rssi_avg == 0 ? r : g_rssi_avg * 0.9f + r * 0.1f;
  }
  if (!g_roam_scanning) {
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
    if (WiFi.scanNetworks(true, false, false, 120) == WIFI_SCAN_FAILED) { Log.println("wifi: roaming scan failed to start"); return; }
    g_roam_scanning = true;
    Log.printf("wifi: roaming scan (%s, average %.0f dBm on %s)\n", force ? "asked" : "weak signal", g_rssi_avg, WiFi.BSSIDstr().c_str());
    return;
  }
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
    Log.printf("wifi:   %s channel %d %d dBm%s\n", WiFi.BSSIDstr(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i), same ? " (current)" : "");
    if (same) cur_rssi = WiFi.RSSI(i);
    else if (WiFi.RSSI(i) > best_rssi) { best = i; best_rssi = WiFi.RSSI(i); }
  }
  if (best >= 0 && best_rssi >= cur_rssi + ROAM_GAIN_DB) {
    uint8_t bssid[6];
    memcpy(bssid, WiFi.BSSID(best), 6);
    const int ch = WiFi.channel(best);
    Log.printf("wifi: moving to AP %s, channel %d (%d dBm, now %d)\n", WiFi.BSSIDstr(best).c_str(), ch, best_rssi, cur_rssi);
    WiFi.scanDelete();
    WiFi.disconnect(false);
    delay(100);
    WiFi.begin(g_ssid, g_pass, ch, bssid);
    g_pinned = true;
    g_down_ms = stamp_nz(millis());
  } else {
    Log.printf("wifi: staying (best other AP %d dBm, this one %d)\n", best_rssi, cur_rssi);
    WiFi.scanDelete();
  }
}

// a join request: try the new network for up to 20 s (faster "failed" on an auth error), keep it on success,
// else go back to the network we had. Returns true when it ran (the machine is looked up again then).
static bool wifi_join_step(void) {
  static char ssid[33], pass[65];
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const bool req = g_join_req;
  if (req) {
    memcpy(ssid, g_join_ssid, sizeof ssid);
    memcpy(pass, g_join_pass, sizeof pass);
    memset(g_join_pass, 0, sizeof g_join_pass);
    g_join_req = false;
  }
  xSemaphoreGive(g_mtx);
  if (!req) return false;
  g_pinned = false;                                  // a new network: any of its APs
  join_publish(WIFI_JOIN_BUSY);
  Log.printf("wifi: joining \"%s\" from the menu\n", ssid);
  WiFi.disconnect(false);
  delay(200);
  g_disc_reason = 0;
  WiFi.begin(ssid, pass);
  const uint32_t t0 = millis();
  bool ok = false;
  while (millis() - t0 < 20000) {
    if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
    const uint8_t r = g_disc_reason;               // 15 4-way handshake timeout, 202 auth fail, 204 handshake timeout
    if (millis() - t0 > 4000 && (r == 15 || r == 202 || r == 204)) break;
    delay(200);
  }
  g_down_ms = 0;                                     // the 15 s rejoin timer starts again from here
  if (ok) {
    Preferences p;
    if (p.begin("wifi", false)) { p.putString("ssid", ssid); p.putString("pass", pass); p.remove("nobuiltin"); p.end(); }
    g_no_builtin = false;
    snprintf(g_ssid, sizeof g_ssid, "%s", ssid);
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
  join_publish(ok ? WIFI_JOIN_OK : WIFI_JOIN_FAILED);
  return true;
}

void net_set_machine(const char *host) {
  if (!g_mtx || !host[0]) return;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  snprintf(g_mhost_new, sizeof g_mhost_new, "%s", host);
  g_mhost_req = true;
  xSemaphoreGive(g_mtx);
  if (g_task) xTaskNotifyGive(g_task);
}
void net_get_machine(char *out, size_t cap) {
  if (!g_mtx) { snprintf(out, cap, "%s", ""); return; }
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  snprintf(out, cap, "%s", g_mhost);
  xSemaphoreGive(g_mtx);
}
void net_wifi_forget(void) {
  g_forget_req = true;
  if (g_task) xTaskNotifyGive(g_task);
}
void net_wifi_retry(void) {
  if (!g_mtx) return;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_wifi_retry_req = true;
  xSemaphoreGive(g_mtx);
}
const char *net_wifi_target(void) { return g_ssid; }
void net_wifi_scan(void) {
  if (!g_mtx) return;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_scan_req = true;
  xSemaphoreGive(g_mtx);
}
bool net_get_wifi_scan(WifiNet *out, int *n, bool *scanning) {
  if (!g_mtx) return false;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const bool ch = g_nets_seq != g_nets_seen;
  g_nets_seen = g_nets_seq;
  memcpy(out, g_nets, sizeof g_nets);
  *n = g_nets_n;
  *scanning = g_scanning || g_scan_req;
  xSemaphoreGive(g_mtx);
  return ch;
}
void net_wifi_join(const char *ssid, const char *pass) {
  if (!g_mtx) return;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  snprintf(g_join_ssid, sizeof g_join_ssid, "%s", ssid);
  snprintf(g_join_pass, sizeof g_join_pass, "%s", pass);
  g_join_req = true;
  g_join_state = WIFI_JOIN_BUSY;
  g_join_seq++;
  xSemaphoreGive(g_mtx);
  if (g_task) xTaskNotifyGive(g_task);
}
bool net_get_wifi_join(int *state) {
  if (!g_mtx) return false;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const bool ch = g_join_seq != g_join_seen;
  g_join_seen = g_join_seq;
  *state = g_join_state;
  xSemaphoreGive(g_mtx);
  return ch;
}

static void task(void *) {
  static char buf[8192];   // a full profile is ~0.7-3 KB, the status ~1 KB
  static LiveData d;
  IPAddress ip;
  bool have_ip = false;
  int fails = 0;
  if (g_ssid[0]) WiFi.begin(g_ssid, g_pass);
  if (g_ssid[0]) Log.printf("wifi: joining \"%s\" (%s)\n", g_ssid, g_saved_net ? "saved on the screen" : "built in");
  else Log.println("wifi: no network set up: choose one on the screen");
  uint32_t boot = millis();
  bool fell_back = false;
  for (;;) {
    wifi_scan_step();
    if (wifi_join_step()) { have_ip = false; fails = 0; continue; }
    {
      xSemaphoreTake(g_mtx, portMAX_DELAY);
      const bool hreq = g_mhost_req, wreq = g_wifi_retry_req;
      char nh[64];
      memcpy(nh, g_mhost_new, sizeof nh);
      g_mhost_req = g_wifi_retry_req = false;
      xSemaphoreGive(g_mtx);
      if (hreq) {
        Preferences p;
        if (p.begin("machine", false)) { p.putString("host", nh); p.end(); }
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        snprintf(g_mhost, sizeof g_mhost, "%s", nh);
        g_have_ip = false;
        xSemaphoreGive(g_mtx);
        g_last_ip = IPAddress();
        have_ip = false;
        fails = 0;
        Log.printf("net: machine address set to \"%s\" (saved)\n", nh);
        publish(NULL, LINK_NO_MACHINE);
      }
      if (g_forget_req) {
        g_forget_req = false;
        Preferences p;
        if (p.begin("wifi", false)) { p.remove("ssid"); p.remove("pass"); p.putBool("nobuiltin", true); p.end(); }
        Log.printf("wifi: forgot \"%s\" (saved password deleted, built-in network off)\n", g_ssid);
        g_ssid[0] = 0;
        memset(g_pass, 0, sizeof g_pass);
        g_saved_net = false;
        g_no_builtin = true;
        WiFi.disconnect(false);
        have_ip = false;
      }
      if (wreq && g_ssid[0] && WiFi.status() != WL_CONNECTED) {
        Log.printf("wifi: trying \"%s\" again (from the screen)\n", g_ssid);
        WiFi.disconnect(false);
        delay(200);
        g_pinned = false;
        WiFi.begin(g_ssid, g_pass);
        g_down_ms = 0;
      }
    }
    {
      static bool rest_idle = true;                 // from the last status reply
      xSemaphoreTake(g_mtx, portMAX_DELAY);
      const bool idle = rest_idle && g_prev_tries <= 0 && millis() - g_last_shot_ms > 90000 && !g_join_req;
      xSemaphoreGive(g_mtx);
      roam_step(idle);
      rest_idle = !d.brew_active && !d.steam_active && !d.hot_water_active;
    }
    // saved network not found within 30 s after boot: try the built-in one (once). Only if it was never joined:
    // a later drop (or a roaming move) is not a reason to switch networks
    static bool joined_once = false;
    if (WiFi.status() == WL_CONNECTED) joined_once = true;
    if (g_saved_net && !g_no_builtin && !fell_back && !joined_once && compiled_net() && WiFi.status() != WL_CONNECTED &&
        millis() - boot > 30000) {
      fell_back = true;
      Log.printf("wifi: saved network not joined, trying the built-in \"%s\"\n", WIFI_SSID);
      snprintf(g_ssid, sizeof g_ssid, "%s", WIFI_SSID);
      snprintf(g_pass, sizeof g_pass, "%s", WIFI_PASS);
      WiFi.disconnect(false);
      WiFi.begin(g_ssid, g_pass);
    }
    if (WiFi.status() != WL_CONNECTED) {
      set_why(wifi_why(g_disc_reason), IPAddress());
      publish(NULL, LINK_CONNECTING);
      have_ip = false;
      delay(500);
      continue;
    }
    if (!have_ip) {
      static bool mdns_up = false;
      if (!mdns_up) mdns_up = MDNS.begin("brewdeck");
      have_ip = resolve(&ip);
      if (!have_ip && (uint32_t)g_last_ip != 0) {
        Log.printf("net: mDNS no answer, keeping %s\n", g_last_ip.toString().c_str());
        ip = g_last_ip;
        have_ip = true;
      }
      if (have_ip) {
        g_last_ip = ip;
        Log.printf("machine at %s\n", ip.toString().c_str());
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        g_ip = ip;
        g_have_ip = true;
        xSemaphoreGive(g_mtx);
      }
      else {
        static uint32_t t_log;
        if (!t_log || millis() - t_log > 30000) {    // was silent: the log looked as if the task had stopped
          t_log = stamp_nz(millis());
          Log.printf("net: \"%s\" not found (mDNS), retrying\n", g_mhost);
        }
        set_why(WHY_NAME_UNKNOWN, IPAddress()); publish(NULL, LINK_NO_MACHINE); delay(1500); continue;
      }
    }
    // a shot-history tap first (it woke the task): judged on the last status (< ~1 s old, it must have succeeded),
    // so the shot is not delayed by this status request; the call after the status below covers the other case
    if (fails == 0 && d.mode[0]) hist_step(ip, &d);
    if (fetch(ip, "/api/system/status", buf, sizeof buf) && strchr(buf, '{')) {
      parse_status(buf, &d);
      set_why(WHY_NONE, IPAddress());
      publish(&d, LINK_ONLINE);
      fails = 0;
      select_step(ip, buf, sizeof buf, &d);
      {   // remember the newest stored shot while idle and no lookup is pending (see g_before_id)
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        if (!d.brew_active && g_prev_tries <= 0 && g_index_ok && g_index->n > 0) g_before_id = g_index->r[0].id;
        xSemaphoreGive(g_mtx);
      }
      prev_step(ip, &d);
      hist_step(ip, &d);
      profiles_step(ip, buf, sizeof buf, &d);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(900));   // ~1 s poll; a profile tap wakes it at once
    } else {
      ++fails;
      Log.printf("net: status request %d failed (rssi %d)\n", fails, WiFi.RSSI());
      if (fails >= 3) {
        Log.println("net: machine not answering, looking it up again");
        set_why(WHY_NO_REPLY, ip);
        publish(NULL, LINK_NO_MACHINE);
        have_ip = host_is_ip(g_mhost);
      }
      delay(700);
    }
  }
}

void net_begin(void) {
  g_mtx = xSemaphoreCreateMutex();
  g_index = (WsShotIndex *)heap_caps_calloc(1, sizeof(WsShotIndex), MALLOC_CAP_SPIRAM);
  g_shot_buf = (char *)heap_caps_malloc(SHOT_BUF_CAP, MALLOC_CAP_SPIRAM);
  memset(&g_data, 0, sizeof g_data);
  memset(&g_prof, 0, sizeof g_prof);
  memset(&g_shape, 0, sizeof g_shape);
  load_creds();
  load_mhost();
  // Must run BEFORE the RGB panel starts: Wi-Fi start touches NVS flash, and on a stock core (no code in PSRAM)
  // that crashes the panel's bounce-buffer ISR ("Cache disabled but cached memory region accessed").
  // persistent(false) stops WiFi.begin writing NVS (a flash write while the panel interrupt reads PSRAM crashed it).
  WiFi.persistent(false);
  WiFi.setHostname("brewdeck");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // no modem sleep: its beacon wake-ups add bursts of flash/PSRAM traffic that disturb the panel
  // local time for the idle screen's clock (NTP from the internet, nothing to/from the machine). Time zone: set
  // GAGGI_TZ in wifi_config.h to override (POSIX TZ string); default Central Europe with summer time.
#ifndef GAGGI_TZ
#define GAGGI_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
  configTzTime(GAGGI_TZ, "pool.ntp.org", "time.google.com");
  // several access points can share a network name: by default the ESP32 joins the FIRST one it finds (seen: -64 to
  // -70 dBm right next to an access point, with stalls); scan every channel and take the strongest instead
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {   // debug: why the link dropped
    g_disc_reason = info.wifi_sta_disconnected.reason;
    Log.printf("wifi: disconnected, reason %u\n", (unsigned)info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  xTaskCreatePinnedToCore(task, "net", 6144, NULL, 1, &g_task, 0);   // measured peak ~2.6 KB (GET /tasks)
}

static float g_act_w, g_act_t;          // active profile (d_act_prof): stop weight, brew temperature, name
static char g_act_name[48];
static char g_front_ver[28], g_static_ver[28];   // pb_versions: kept across reconnects
static uint32_t g_steam_set = 0;   // steam set point (pb_steam_set), 0 = not known yet
static WsScaleCfg g_scfg;   // the 3 scale flags only (see pb_scale_cfg; set by the WS task); kept across reconnects

void net_addr_info(char *screen, size_t scap, char *machine, size_t mcap) {
  if (WiFi.status() == WL_CONNECTED) snprintf(screen, scap, "%s", WiFi.localIP().toString().c_str());
  else snprintf(screen, scap, "%s", "");
  IPAddress ip;
  if (net_machine_ip(&ip)) snprintf(machine, mcap, "%s", ip.toString().c_str());
  else snprintf(machine, mcap, "%s", g_mhost);
}

bool net_get(LiveData *out, int *link) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  *out = g_data;
  *link = g_link;
  const uint32_t now = millis();
  // REST requests can time out while the machine is busy (seen at the start of a shot): fresh WebSocket data
  // alone keeps the machine "online" (with the last REST values for the rest)
  if (g_link != LINK_ONLINE && g_wss_ok && now - g_wss_ms < WS_FRESH_MS && g_data.mode[0]) *link = LINK_ONLINE;
  if (*link == LINK_ONLINE && g_wss_ok && now - g_wss_ms < WS_FRESH_MS) {   // ~3/s instead of ~1/s
    out->temperature = g_wss.temperature;
    out->pressure = g_wss.pressure;
    out->weight = g_wss.weight;
    out->water_level = g_wss.water_level;
    out->brew_active = g_wss.brew_active;
    out->steam_active = g_wss.steam_active;
    out->hot_water_active = g_wss.hot_water;
  }
  if (*link == LINK_ONLINE && g_wsy_ok && now - g_wsy_ms < WS_FRESH_MS) {
    out->scales_present = g_wsy.scales_present;
    out->warmup_remaining = (int)g_wsy.warmup_remaining;
    out->readiness = (int)g_wsy.readiness;
    const char *m = ws_mode_name(g_wsy.mode);
    if (m[0]) snprintf(out->mode, sizeof out->mode, "%s", m);
  }
  out->act_weight = g_act_w;
  out->act_temp = g_act_t;
  snprintf(out->act_profile, sizeof out->act_profile, "%s", g_act_name);
  out->steam_set = (int)g_steam_set;
  snprintf(out->front_ver, sizeof out->front_ver, "%s", g_front_ver);
  snprintf(out->static_ver, sizeof out->static_ver, "%s", g_static_ver);
  if (!g_scfg.valid) out->scale_type = SCALE_UNKNOWN;
  else if (g_scfg.force_predictive || (!g_scfg.bt_enabled && !g_scfg.hw_enabled)) out->scale_type = SCALE_PREDICTIVE;
  else if (g_scfg.bt_enabled && !g_scfg.hw_enabled) out->scale_type = SCALE_BT;
  else if (g_scfg.hw_enabled && !g_scfg.bt_enabled) out->scale_type = SCALE_HW;
  else out->scale_type = SCALE_UNKNOWN;             // both enabled: which one is connected is not reported
  {   // the machine's readiness: log every change with the temperatures (to see when it switches)
    static int last_rd = -2;
    if (*link == LINK_ONLINE && out->readiness != last_rd) {
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
    const bool idle = *link == LINK_ONLINE && !strcmp(out->mode, "HOME") && !out->brew_active && !out->steam_active &&
                      !out->hot_water_active;
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
                  *link >= 0 && *link < 3 ? LN[*link] : "?", g_link == LINK_ONLINE ? "ok" : "failing",
                  g_wss_ok ? (long)(now - g_wss_ms) : -1L);
    shown = *link;
  }
  g_seen = g_seq;
  xSemaphoreGive(g_mtx);
  return changed;
}

bool net_get_profiles(ProfileList *out) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  bool changed = g_pseq != g_pseen;
  if (changed) *out = g_prof;
  g_pseen = g_pseq;
  xSemaphoreGive(g_mtx);
  return changed;
}

bool net_get_shape(ProfileShape *out) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  bool changed = g_sseq != g_sseen;
  if (changed) *out = g_shape;
  g_sseen = g_sseq;
  xSemaphoreGive(g_mtx);
  return changed;
}

void net_select_profile(int id) {
  if (id <= 0) return;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_select_req = id;
  g_select_ms = millis();
  xSemaphoreGive(g_mtx);
  if (g_task) xTaskNotifyGive(g_task);
}

// ---- WebSocket hand-over (net_ws.h), called from the ws task ----
bool net_machine_ip(IPAddress *ip) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const bool ok = g_have_ip;
  if (ok) *ip = g_ip;
  xSemaphoreGive(g_mtx);
  return ok;
}
void net_ws_sensor(const WsSensor *s) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_wss = *s;
  g_wss_ms = millis();
  g_wss_ok = true;
  g_seq++;
  xSemaphoreGive(g_mtx);
}
void net_ws_system(const WsSystem *s) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_wsy = *s;
  g_wsy_ms = millis();
  g_wsy_ok = true;
  g_seq++;
  xSemaphoreGive(g_mtx);
}
void net_ws_shot(const WsShot *s) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_shotq[g_shot_w++ % 32] = *s;
  g_last_shot_ms = millis();
  if (g_shot_w - g_shot_r > 32) g_shot_r = g_shot_w - 32;   // UI stalled: drop the oldest
  xSemaphoreGive(g_mtx);
}
void net_ws_act_profile(const char *name, float weight, float temp) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (g_act_w != weight || g_act_t != temp || strcmp(g_act_name, name) != 0) g_seq++;
  g_act_w = weight;
  g_act_t = temp;
  snprintf(g_act_name, sizeof g_act_name, "%s", name);
  xSemaphoreGive(g_mtx);
}
void net_ws_versions(const char *front, const char *stat) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (strcmp(g_front_ver, front) || strcmp(g_static_ver, stat)) g_seq++;
  snprintf(g_front_ver, sizeof g_front_ver, "%s", front);
  snprintf(g_static_ver, sizeof g_static_ver, "%s", stat);
  xSemaphoreGive(g_mtx);
}
void net_ws_steam_set(uint32_t deg) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (g_steam_set != deg) g_seq++;
  g_steam_set = deg;
  xSemaphoreGive(g_mtx);
}
void net_ws_scale_cfg(const WsScaleCfg *c) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_scfg = *c;
  g_seq++;
  xSemaphoreGive(g_mtx);
}
static WsNotif g_notif;
static uint32_t g_notif_seq = 0, g_notif_seen = 0;
void net_ws_notif(const WsNotif *n) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_notif = *n;
  g_notif_seq++;
  xSemaphoreGive(g_mtx);
}
bool net_get_notif(WsNotif *out) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const bool ch = g_notif_seq != g_notif_seen;
  if (ch) { *out = g_notif; g_notif_seen = g_notif_seq; }
  xSemaphoreGive(g_mtx);
  return ch;
}
void net_ws_lost(void) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_wss_ok = g_wsy_ok = false;
  g_seq++;
  xSemaphoreGive(g_mtx);
}

void net_ws_index(const WsShotIndex *ix) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  if (g_index) { *g_index = *ix; g_index_ok = true; g_index_seq++; }
  xSemaphoreGive(g_mtx);
}

void net_request_prev(const char *profile, int dur10) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_prev_hash = shot_name_hash(profile, strlen(profile));
  g_prev_dur10 = dur10 < 0 ? 0 : (uint32_t)dur10;
  g_prev_tries = 8;                   // ~8 s for the machine to store the shot
  g_prev_state = PREV_LOADING;
  g_prev_seq++;
  xSemaphoreGive(g_mtx);
  if (g_task) xTaskNotifyGive(g_task);
}

bool net_get_index(WsShotIndex *out) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const bool ch = g_index_ok && g_index_seq != g_index_seen;
  if (ch) { *out = *g_index; g_index_seen = g_index_seq; }
  xSemaphoreGive(g_mtx);
  return ch;
}

void net_request_hist(uint32_t id) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_hist_req = id;
  g_hist_state = PREV_LOADING;
  xSemaphoreGive(g_mtx);
  if (g_task) xTaskNotifyGive(g_task);
}

int net_get_hist(ShotTrace *shot, ProfileShape *shape, ShotTrace *prev, int *prev_state, bool *changed) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const int st = g_hist_state;
  *changed = g_hist_seq != g_hist_seen;
  *prev_state = g_hist_prev_state;
  if (*changed && st == PREV_READY) {
    memcpy(shot, g_hist, sizeof *shot);
    memcpy(shape, g_hist_shape, sizeof *shape);
    if (g_hist_prev_state == PREV_READY) memcpy(prev, g_hist_prev, sizeof *prev);
  }
  g_hist_seen = g_hist_seq;
  xSemaphoreGive(g_mtx);
  return st;
}

int net_get_prev(ShotTrace *out, bool *changed) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const int st = g_prev_state;
  *changed = g_prev_seq != g_prev_seen;
  if (*changed && st == PREV_READY && g_prev && out) memcpy(out, g_prev, sizeof *out);
  g_prev_seen = g_prev_seq;
  xSemaphoreGive(g_mtx);
  return st;
}

bool net_get_shot(WsShot *out) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  const bool any = g_shot_r != g_shot_w;
  if (any) *out = g_shotq[g_shot_r++ % 32];
  xSemaphoreGive(g_mtx);
  return any;
}
