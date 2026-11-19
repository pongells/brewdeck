// See ws.h. Minimal RFC 6455 client: handshake, unmasked server frames in; masked pong / close and the commands (ws.h)
// out. Decoded values go to net_data.cpp (net_ws_*), which overlays them on the REST data while they are fresh.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>
#include "net.h"
#include "net_ws.h"
#include "pb.h"
#include "stamp.h"
#include "tlog.h"
#include "ws.h"

static int wifi_rssi(void) {   // the network task's reading: asking Wi-Fi from here would block on the ESP32-P4
  int8_t r = 0;
  net_wifi_up(&r);
  return r;
}

static uint8_t g_rx[4096];   // one frame; bigger ones (shot history index ~2.2 KB is the largest seen) are skipped

static bool read_n(WiFiClient &c, uint8_t *p, size_t n) {
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < n) {
    if (!c.connected() && !c.available()) return false;
    int a = c.available();
    if (a <= 0) {
      if (millis() - t0 > 3000) return false;
      delay(2);
      continue;
    }
    int r = c.read(p + got, n - got);
    if (r > 0) got += r;
  }
  return true;
}

static void send_ctrl(WiFiClient &c, uint8_t opcode, const uint8_t *p, size_t n) {   // pong / close only
  if (n > 125) n = 125;
  uint8_t f[2 + 4 + 125];
  const uint32_t m = esp_random();
  f[0] = 0x80 | opcode;
  f[1] = 0x80 | (uint8_t)n;
  memcpy(f + 2, &m, 4);
  for (size_t i = 0; i < n; i++) f[6 + i] = p[i] ^ f[2 + (i & 3)];
  c.write(f, 6 + n);
}

static bool handshake(WiFiClient &c, const IPAddress &ip) {
  uint8_t k[16];
  for (int i = 0; i < 16; i += 4) { uint32_t r = esp_random(); memcpy(k + i, &r, 4); }
  static const char *B = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char key[25];
  int o = 0;
  for (int i = 0; i < 16; i += 3) {                 // base64 of 16 bytes = 24 chars
    uint32_t v = k[i] << 16 | (i + 1 < 16 ? k[i + 1] << 8 : 0) | (i + 2 < 16 ? k[i + 2] : 0);
    key[o++] = B[v >> 18 & 63];
    key[o++] = B[v >> 12 & 63];
    key[o++] = i + 1 < 16 ? B[v >> 6 & 63] : '=';
    key[o++] = i + 2 < 16 ? B[v & 63] : '=';
  }
  key[o] = 0;
  c.printf("GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", ip.toString().c_str(), key);
  char line[128];
  c.setTimeout(3000);   // milliseconds (Stream::setTimeout), not seconds
  size_t n = c.readBytesUntil('\n', line, sizeof line - 1);
  line[n] = 0;
  const bool ok = strstr(line, " 101") != NULL;
  if (!ok) Log.printf("ws: handshake refused: %s\n", line);   // e.g. 429 = all 3 slots in use
  while (c.connected()) {                           // rest of the response headers
    n = c.readBytesUntil('\n', line, sizeof line - 1);
    if (n <= 1) break;
  }
  return ok;
}

// ---- tare --------------------------------------------------------------------
// Tare: c_tare_pend + UpdateSystemStateCommandDto { tarePending: true }, exactly what the machine's web UI sends from
// its Tare buttons. operationMode (field 1)
// is left out, as the web UI does (proto3 would decode it as 0 = BREW_AUTO; presumably the machine's tare handler
// reads only tarePending, since the web UI's message is identical). Sent only on a tap of Tare, and only while this task's own latest data says: a
// scale is connected, no shot / steam / hot water running, mode HOME. The request expires after 2 s.
static volatile uint32_t g_tare_req;                // millis() of the tap, 0 = none
static bool g_seen_brew = true, g_seen_scale = false;   // from the latest d_sensor_snap / d_sys_state
static int  g_seen_mode = -1;

void ws_tare(void) { g_tare_req = stamp_nz(millis()); }

// flush: c_opmode FLUSH / FLUSH_AUTO only from HOME with nothing running, HOME only from a flush mode
static volatile uint32_t g_flush_req;               // millis() of the tap / long press, 0 = none
static volatile int g_flush_mode;
void ws_flush(int mode) { g_flush_mode = mode; g_flush_req = stamp_nz(millis()); }

static bool tare_allowed(void) { return g_seen_scale && !g_seen_brew && g_seen_mode == PB_MODE_HOME; }

// masked binary frame of any length (client -> server); small header on the stack, payload masked in chunks
static void send_binary(WiFiClient &c, const uint8_t *p, size_t n) {
  uint8_t h[14];
  size_t k = 0;
  h[k++] = 0x80 | 2;
  if (n < 126) h[k++] = 0x80 | (uint8_t)n;
  else { h[k++] = 0x80 | 126; h[k++] = (uint8_t)(n >> 8); h[k++] = (uint8_t)n; }   // profiles are < 64 KB
  const uint32_t m = esp_random();
  memcpy(h + k, &m, 4);
  const uint8_t *mk = h + k;
  k += 4;
  c.write(h, k);
  uint8_t chunk[256];
  for (size_t o = 0; o < n; o += sizeof chunk) {
    const size_t l = n - o < sizeof chunk ? n - o : sizeof chunk;
    for (size_t i = 0; i < l; i++) chunk[i] = p[o + i] ^ mk[(o + i) & 3];
    c.write(chunk, l);
  }
}

// ---- target weight -----------------------------------------------------------
// c_upd_act_prof with the machine's own last d_act_prof, only globalStopConditions.weight changed (pb_weight_cmd),
// as the web UI's dashboard does. Only while idle in HOME, only if the active profile is still the one that
// was edited (same name), request expires after 3 s. The machine's d_resp answer is logged.
static uint8_t *g_prof;                             // last d_act_prof (ProfileDto), PSRAM
static size_t g_prof_n;
static uint8_t *g_cmd;                              // command buffer, PSRAM
static const size_t PROF_CAP = 4096, CMD_CAP = 4096 + 64;
// requests come from the UI task, the WS task takes them: name and values are handed over under g_req_mux
static portMUX_TYPE g_req_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t g_w_req;                   // millis() of the request, 0 = none
static volatile int g_w_req10;
static char g_w_name[48];
static volatile bool g_w_save = true, g_t_save = true;

void ws_set_weight(const char *profile, int weight10, bool save) {
  const uint32_t now = stamp_nz(millis());
  portENTER_CRITICAL(&g_req_mux);
  strlcpy(g_w_name, profile, sizeof g_w_name);
  g_w_req10 = weight10;
  g_w_save = save;
  g_w_req = now;
  portEXIT_CRITICAL(&g_req_mux);
}

// the profile just sent becomes our copy until the machine's next d_act_prof: a weight change followed by a
// temperature change (or the other way round) must not put the first value back
static void keep_sent(size_t n) {
  char act[24];
  const uint8_t *d;
  size_t dl;
  if (pb_envelope(g_cmd, n, act, sizeof act, &d, &dl) && dl <= PROF_CAP) { memmove(g_prof, d, dl); g_prof_n = dl; }
}

// ---- save on the machine -----------------------------------------------------
// c_save_act_prof (PB_SAVE_PROF_CMD) makes the screen's changes stick across a machine restart, like the Save button
// of the machine's built-in screen: 3 s after the last weight / temperature change sent from here (one save for a run
// of -/+ taps), and after a profile picked on this screen once the machine reports it active (boot profile). Only
// while idle in HOME and only while the active profile is the one changed / picked; a pick that the machine has not
// taken within 10 s is dropped. Nothing is saved for changes made elsewhere (web UI, built-in screen).
static uint32_t g_save_due;                         // millis() when to save, 0 = nothing pending (WS task only)
static uint32_t g_save_set;                         // when it was asked for (expiry)
static char g_save_name[48];
static volatile uint32_t g_pick_req;                // profile picked on the screen (from the UI task)
// The save writes the WHOLE active profile, so before it the stored profile
// is read (GET /api/profile/<id>) and, for the field(s) this screen did NOT change since the last save, active and
// stored must agree; otherwise someone changed them only temporarily (the web UI dashboard edits stop weight and brew
// temperature on the active profile only) and the save is skipped with a banner. Profile edits in the web UI go
// straight to the stored profile, so they never show up here.
static bool g_chg_w, g_chg_t;                       // changed on this screen since the last save, for g_chg_name
static char g_chg_name[48];
static void chg_mark(const char *profile, bool weight) {
  if (strcmp(g_chg_name, profile) != 0) { snprintf(g_chg_name, sizeof g_chg_name, "%s", profile); g_chg_w = g_chg_t = false; }
  if (weight) g_chg_w = true; else g_chg_t = true;
}
static void save_note(const char *msg) {            // a banner on the screen (through the notification path)
  WsNotif n;
  n.type = NOTIF_WARN;
  snprintf(n.msg, sizeof n.msg, "%s", msg);
  net_ws_notif(&n);
}
static char g_pick_name[48];
static void save_later(const char *profile, uint32_t delay_ms) {
  snprintf(g_save_name, sizeof g_save_name, "%s", profile);
  g_save_set = millis();
  g_save_due = stamp_nz(g_save_set + delay_ms);
}
void ws_save_pick(const char *profile) {
  const uint32_t now = stamp_nz(millis());
  portENTER_CRITICAL(&g_req_mux);
  strlcpy(g_pick_name, profile, sizeof g_pick_name);
  g_pick_req = now;
  portEXIT_CRITICAL(&g_req_mux);
}
static void save_step(WiFiClient &c) {
  if (g_pick_req) {                                 // a pick older than 10 s (no session then) is not saved later
    char pick[sizeof g_pick_name];
    portENTER_CRITICAL(&g_req_mux);
    const uint32_t r = g_pick_req;
    g_pick_req = 0;
    memcpy(pick, g_pick_name, sizeof pick);
    portEXIT_CRITICAL(&g_req_mux);
    if (millis() - r < 10000) save_later(pick, 1000);
    else Log.println("ws: pick save dropped (request too old)");
  }
  if (!g_save_due || (int32_t)(millis() - g_save_due) < 0) return;
  uint32_t id = 0;
  char name[48] = "";
  float w0;
  const bool known = g_prof_n && pb_profile_info(g_prof, g_prof_n, &id, name, sizeof name, &w0);
  if (!known || strcmp(name, g_save_name) != 0) {   // a pick not taken yet: wait, up to 10 s
    if (millis() - g_save_set > 10000) {
      g_save_due = 0;
      Log.printf("ws: save dropped (active profile \"%s\", wanted \"%s\")\n", name, g_save_name);
    }
    return;
  }
  if (g_seen_brew || g_seen_mode != PB_MODE_HOME) {            // busy (a shot right after the change): save once idle again,
    // within 15 min (dropping it after 10 s lost the change at the next machine restart)
    if (millis() - g_save_set > 15u * 60u * 1000u) { g_save_due = 0; Log.println("ws: save dropped (machine busy)"); }
    return;
  }
  {   // fields not changed here must match the stored profile
    float sw, st, aw = w0, at = 0;
    pb_profile_temp(g_prof, g_prof_n, &at);
    if (!net_stored_profile((int)id, &sw, &st)) {   // no answer: try again in 5 s, within the same 15 min
      if (millis() - g_save_set > 15u * 60u * 1000u) { g_save_due = 0; Log.println("ws: save dropped (stored profile not read)"); return; }
      Log.println("ws: save waits (stored profile not read)");
      g_save_due = stamp_nz(millis() + 5000);
      return;
    }
    const bool same_profile = !strcmp(g_chg_name, name);
    const bool w_ok = (same_profile && g_chg_w) || fabsf(aw - sw) < 0.05f;
    const bool t_ok = (same_profile && g_chg_t) || fabsf(at - st) < 0.05f;
    if (!w_ok || !t_ok) {
      g_save_due = 0;
      Log.printf("ws: save skipped: %s%s%s changed elsewhere, not saved (active %.1f g %.1f C, stored %.1f g %.1f C)\n",
                 w_ok ? "" : "stop weight", (!w_ok && !t_ok) ? " and " : "", t_ok ? "" : "brew temperature",
                 aw, at, sw, st);
      save_note("Not saved: profile has other unsaved changes (web UI)");
      return;
    }
  }
  g_save_due = 0;
  g_chg_w = g_chg_t = false;                        // saved: active == stored from here
  send_binary(c, PB_SAVE_PROF_CMD, PB_SAVE_PROF_CMD_LEN);
  Log.printf("ws: profile saved on the machine (c_save_act_prof, profile %u \"%s\")\n", (unsigned)id, name);
  net_profile_saved((int)id);                       // the profile list re-reads its stored values
}

static void weight_step(WiFiClient &c) {
  if (!g_w_req) return;
  char want[sizeof g_w_name];
  portENTER_CRITICAL(&g_req_mux);
  const uint32_t r = g_w_req;
  const int req10 = g_w_req10;
  const bool save = g_w_save;
  g_w_req = 0;
  memcpy(want, g_w_name, sizeof want);
  portEXIT_CRITICAL(&g_req_mux);
  if (millis() - r > 3000) { Log.println("ws: weight dropped (request too old)"); return; }
  uint32_t id;
  char name[48];
  float w0;
  if (!g_prof_n || !pb_profile_info(g_prof, g_prof_n, &id, name, sizeof name, &w0)) {
    Log.println("ws: weight refused (no active profile from the machine yet)");
    return;
  }
  if (g_seen_brew || g_seen_mode != PB_MODE_HOME || strcmp(name, want) != 0) {
    Log.printf("ws: weight refused (brewing/steam %d, mode %d, profile \"%s\" vs \"%s\")\n", g_seen_brew,
                  g_seen_mode, name, want);
    return;
  }
  const float w = req10 / 10.0f;
  if (w == w0) return;                              // nothing to change
  const size_t n = g_cmd ? pb_weight_cmd(g_prof, g_prof_n, w, g_cmd, CMD_CAP) : 0;
  if (!n) { Log.println("ws: weight refused (could not build the command)"); return; }
  send_binary(c, g_cmd, n);
  keep_sent(n);
  if (save) { chg_mark(name, true); save_later(name, 3000); }
  Log.printf("ws: target weight %.1f -> %.1f g sent (profile %u, %u B)\n", w0, w, (unsigned)id, (unsigned)n);
}

// ---- brew temperature ---------------------------------------------------------
// Same rules as the weight: c_upd_act_prof with only ProfileDto.waterTemperature changed (pb_temp_cmd, 1-105 degC, never 0),
// idle in HOME, same active profile, request expires after 3 s.
static volatile uint32_t g_t_req;
static volatile int g_t_req_deg;
static char g_t_name[48];

void ws_set_temp(const char *profile, int deg, bool save) {
  const uint32_t now = stamp_nz(millis());
  portENTER_CRITICAL(&g_req_mux);
  strlcpy(g_t_name, profile, sizeof g_t_name);
  g_t_req_deg = deg;
  g_t_save = save;
  g_t_req = now;
  portEXIT_CRITICAL(&g_req_mux);
}

static void temp_step(WiFiClient &c) {
  if (!g_t_req) return;
  char want[sizeof g_t_name];
  portENTER_CRITICAL(&g_req_mux);
  const uint32_t r = g_t_req;
  const int req_deg = g_t_req_deg;
  const bool save = g_t_save;
  g_t_req = 0;
  memcpy(want, g_t_name, sizeof want);
  portEXIT_CRITICAL(&g_req_mux);
  if (millis() - r > 3000) { Log.println("ws: temperature dropped (request too old)"); return; }
  uint32_t id;
  char name[48];
  float w0, t0;
  if (!g_prof_n || !pb_profile_info(g_prof, g_prof_n, &id, name, sizeof name, &w0) ||
      !pb_profile_temp(g_prof, g_prof_n, &t0)) {
    Log.println("ws: temperature refused (no active profile from the machine yet)");
    return;
  }
  if (g_seen_brew || g_seen_mode != PB_MODE_HOME || strcmp(name, want) != 0) {
    Log.printf("ws: temperature refused (brewing/steam %d, mode %d, profile \"%s\" vs \"%s\")\n", g_seen_brew,
                  g_seen_mode, name, want);
    return;
  }
  const float t = (float)req_deg;
  if (t == t0) return;
  const size_t n = g_cmd ? pb_temp_cmd(g_prof, g_prof_n, t, g_cmd, CMD_CAP) : 0;
  if (!n) { Log.println("ws: temperature refused (could not build the command / out of range)"); return; }
  send_binary(c, g_cmd, n);
  keep_sent(n);
  if (save) { chg_mark(name, false); save_later(name, 3000); }
  Log.printf("ws: brew temperature %.1f -> %.0f C sent (profile %u, %u B)\n", t0, t, (unsigned)id, (unsigned)n);
}

static void flush_step(WiFiClient &c) {
  const uint32_t r = g_flush_req;
  if (!r) return;
  g_flush_req = 0;
  const int m = g_flush_mode;
  if (millis() - r > 2000) { Log.println("ws: flush dropped (request too old)"); return; }
  const bool from_flush = g_seen_mode == PB_MODE_FLUSH || g_seen_mode == PB_MODE_FLUSH_AUTO;
  const bool ok = m == PB_MODE_HOME ? from_flush : (m == PB_MODE_FLUSH || m == PB_MODE_FLUSH_AUTO) &&
                                                       g_seen_mode == PB_MODE_HOME && !g_seen_brew;
  uint8_t b[16];
  const size_t n = ok ? pb_opmode_cmd(m, b, sizeof b) : 0;
  if (!n) {
    Log.printf("ws: flush mode %d refused (machine mode %d, brewing/steam/water %d)\n", m, g_seen_mode, g_seen_brew);
    return;
  }
  static uint32_t t_last;
  if (t_last && millis() - t_last < 500) return;    // at most two per second
  t_last = millis();
  send_binary(c, b, n);
  Log.printf("ws: mode %s sent (was %s)\n", ws_mode_name(m), ws_mode_name(g_seen_mode));
}

static void tare_step(WiFiClient &c) {
  const uint32_t r = g_tare_req;
  if (!r) return;
  g_tare_req = 0;
  if (millis() - r > 2000) { Log.println("ws: tare dropped (request too old)"); return; }
  if (!tare_allowed()) {
    Log.printf("ws: tare refused (scale %d, brewing/steam %d, mode %d)\n", g_seen_scale, g_seen_brew, g_seen_mode);
    return;
  }
  static uint32_t t_last;
  if (t_last && millis() - t_last < 1000) return;   // one tare per second at most (the web UI debounces 250 ms)
  t_last = millis();
  send_binary(c, PB_TARE_CMD, PB_TARE_CMD_LEN);
  Log.println("ws: tare sent");
}

static void handle_frame(const uint8_t *f, size_t n, uint32_t *cnt);
static void handle(const uint8_t *f, size_t n, uint32_t *cnt) {
  handle_frame(f, n, cnt);
#ifdef BREWDECK_DEBUG_HTTP
  char act[24];                                     // where the internal-RAM low point moved (mem_watch, debug)
  const uint8_t *d;
  size_t dl;
  if (pb_envelope(f, n, act, sizeof act, &d, &dl)) mem_watch("ws", act);
#endif
}
static void handle_frame(const uint8_t *f, size_t n, uint32_t *cnt) {
  char act[24];
  const uint8_t *d;
  size_t dl;
  if (!pb_envelope(f, n, act, sizeof act, &d, &dl)) return;
  if (!strcmp(act, "d_sensor_snap")) {
    WsSensor s;
    if (pb_sensor(d, dl, &s)) {
      net_ws_sensor(&s);
      cnt[0]++;
      g_seen_brew = s.brew_active || s.steam_active || s.hot_water;
    }
  } else if (!strcmp(act, "d_sys_state")) {
    WsSystem s;
    if (pb_system(d, dl, &s)) {
      net_ws_system(&s);
      cnt[1]++;
      g_seen_scale = s.scales_present;
      g_seen_mode = s.mode;
    }
  } else if (!strcmp(act, "d_shot_snap")) {
    WsShot s;
    if (pb_shot(d, dl, &s)) { net_ws_shot(&s); cnt[2]++; }
  } else if (!strcmp(act, "d_shot_hist_index")) {
    static WsShotIndex *ix = NULL;                  // ~2.8 KB in PSRAM (internal RAM is for Wi-Fi)
    if (!ix) ix = (WsShotIndex *)heap_caps_malloc(sizeof *ix, MALLOC_CAP_SPIRAM);
    if (ix && pb_shot_index(d, dl, ix)) net_ws_index(ix);
  } else if (!strcmp(act, "d_notif")) {          // notification ("Releasing pressure!", ...): banner on the screen
    WsNotif nt;
    if (pb_notif(d, dl, &nt)) {
      net_ws_notif(&nt);
      static char last[64] = "";
      static uint32_t last_ms = 0;
      if (strcmp(last, nt.msg) || millis() - last_ms > 60000) {   // each text once a minute (the tank one repeats ~1.4/s)
        Log.printf("ws: notification %d \"%s\"\n", nt.type, nt.msg);
        snprintf(last, sizeof last, "%s", nt.msg);
        last_ms = millis();
      }
    }
  } else if (!strcmp(act, "d_act_prof")) {      // the active profile, kept for the weight / temperature commands and the save check
    if (!g_prof) g_prof = (uint8_t *)heap_caps_malloc(PROF_CAP, MALLOC_CAP_SPIRAM);
    if (!g_cmd) g_cmd = (uint8_t *)heap_caps_malloc(CMD_CAP, MALLOC_CAP_SPIRAM);
    if (g_prof && dl <= PROF_CAP) { memcpy(g_prof, d, dl); g_prof_n = dl; }
    else { g_prof_n = 0; Log.printf("ws: active profile not kept (%u B)\n", (unsigned)dl); }
    uint32_t id;
    char name[48];
    float w, t;
    if (pb_profile_info(d, dl, &id, name, sizeof name, &w) && pb_profile_temp(d, dl, &t)) net_ws_act_profile(name, w, t);
  } else if (!strcmp(act, "d_resp")) {
    char ra[32], re[64];
    int res;
    if (pb_resp(d, dl, ra, sizeof ra, &res, re, sizeof re)) {
      Log.printf("ws: answer to %s: %s %s\n", ra, res ? "ERROR" : "OK", re);
      // a refused profile change: forget our patched copy (keep_sent), so the next change starts from the machine's
      // next d_act_prof instead of carrying the refused value along
      if (res && !strcmp(ra, "c_upd_act_prof")) g_prof_n = 0;
    }
  } else if (!strcmp(act, "d_settings")) {
    // carries MQTT credentials and API tokens: only the 3 scale-type flags (pb_scale_cfg), the steam set point (pb_steam_set) and the built-in screen / web UI versions
    // (pb_versions, Menu > About; not logged) are decoded; nothing else of it is logged or
    // kept, the frame buffer is overwritten by the next frame
    uint32_t steam;
    if (pb_steam_set(d, dl, &steam)) net_ws_steam_set(steam);
    char fv[28], sv[28];
    if (pb_versions(d, dl, fv, sizeof fv, sv, sizeof sv)) net_ws_versions(fv, sv);
    WsScaleCfg sc;
    if (pb_scale_cfg(d, dl, &sc)) {
      net_ws_scale_cfg(&sc);
      Log.printf("ws: scale settings: predictive %d, hardware %d, bluetooth %d (%u B settings frame)\n",
                    sc.force_predictive, sc.hw_enabled, sc.bt_enabled, (unsigned)n);
    }
  }
  // everything else is ignored and never stored
}

// The machine pushes something at least every ~1 s (d_esp_mem even when idle). Seen: a connection can go silent for
// good (open, no close frame) when another client connects; other clients keep getting data.
static const uint32_t SILENT_MS = 5000;

static uint32_t g_frames, g_bytes, g_t_conn;   // per connection, logged on disconnect (debug)

static IPAddress g_sess_ip;                         // address of the running session

static void session(WiFiClient &c) {
  uint32_t cnt[3] = {0, 0, 0}, t_log = millis(), t_rx = millis();
  g_frames = g_bytes = 0;
  g_t_conn = millis();
  size_t asm_n = 0;                                 // fragmented message being assembled in g_rx
  bool asm_drop = false;
  g_seen_brew = true;                               // nothing known on a new connection: no tare until data arrives
  g_seen_scale = false;
  g_seen_mode = -1;
  g_prof_n = 0;                                     // no active profile until this session's d_act_prof (a change
                                                    // must not be built from a copy of another session / machine)
  for (;;) {
    if (!c.connected()) return;
    {   // machine address changed on the screen (or not known any more): leave the old machine and reconnect, else a
      static uint32_t t_chk;                      // wrong address stays "online" on the old socket
      if (millis() - t_chk > 500) {
        t_chk = millis();
        IPAddress cur;
        if (!net_machine_ip(&cur) || cur != g_sess_ip) {
          Log.println("ws: machine address changed, reconnecting");
          send_ctrl(c, 8, (const uint8_t *)"\x03\xe8", 2);   // close, 1000 normal
          return;
        }
      }
    }
    tare_step(c);
    flush_step(c);
    weight_step(c);
    temp_step(c);
    save_step(c);
    if (c.available() < 2) {
      delay(10);
      if (millis() - t_rx > SILENT_MS) {            // dead connection: free the machine's slot and reconnect
        Log.println("ws: no data for 5 s, reconnecting");
        send_ctrl(c, 8, (const uint8_t *)"\x03\xe8", 2);   // close, 1000 normal
        return;
      }
      if (millis() - t_log > 30000) {
        Log.printf("ws: last 30 s: sensor %u, system %u, shot %u; internal RAM free %u, largest %u, lowest %u; rssi %d\n",
                      (unsigned)cnt[0], (unsigned)cnt[1], (unsigned)cnt[2],
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), wifi_rssi());
        cnt[0] = cnt[1] = cnt[2] = 0;
        t_log = millis();
      }
      continue;
    }
    uint8_t h[2];
    if (!read_n(c, h, 2)) return;
    t_rx = millis();
    g_frames++;
    const bool fin = h[0] & 0x80, masked = h[1] & 0x80;
    const uint8_t op = h[0] & 0x0F;
    uint64_t len = h[1] & 0x7F;
    uint8_t x[8];
    if (len == 126) { if (!read_n(c, x, 2)) return; len = (uint64_t)x[0] << 8 | x[1]; }
    else if (len == 127) { if (!read_n(c, x, 8)) return; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | x[i]; }
    g_bytes += (uint32_t)len;
    uint8_t mk[4] = {0, 0, 0, 0};
    if (masked && !read_n(c, mk, 4)) return;        // servers must not mask, but accept it
    if (op >= 8) {                                  // control frame (<= 125 bytes)
      uint8_t p[125];
      if (len > 125 || !read_n(c, p, (size_t)len)) return;
      for (size_t i = 0; i < len; i++) p[i] ^= mk[i & 3];
      if (op == 9) send_ctrl(c, 10, p, (size_t)len);                       // ping -> pong
      else if (op == 8) { send_ctrl(c, 8, p, len >= 2 ? 2 : 0); return; } // close -> close back
      continue;
    }
    if (op != 0) { asm_n = 0; asm_drop = false; }   // a new message starts
    if (op == 1) asm_drop = true;                   // text frames: not used by this protocol
    if (asm_drop || len > sizeof g_rx || asm_n + len > sizeof g_rx) {   // too big (or text): read and discard
                                                    // (len checked alone too: a 64-bit length could wrap the sum)
      if (!asm_drop) Log.printf("ws: %u B message skipped (too big)\n", (unsigned)(asm_n + len));
      asm_drop = true;
      uint8_t junk[256];
      while (len) {
        size_t k = len > sizeof junk ? sizeof junk : (size_t)len;
        if (!read_n(c, junk, k)) return;
        len -= k;
      }
    } else {
      if (!read_n(c, g_rx + asm_n, (size_t)len)) return;
      for (size_t i = 0; i < len; i++) g_rx[asm_n + i] ^= mk[(asm_n + i) & 3];
      asm_n += (size_t)len;
    }
    if (fin) {
      if (!asm_drop && asm_n) handle(g_rx, asm_n, cnt);
      asm_n = 0;
      asm_drop = false;
    }
  }
}

static void task(void *) {
  uint32_t backoff = 2000;
  for (;;) {
    IPAddress ip;
    if (!net_wifi_up(NULL) || !net_machine_ip(&ip)) { delay(1000); continue; }
    WiFiClient c;
    if (c.connect(ip, 80, 3000) && handshake(c, ip)) {
      c.setNoDelay(true);
      Log.println("ws: connected");
      backoff = 2000;
      g_sess_ip = ip;
      session(c);
      Log.printf("ws: disconnected after %u ms, %u frames, %u bytes; rssi %d dBm\n", (unsigned)(millis() - g_t_conn),
                    (unsigned)g_frames, (unsigned)g_bytes, wifi_rssi());
    } else {
      backoff = backoff < 30000 ? backoff * 2 : 30000;   // machine off, rebooting, or all slots taken
    }
    c.stop();
    net_ws_lost();
    delay(backoff);
  }
}

void ws_begin(void) {
  xTaskCreatePinnedToCore(task, "ws", 6144, NULL, 1, NULL, 0);
}
