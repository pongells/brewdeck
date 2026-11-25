// The network task: finds the machine and polls the Gaggiuino REST API (live values also arrive ~3/s over WebSocket, see ws.cpp):
//   /api/system/status   about once a second
//   /api/profiles/all    on first contact, every 30 s, and soon after the active profile changes
//   /api/profile/<id>    once per profile, for the summary line (phases, stop weight, temperature), and the
//                        active profile again whenever the list is refreshed (preview graph + target weight)
// One command: POST /api/profile-select/<id> when a profile is tapped, sent only
// while the machine is idle in HOME. Same effect as a switch in the web UI: active now, the boot profile is not
// changed. DANGER: DELETE on the same URL deletes the profile, so http() can only emit GET or POST.
// The screen's own Wi-Fi is wifi.cpp, what is handed to the core loop net_data.cpp (both called from this task).
#include "stamp.h"
#include <Arduino.h>
#include "tlog.h"
#include <esp_heap_caps.h>
#include <mdns.h>
#include <string.h>
#include <stdlib.h>
#include "wifi_config.h"
#include "net.h"
#include "board_core.h"
#include "net_ws.h"
#include "parse.h"
#include "net_int.h"
#include "kv.h"
#include "tcp.h"

static int g_select_req = 0;          // profile id to select (0 = none), set by the UI task
static uint32_t g_select_ms = 0;      // when it was asked for: dropped after 5 s (the core answers
                                      // NOT_TAKEN then; a select queued during a shot would fire after it)
static bool g_force_list = false;     // re-read the profile list on the next cycle (after a select)
static volatile int g_redetail_id = 0; // a profile saved from this screen: re-read its stored values (net_profile_saved)
static TaskHandle_t g_task = NULL;
static uint32_t g_ip;                 // machine address for the WebSocket task (network order)
static bool g_have_ip = false;
// previous shot for the shot review (GET /api/shots/latest + /api/shots/<id>, sent only after a
// shot while the machine is idle)
// Shot history (menu > Shot history): on a tap, GET /api/shots/<id> of that shot and of the next
// older stored shot of the same profile (its "vs Previous shot"), only while the machine is idle. PSRAM.
static uint32_t g_hist_req = 0;       // id asked for, 0 = none
static int g_hist_state = PREV_NONE, g_hist_prev_state = PREV_NONE;
static bd_shot *g_hist = NULL, *g_hist_prev = NULL;
static bd_profile_shape *g_hist_shape = NULL;
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
static uint32_t g_prev_asked_ms = 0;  // when it was asked (dropped after 2 min)
static int g_prev_state = PREV_NONE;
static bd_shot *g_prev = NULL;      // PSRAM
static uint32_t g_prev_seq = 0, g_prev_seen = 0;

// machine address: an IP ("192.168.1.50") or a name ("gaggiuino" / "gaggiuino.local", found by mDNS). Set from the
// menu (NVS "machine"/"host"), else MACHINE_IP / MACHINE_HOST from wifi_config.h.
static char g_mhost[64];
static uint32_t g_last_ip;             // last address that answered: kept when mDNS does not answer (0 = none)
static bool g_mhost_req = false;
static char g_mhost_new[64];
static bool host_is_ip(const char *h) { uint32_t t; return ip_parse(h, &t); }
static void load_mhost(void) {
  if (kv_get_str("machine", "host", g_mhost, sizeof g_mhost) && g_mhost[0]) return;
  if (strlen(MACHINE_IP) > 0) snprintf(g_mhost, sizeof g_mhost, "%s", MACHINE_IP);
  else snprintf(g_mhost, sizeof g_mhost, "%s.local", MACHINE_HOST);
}
static bool resolve(uint32_t *ip) {
  if (ip_parse(g_mhost, ip)) return true;
  char name[64];
  snprintf(name, sizeof name, "%s", g_mhost);
  char *dot = strstr(name, ".local");
  if (dot && dot[6] == 0) *dot = 0;               // mDNS wants the bare name
  esp_ip4_addr_t a = {};
  if (mdns_query_a(name, 2000, &a) != ESP_OK || !a.addr) return false;   // ESP_ERR_NOT_FOUND: no answer in 2 s
  *ip = a.addr;                                     // network order
  return true;
}

// GET path; on success buf holds the body (NUL-terminated). A reply larger than cap is cut off (parsers detect it).
// post: false = GET, true = POST with an empty body. No other method can be sent (see the header comment).
static bool http(uint32_t ip, bool post, const char *path, char *buf, size_t cap) {
  TcpConn c;
  if (!c.connect(ip, 80, 2000)) return false;
  c.set_timeout(200);                               // each wait; the whole reply gets 3 s (below)
  if (post) c.printf("POST %s HTTP/1.0\r\nHost: gaggiuino.local\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", path);
  else c.printf("GET %s HTTP/1.0\r\nHost: gaggiuino.local\r\nConnection: close\r\n\r\n", path);
  uint32_t t0 = millis();
  size_t n = 0, want = 0;                           // want: header + Content-Length, once the header is in
  // stop at Content-Length: the machine answers HTTP/1.1 and keeps the connection open after the body, so waiting for
  // the close always ran into the 3 s limit (every GET /api/shots/<id> took ~3.1 s instead of ~0.2 s)
  while (millis() - t0 < 3000 && n + 1 < cap && (!want || n < want)) {
    const size_t r = c.read_some(buf + n, cap - 1 - n);
    if (!r) {
      if (!c.connected()) break;                    // closed by the machine (or failed)
      continue;                                     // nothing within 200 ms: wait again
    }
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
  c.close();
  mem_watch(post ? "http POST" : "http GET", path);
  if (strncmp(buf, "HTTP/1.", 7) != 0 || strncmp(buf + 8, " 200", 4) != 0) return false;   // the status line only
  char *body = strstr(buf, "\r\n\r\n");
  if (!body) return false;
  memmove(buf, body + 4, strlen(body + 4) + 1);
  return buf[0] != 0;
}
static bool fetch(uint32_t ip, const char *path, char *buf, size_t cap) { return http(ip, false, path, buf, cap); }

static const char *selected_name(const ProfileList *l) {
  for (int i = 0; i < l->count; i++)
    if (l->p[i].selected) return l->p[i].name;
  return "";
}

// One profile step per poll cycle, so the 1 s status updates never stall for long.
static void profiles_step(uint32_t ip, char *buf, size_t cap, const LiveData *d) {
  static ProfileList cur, next;
  static bool have_list = false;
  static uint32_t last_list = 0;
  static uint8_t tries[BD_MAX_PROFILES];
  static bool stale[BD_MAX_PROFILES];                  // details to read again (saved from this screen)
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
    int stale_id[BD_MAX_PROFILES], n_stale = 0;     // a re-read after a save that has not worked yet survives this
    for (int i = 0; i < cur.count; i++) if (stale[i]) stale_id[n_stale++] = cur.p[i].id;
    cur = next;
    memset(tries, 0, sizeof tries);
    memset(stale, 0, sizeof stale);
    for (int k = 0; k < n_stale; k++)
      for (int i = 0; i < cur.count; i++) if (cur.p[i].id == stale_id[k]) stale[i] = true;
    shape_id = 0;                                   // re-read the active profile too (it may have been edited)
    shape_tries = 0;
    have_list = true;
    Log.printf("profiles: %d\n", cur.count);
    data_publish_profiles(&cur);
    if (!forced) return;                            // after a select: fetch the new graph in this same cycle
  }
  for (int i = 0; i < cur.count; i++) {             // the active profile's phases come first
    if (!cur.p[i].selected || cur.p[i].id == shape_id || shape_tries >= 3) continue;
    shape_tries++;
    char path[32];
    snprintf(path, sizeof path, "/api/profile/%d", cur.p[i].id);
    static bd_profile_shape sh;                         // ~1.2 KB, off the task stack
    if (fetch(ip, path, buf, cap) && parse_profile_shape(buf, &sh)) {
      sh.id = cur.p[i].id;
      shape_id = sh.id;
      data_publish_shape(&sh);
      if (!cur.p[i].detail && parse_profile_detail(buf, &cur.p[i])) data_publish_profiles(&cur);
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
      data_publish_profiles(&cur);
    }
    return;
  }
}

static void prev_publish(int state) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_prev_state = state;
  g_prev_seq++;
  xSemaphoreGive(g_net_mtx);
}

// After a shot: find the previous shot of the same profile and load it. The newest stored shot must be newer than
// g_before_id (the shot just shown); until the machine has stored it and re-sent the index, retry on the next cycles.
// If nothing new is stored by the last try, the machine did not keep the shot: take the newest of the profile.
// A request that could not run for 2 min (machine busy or off) is dropped.
static void prev_step(uint32_t ip, const LiveData *d) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const int tries = g_prev_tries;
  if (tries <= 0) { xSemaphoreGive(g_net_mtx); return; }   // nothing asked for: no copy of the index
  if (millis() - g_prev_asked_ms > 120000) {
    g_prev_tries = 0;
    xSemaphoreGive(g_net_mtx);
    Log.println("previous shot: not found within 2 min, dropped");
    prev_publish(PREV_FAILED);
    return;
  }
  const uint32_t hash = g_prev_hash, dur = g_prev_dur10;
  xSemaphoreGive(g_net_mtx);
  static WsShotIndex *ixp = NULL;
  if (!ixp) ixp = (WsShotIndex *)heap_caps_malloc(sizeof *ixp, MALLOC_CAP_SPIRAM);
  const bool have_ix = data_index(ixp);
  if (d->brew_active || d->steam_active || d->hot_water_active) return;   // only while idle
  char *const buf = g_shot_buf;
  const size_t CAP = SHOT_BUF_CAP;
  if (!g_prev) g_prev = (bd_shot *)heap_caps_malloc(sizeof(bd_shot), MALLOC_CAP_SPIRAM);
  if (!buf || !g_prev || !ixp) {
    xSemaphoreTake(g_net_mtx, portMAX_DELAY);
    g_prev_tries = 0;
    xSemaphoreGive(g_net_mtx);
    prev_publish(PREV_FAILED);
    return;
  }
  const WsShotIndex &ix = *ixp;
  bool last_try = false;
  auto retry = [&](const char *why) {
    xSemaphoreTake(g_net_mtx, portMAX_DELAY);
    const int left = --g_prev_tries;
    xSemaphoreGive(g_net_mtx);
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
  // stored (the REST id is newer) but not in the index yet: usually the index push is on its way, so wait for it; by
  // the last try it was missed (a WebSocket gap) and the machine does not send it again. Every shot in the index is
  // older then, so its newest of the profile is the previous one (wrong only if more than one push was missed).
  const bool ix_stale = at < 0 && g_before_id && (uint32_t)latest > g_before_id;
  if (ix_stale && tries > 1) { retry("this shot is stored but not in the index yet"); return; }
  if (ix_stale) {
    from = 0;
    Log.printf("previous shot: shot %d is stored but not in the index yet, using the index as it is\n", latest);
  } else if (at < 0 || ix.r[at].name_hash != hash || !newer) {
    if (tries > 1 || !g_before_id || (uint32_t)latest != g_before_id) { retry("this shot not stored yet"); return; }
    last_try = true;                                // nothing new stored: the machine did not keep this shot
    from = 0;
    Log.println("previous shot: this shot was not stored by the machine, using the newest stored one");
  }
  if (!last_try && at >= 0 && (dd < -10 || dd > 10))
    Log.printf("previous shot: stored shot %d is %u.%u s, the screen saw %u.%u s\n", latest,
                  (unsigned)(ix.r[at].dur10 / 10), (unsigned)(ix.r[at].dur10 % 10), (unsigned)(dur / 10), (unsigned)(dur % 10));
  int prev = -1;
  for (int i = from; i < ix.n; i++) if (ix.r[i].name_hash == hash) { prev = i; break; }
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_prev_tries = 0;
  xSemaphoreGive(g_net_mtx);
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
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_hist_state = st;
  g_hist_prev_state = pst;
  g_hist_seq++;
  xSemaphoreGive(g_net_mtx);
}
static void hist_step(uint32_t ip, const LiveData *d) {
  static WsShotIndex *ixp = NULL;
  if (!ixp) ixp = (WsShotIndex *)heap_caps_malloc(sizeof *ixp, MALLOC_CAP_SPIRAM);
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const uint32_t id = g_hist_req;
  g_hist_req = 0;
  xSemaphoreGive(g_net_mtx);
  const bool have_ix = id && data_index(ixp);
  if (!id) return;
  if (d->brew_active || d->steam_active || d->hot_water_active) {
    Log.printf("history: shot %u not loaded, the machine is busy\n", (unsigned)id);
    hist_publish(PREV_FAILED, PREV_NONE);
    return;
  }
  if (!g_hist) g_hist = (bd_shot *)heap_caps_malloc(sizeof(bd_shot), MALLOC_CAP_SPIRAM);
  if (!g_hist_prev) g_hist_prev = (bd_shot *)heap_caps_malloc(sizeof(bd_shot), MALLOC_CAP_SPIRAM);
  if (!g_hist_shape) g_hist_shape = (bd_profile_shape *)heap_caps_malloc(sizeof(bd_profile_shape), MALLOC_CAP_SPIRAM);
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
  uint32_t ip;
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
static void select_step(uint32_t ip, char *buf, size_t cap, const LiveData *d) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const int id = g_select_req;
  const uint32_t at = g_select_ms;
  g_select_req = 0;
  xSemaphoreGive(g_net_mtx);
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

void net_set_machine(const char *host) {
  if (!g_net_mtx || !host[0]) return;
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  snprintf(g_mhost_new, sizeof g_mhost_new, "%s", host);
  g_mhost_req = true;
  xSemaphoreGive(g_net_mtx);
  net_wake();
}
void net_get_machine(char *out, size_t cap) {
  if (!g_net_mtx) { snprintf(out, cap, "%s", ""); return; }
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  snprintf(out, cap, "%s", g_mhost);
  xSemaphoreGive(g_net_mtx);
}
// ---- the network task: one pass of its loop, step by step ------------------------------------------------------
// The loop's state (only this task touches it).
static struct {
  char buf[8192];                                   // a full profile is ~0.7-3 KB, the status ~1 KB
  LiveData d;                                       // the last status reply
  uint32_t ip;
  bool have_ip;
  int fails;                                        // status requests failed in a row
} g_loop;

// Wi-Fi: join, rejoin, roaming, the screen's own Wi-Fi requests. true = it did something: start the pass again
static bool loop_wifi(void) {
  static bool rest_idle = true;                     // from the last status reply
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool idle = rest_idle && g_prev_tries <= 0 && millis() - data_last_shot_ms() > 90000;
  xSemaphoreGive(g_net_mtx);
  if (wifi_step(idle)) { g_loop.have_ip = false; g_loop.fails = 0; return true; }
  rest_idle = !g_loop.d.brew_active && !g_loop.d.steam_active && !g_loop.d.hot_water_active;
  return false;
}

// a new machine address from the screen: saved, then looked up from scratch
static void loop_machine_addr(void) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool hreq = g_mhost_req;
  char nh[64];
  memcpy(nh, g_mhost_new, sizeof nh);
  g_mhost_req = false;
  xSemaphoreGive(g_net_mtx);
  if (!hreq) return;
  kv_set_str("machine", "host", nh);
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  snprintf(g_mhost, sizeof g_mhost, "%s", nh);
  g_have_ip = false;
  xSemaphoreGive(g_net_mtx);
  g_last_ip = 0;
  g_loop.have_ip = false;
  g_loop.fails = 0;
  Log.printf("net: machine address set to \"%s\" (saved)\n", nh);
  data_publish(NULL, BD_LINK_NO_MACHINE);
}

// no Wi-Fi: say why, wait. true = down
static bool loop_wifi_down(void) {
  int why;
  if (!wifi_down_why(&why)) return false;
  data_set_why(why, 0);
  data_publish(NULL, BD_LINK_CONNECTING);
  g_loop.have_ip = false;
  delay(500);
  return true;
}

// the machine's address (mDNS name or IP; the last one found while the name does not answer). false = not found
static bool loop_find_machine(void) {
  if (g_loop.have_ip) return true;
  static bool mdns_up = false;                      // our own name (brewdeck.local) + the responder for the lookups
  if (!mdns_up) {
    mdns_up = mdns_init() == ESP_OK;
    if (mdns_up && mdns_hostname_set(board_hostname()) != ESP_OK) Log.println("net: mDNS name not set");
  }
  uint32_t ip = 0;
  char is[16];
  bool found = resolve(&ip);
  if (!found && g_last_ip) {
    ip_str(g_last_ip, is, sizeof is);
    Log.printf("net: mDNS no answer, keeping %s\n", is);
    ip = g_last_ip;
    found = true;
  }
  if (!found) {
    static uint32_t t_log;
    if (!t_log || millis() - t_log > 30000) {        // every 30 s, so the log shows the task is still trying
      t_log = stamp_nz(millis());
      Log.printf("net: \"%s\" not found (mDNS), retrying\n", g_mhost);
    }
    data_set_why(BD_WHY_NAME_UNKNOWN, 0);
    data_publish(NULL, BD_LINK_NO_MACHINE);
    delay(1500);
    return false;
  }
  g_loop.ip = ip;
  g_loop.have_ip = true;
  g_last_ip = ip;
  ip_str(ip, is, sizeof is);
  Log.printf("machine at %s\n", is);
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_ip = ip;
  g_have_ip = true;
  xSemaphoreGive(g_net_mtx);
  return true;
}

// a Wi-Fi scan (roaming, or the Wi-Fi page) takes the radio off the channel for 1-4 s: a request then fails and
// counted towards "machine not answering". Wait for the scan instead; the WebSocket keeps the last values meanwhile.
// true = waiting
static bool loop_scan_wait(void) {
  static uint32_t scan_wait = 0;                    // at most 6 s: a scan that never ends must not stop the polling
  if (wifi_scan_running() && (!scan_wait || millis() - scan_wait < 6000)) {
    if (!scan_wait) scan_wait = stamp_nz(millis());
    delay(100);
    return true;
  }
  scan_wait = 0;
  return false;
}

// the status poll (~1 s) and what follows it: a pick, the previous shot, a history tap, the profiles
static void loop_poll(void) {
  LiveData *d = &g_loop.d;
  const uint32_t ip = g_loop.ip;
  // a shot-history tap first (it woke the task): judged on the last status (< ~1 s old, it must have succeeded),
  // so the shot is not delayed by this status request; the call after the status below covers the other case
  if (g_loop.fails == 0 && d->mode[0]) hist_step(ip, d);
  if (fetch(ip, "/api/system/status", g_loop.buf, sizeof g_loop.buf) && strchr(g_loop.buf, '{')) {
    parse_status(g_loop.buf, d);
    data_set_why(BD_WHY_NONE, 0);
    data_publish(d, BD_LINK_ONLINE);
    g_loop.fails = 0;
    select_step(ip, g_loop.buf, sizeof g_loop.buf, d);
    {   // remember the newest stored shot while idle and no lookup is pending (see g_before_id)
      const uint32_t newest = data_newest_shot();
      xSemaphoreTake(g_net_mtx, portMAX_DELAY);
      if (!d->brew_active && g_prev_tries <= 0 && newest) g_before_id = newest;
      xSemaphoreGive(g_net_mtx);
    }
    prev_step(ip, d);
    hist_step(ip, d);
    profiles_step(ip, g_loop.buf, sizeof g_loop.buf, d);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(900));   // ~1 s poll; a profile tap wakes it at once
  } else {
    ++g_loop.fails;
    int8_t rssi = 0;
    net_wifi_up(&rssi);
    Log.printf("net: status request %d failed (rssi %d)\n", g_loop.fails, rssi);
    if (g_loop.fails >= 3) {
      Log.println("net: machine not answering, looking it up again");
      data_set_why(BD_WHY_NO_REPLY, ip);
      data_publish(NULL, BD_LINK_NO_MACHINE);
      g_loop.have_ip = host_is_ip(g_mhost);
    }
    delay(700);
  }
}

static void task(void *) {
  wifi_start();
  for (;;) {
    if (loop_wifi()) continue;
    loop_machine_addr();
    if (loop_wifi_down()) continue;
    if (!loop_find_machine()) continue;
    if (loop_scan_wait()) continue;
    loop_poll();
  }
}

void net_wake(void) { if (g_task) xTaskNotifyGive(g_task); }

void net_begin(void) {
  data_init();
  g_shot_buf = (char *)heap_caps_malloc(SHOT_BUF_CAP, MALLOC_CAP_SPIRAM);
  load_mhost();
  wifi_setup();                       // before the panel starts (see net.h)
  xTaskCreatePinnedToCore(task, "net", 6144, NULL, 1, &g_task, 0);   // measured peak ~2.6 KB (GET /tasks)
}

void net_addr_info(char *screen, size_t scap, char *machine, size_t mcap) {
  if (g_net_mtx) wifi_screen_ip(screen, scap);
  else snprintf(screen, scap, "%s", "");
  uint32_t ip;
  if (net_machine_ip(&ip)) { ip_str(ip, machine, mcap); return; }
  NetLock l;   // the network task rewrites the name when the address is changed
  snprintf(machine, mcap, "%s", g_mhost);
}

void net_select_profile(int id) {
  if (id <= 0) return;
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_select_req = id;
  g_select_ms = millis();
  xSemaphoreGive(g_net_mtx);
  net_wake();
}

// ---- WebSocket hand-over (net_ws.h), called from the ws task ----
bool net_machine_ip(uint32_t *ip) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const bool ok = g_have_ip;
  if (ok) *ip = g_ip;
  xSemaphoreGive(g_net_mtx);
  return ok;
}
void net_request_prev(const char *profile, int dur10) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_prev_hash = shot_name_hash(profile, strlen(profile));
  g_prev_dur10 = dur10 < 0 ? 0 : (uint32_t)dur10;
  g_prev_tries = 8;                   // ~8 s for the machine to store the shot
  g_prev_asked_ms = millis();
  g_prev_state = PREV_LOADING;
  g_prev_seq++;
  xSemaphoreGive(g_net_mtx);
  net_wake();
}

void net_request_hist(uint32_t id) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  g_hist_req = id;
  g_hist_state = PREV_LOADING;
  xSemaphoreGive(g_net_mtx);
  net_wake();
}

int net_get_hist(bd_shot *shot, bd_profile_shape *shape, bd_shot *prev, int *prev_state, bool *changed) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const int st = g_hist_state;
  *changed = g_hist_seq != g_hist_seen;
  *prev_state = g_hist_prev_state;
  if (*changed && st == PREV_READY) {
    memcpy(shot, g_hist, sizeof *shot);
    memcpy(shape, g_hist_shape, sizeof *shape);
    if (g_hist_prev_state == PREV_READY) memcpy(prev, g_hist_prev, sizeof *prev);
  }
  g_hist_seen = g_hist_seq;
  xSemaphoreGive(g_net_mtx);
  return st;
}

int net_get_prev(bd_shot *out, bool *changed) {
  xSemaphoreTake(g_net_mtx, portMAX_DELAY);
  const int st = g_prev_state;
  *changed = g_prev_seq != g_prev_seen;
  if (*changed && st == PREV_READY && g_prev && out) memcpy(out, g_prev, sizeof *out);
  g_prev_seen = g_prev_seq;
  xSemaphoreGive(g_net_mtx);
  return st;
}

