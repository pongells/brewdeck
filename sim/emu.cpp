// Interactive desktop emulator: a UI folder (Brewdeck's by default) in an 800x480 SDL window, mouse = touch, on a
// fake core (core_state.cpp, shared with the device) and a fake machine that plays recordings from sim/data (real
// machine replies, decoded by pb.cpp / parse.cpp). Nothing goes on the network. Build: sim/build-emu.sh, run: sim/emu.
// Keys, stdin commands and environment variables: HELP below (also printed at start), docs/development.md.
// What it does NOT show: device timing (the Mac is far faster), Wi-Fi behaviour, touch feel, real backlight.
#include "../firmware/src/core/stamp.h"
#ifdef EMU_WEB                                   // sim/web: the same fake machine compiled to WebAssembly, no SDL
#include <emscripten.h>
static uint32_t SDL_GetTicks(void) { return (uint32_t)emscripten_get_now(); }
#else
#include <SDL.h>
#endif
#include <lvgl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>
#include "ui.h"
#include "parse.h"
#include "pb.h"
#include "live.h"
#include "core_state.h"                    // the fake core keeps its state like the device core
// Brewdeck's UI test hooks (the panel's "Go to screen" buttons): another UI (UI=<folder> sim/build-emu.sh) does not
// have them, so these do nothing unless Brewdeck's UI is linked (its definitions win over the weak ones)
#define EMU_WEAK __attribute__((weak))
EMU_WEAK void ui_splash(void) {}
EMU_WEAK void ui_test_open_editor(void) {}
EMU_WEAK void ui_test_open_temp_editor(void) {}
EMU_WEAK void ui_test_profiles(int, int) {}
EMU_WEAK void ui_test_idle(void) {}
EMU_WEAK void ui_test_screen_off(void) {}
EMU_WEAK void ui_test_splash_end(void) {}
EMU_WEAK bool ui_test_splash_shown(void) { return false; }
EMU_WEAK int ui_test_brew_shown(void) { return 0; }
EMU_WEAK void ui_test_home(void) {}
EMU_WEAK void ui_test_wifi_password(const char *) {}
EMU_WEAK void ui_test_menu(int) {}
#include <map>
#include <string>

#define W 800
#define H 480

static const char *HELP =
    "keys:  S start shot (recorded LMD Single shot)   B scale on/off   P scale type BT/wired/predictive\n"
    "       H heating on/off   L water low/full   O machine offline/back   N no Wi-Fi/back\n"
    "       I idle screen now   M steam (recorded session)   R reload profiles   Esc/Q quit\n"
    "mouse = finger. Profile taps and weight changes go to the fake machine (printed here).\n"
    "stdin: key s | tap x y | drag x1 y1 x2 y2 | hold x y ms | click x y | button <label> | wait ms | save f.raw |\n"
    "       savewin f.bmp | redraw | quit\n"
    "env:   EMU_DATA=<dir> other machine data   EMU_SHOT_CUT=s cut the shot   EMU_SHOT_PROFILE=id   EMU_IGNORE=1 the\n"
    "       machine ignores commands   EMU_FLUSHLOG=1 log big screen flushes\n";

// ---- board layer (brewdeck_core.h): the emulator's screen is a frame buffer, its touch the mouse ----------------
#include "bd_lvgl9.h"
static uint16_t g_fb[W * H];
static int g_mx, g_my;
static bool g_mdown;
static bool g_dirty = true;
static uint32_t g_flush_px = 0;              // px flushed in this lv_timer_handler pass (EMU_FLUSHLOG=1 prints big ones)

static const bd_display_info g_bd_info = {W, H, BD_PIXEL_RGB565, 80};
const bd_display_info *bd_display(void) { return &g_bd_info; }
void bd_display_flush(int x1, int y1, int x2, int y2, const uint16_t *px) {
  for (int y = y1; y <= y2; y++)
    for (int x = x1; x <= x2; x++) g_fb[y * W + x] = *px++;
  g_dirty = true;
  g_flush_px += (uint32_t)((x2 - x1 + 1) * (y2 - y1 + 1));
}
bool bd_touch_read(int *x, int *y) {
  *x = g_mx;
  *y = g_my;
  return g_mdown;
}

void bd_display_stats(uint32_t *us, uint32_t *px, bool) { if (us) *us = 0; if (px) *px = 0; }   // not measured here
void bd_debug_route(const char *, bd_debug_fn) {}                // no debug web server in the emulator
void *bd_alloc(size_t n, bd_mem where) { (void)where; return malloc(n); }
void bd_free(void *p) { free(p); }
void bd_log(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vprintf(fmt, a);
  va_end(a);
  printf("\n");
}

// ---- fake machine -----------------------------------------------------------------------------
static LiveData g_m;                      // what the "machine" reports
static int g_link = BD_LINK_ONLINE;
static bool g_offline = false, g_nowifi = false;
static ProfileList g_list;
static int g_active_id = 7;                 // LMD Single: the recorded shot's profile
static float g_temps[64];                 // per profile id: active brew temperature (0 = profile's own)
static float g_weights[64];               // per profile id: active target weight (0 = profile's own)
static int g_backlight = 100;

static char *read_file(const char *name, size_t *len) {
  char p[512];
  const char *dir = getenv("EMU_DATA");             // another fake machine's files (e.g. many profiles), else sim/data
  if (dir) snprintf(p, sizeof p, "%s/%s", dir, name);
  else snprintf(p, sizeof p, "%s/data/%s", SIM_DIR, name);
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = (char *)malloc(n + 1);
  size_t r = fread(b, 1, n, f);
  b[r] = 0;
  fclose(f);
  if (len) *len = r;
  return b;
}

static bool load_shape(int id, bd_profile_shape *sh) {
  char nm[40];
  snprintf(nm, sizeof nm, "profile_%d.json", id);
  char *js = read_file(nm, NULL);
  if (!js) return false;
  bool ok = parse_profile_shape(js, sh);
  free(js);
  sh->id = id;
  return ok;
}

static const char *profile_name(int id) {
  for (int i = 0; i < g_list.count; i++)
    if (g_list.p[i].id == id) return g_list.p[i].name;
  return "";
}

static void push_shape(void) {
  static bd_profile_shape sh;
  memset(&sh, 0, sizeof sh);
  if (!load_shape(g_active_id, &sh)) { sh.id = g_active_id; sh.n = 0; }   // no recording of this one: empty preview
  cs_take_shape(&sh);
  g_m.act_weight = g_weights[g_active_id] > 0 ? g_weights[g_active_id] : sh.stop_weight;
  snprintf(g_m.act_profile, sizeof g_m.act_profile, "%s", profile_name(g_active_id));
  float t = 0;
  for (int i = 0; i < g_list.count; i++) if (g_list.p[i].id == g_active_id) t = g_list.p[i].temp;
  if (g_temps[g_active_id % 64] > 0) t = g_temps[g_active_id % 64];
  g_m.act_temp = t > 0 ? t : 90;
  g_m.target = g_m.act_temp;
  snprintf(g_m.profile, sizeof g_m.profile, "%s", profile_name(g_active_id));
}

static void load_profiles(void) {
  memset(&g_list, 0, sizeof g_list);
  char *js = read_file("profiles_all.json", NULL);
  if (js) { parse_profile_list(js, &g_list); free(js); }
  for (int i = 0; i < g_list.count; i++) {
    ProfileInfo *p = &g_list.p[i];
    char nm[40];
    snprintf(nm, sizeof nm, "profile_%d.json", p->id);
    char *d = read_file(nm, NULL);
    if (d) { parse_profile_detail(d, p); free(d); }
    else { p->detail = true; p->phases = 0; p->weight = 0; p->temp = 0; }   // not recorded: shown without details
    p->selected = p->id == g_active_id;
  }
  cs_take_profiles(&g_list);
  push_shape();
  printf("machine: %d profiles, active %d \"%s\"\n", g_list.count, g_active_id, profile_name(g_active_id));
}

static void on_select(int id) {
  printf("machine <- select profile %d (POST /api/profile-select/%d)\n", id, id);
  g_active_id = id;
  for (int i = 0; i < g_list.count; i++) g_list.p[i].selected = g_list.p[i].id == id;
  cs_take_profiles(&g_list);
  // the machine answers later, not inside the tap: an instant push_shape came before the UI's "Loading..." and the
  // preview then stayed on "Loading..." (only in the emulator)
  lv_timer_t *t = lv_timer_create([](lv_timer_t *tm) { push_shape(); lv_timer_delete(tm); }, 400, NULL);
  (void)t;
}
static void on_weight(const char *profile, int w10) {
  printf("machine <- target weight %.1f g for \"%s\" (c_upd_act_prof)\n", w10 / 10.0, profile);
  if (strcmp(profile, profile_name(g_active_id)) == 0) {
    g_weights[g_active_id] = w10 / 10.0f;
    g_m.act_weight = w10 / 10.0f;
  }
}
static void on_temp(const char *profile, int deg) {
  printf("machine <- brew temperature %d C for \"%s\" (c_upd_act_prof)\n", deg, profile);
  if (strcmp(profile, profile_name(g_active_id)) == 0) {
    g_temps[g_active_id % 64] = deg;
    g_m.act_temp = deg;
    g_m.target = deg;
  }
}
static void on_tare(void) { printf("machine <- tare\n"); g_m.weight = 0; }
// flush (c_opmode): FLUSH / HOME switch the mode; FLUSH_AUTO is played as a 5 s flush, then HOME (a GUESS of what the
// machine does, its "Auto Flush Duration" defaults to 5 s; check against the real machine)
static uint32_t g_autoflush_ms = 0;
static void on_flush(int mode) {
  static const char *const N[8] = {"", "", "FLUSH", "", "", "FLUSH_AUTO", "", "HOME"};
  const char *n = mode >= 0 && mode < 8 ? N[mode] : "";
  printf("machine <- mode %s\n", n[0] ? n : "?");
  if (!n[0] || g_m.brew_active || g_m.steam_active) return;
  strcpy(g_m.mode, n);
  g_autoflush_ms = mode == 5 ? stamp_nz(SDL_GetTicks()) : 0;
  if (mode != 5) g_m.pressure = 0;
}
// steam: plays a REAL steam session (sim/data/ws_steam.bin, recorded with tools/ws_listen.py right after the demo
// shot; <u32 ms><u32 len><frame> per sensor / system frame): steam on at 0:01 with the boiler at 91 degC,
// heats ~1.8 degC/s while the pump fills the boiler (~12 bar held); wand opened ~1:30 -> ~1.5 bar while steaming,
// 145-158 degC around the 155 degC set point; wand closed ~3:05 -> pressure up again; steam off 3:20 at 155.5 degC,
// then cooling to 132 degC (~3.5-4 bar) when the recording stops at 3:57.
// "Steam" / M starts it (again: stops it, HOME); afterwards the pressure fades and the boiler cools to the target.
// "Steam, fast" plays it at 10x: the recording AND the screen's clock (lv_tick), so the steam timer and the chart's
// one point per second show the real session in ~24 s.
struct SteamFrame { uint32_t at; std::vector<uint8_t> b; };
static std::vector<SteamFrame> g_steam;
static struct { bool on, after; uint32_t t0; size_t i; int speed; } g_st;
static void load_steam(void) {
  size_t n;
  uint8_t *b = (uint8_t *)read_file("ws_steam.bin", &n);
  if (!b) { printf("no ws_steam.bin\n"); return; }
  for (size_t i = 0; i + 8 <= n;) {
    const uint32_t at = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
    const uint32_t L = b[i + 4] | b[i + 5] << 8 | b[i + 6] << 16 | (uint32_t)b[i + 7] << 24;
    if (i + 8 + L > n) break;
    g_steam.push_back({at, std::vector<uint8_t>(b + i + 8, b + i + 8 + L)});
    i += 8 + L;
  }
  free(b);
  printf("steam recording: %zu frames, %.1f s\n", g_steam.size(), g_steam.empty() ? 0 : g_steam.back().at / 1000.0);
}
static void steam_set(bool on, int speed = 1) {
  memset(&g_st, 0, sizeof g_st);
  g_st.speed = 1;
  if (on && !g_steam.empty()) {
    g_st.on = true; g_st.t0 = SDL_GetTicks(); g_st.speed = speed;
    printf("steam: playing the recorded session%s\n", speed > 1 ? " at 10x" : "");
    return;
  }
  g_m.steam_active = false;
  strcpy(g_m.mode, "HOME");
  g_st.after = true;
  printf("steam off\n");
}
static void steam_step(uint32_t now) {
  if (!g_st.on) {
    if (g_st.after) {                                        // pressure left after steam fades over a few minutes
      g_m.pressure *= 0.998f;
      if (g_m.pressure < 0.3f) { g_m.pressure = 0; g_st.after = false; }
    }
    return;
  }
  while (g_st.i < g_steam.size() && (now - g_st.t0) * (uint32_t)g_st.speed >= g_steam[g_st.i].at) {
    const std::vector<uint8_t> &f = g_steam[g_st.i++].b;
    char act[24]; const uint8_t *d; size_t dl;
    if (!pb_envelope(f.data(), f.size(), act, sizeof act, &d, &dl)) continue;
    if (!strcmp(act, "d_sensor_snap")) {
      WsSensor s; pb_sensor(d, dl, &s);
      g_m.temperature = s.temperature;
      g_m.pressure = s.pressure;
    } else {
      WsSystem s; pb_system(d, dl, &s);
      const bool st = s.mode == 4;                          // STEAM
      if (st != g_m.steam_active) printf("machine: mode %s\n", ws_mode_name(s.mode));
      g_m.steam_active = st;
      snprintf(g_m.mode, sizeof g_m.mode, "%s", ws_mode_name(s.mode));
    }
  }
  if (g_st.i >= g_steam.size()) { g_st.on = false; g_st.after = true; g_st.speed = 1; printf("steam recording done\n"); }
}

static void on_backlight(int pct) { g_backlight = pct; g_dirty = true; printf("screen: backlight %d %%\n", pct); }
// previous shot: answered 1.5 s after the request with a stored shot recording
static uint32_t g_prev_due = 0;
static void on_prev(const char *profile, int dur10) {
  printf("machine <- previous shot of \"%s\" (this one %.1f s): GET /api/shots/...\n", profile, dur10 / 10.0);
  g_prev_due = SDL_GetTicks() + 1500;
}
// shot history: a fake index of stored shots (the real stored shots in sim/data, plus copies of them under other
// ids, dates and profile names so the list has several days); a tap answers 0.4 s later, like GET /api/shots/<id>
struct FakeShot { uint32_t id; const char *file; const char *name; int ago_min; int dur10; };
static const FakeShot FAKE_SHOTS[] = {
  {304, "shot_341.json", "LMD Single v1.1 (community)", 40, 327},
  {303, "shot_340.json", "LMD Single v1.1 (community)", 3 * 60 + 20, 381},
  {302, "shot_298.json", "Leva 9 SINGLE v0.9 (community)", 21 * 60, 105},
  {301, "shot_297.json", "Leva 9 SINGLE v0.9 (community)", 26 * 60, 214},
  {300, "shot_297.json", "Leva 9 SINGLE v0.9 (community)", 28 * 60, 230},
  {299, "shot_297.json", "Default", 45 * 60, 280},
  {298, "shot_298.json", "Leva 9 SINGLE v0.9 (community)", 50 * 60, 73},
  {297, "shot_297.json", "Leva 9 SINGLE v0.9 (community)", 50 * 60 + 11, 214},
  {296, "shot_340.json", "LMD Single v1.1 (community)", 74 * 60, 381},
  {295, "shot_340.json", "LMD Single v1.1 (community)", 75 * 60, 381},
  {294, "shot_297.json", "Leva 9 SINGLE v0.9 (community)", 98 * 60, 255},
  {293, NULL, "Leva 9 SINGLE v0.9 (community)", 99 * 60, 240},    // no file: "Not available"
};
static const int FAKE_N = (int)(sizeof FAKE_SHOTS / sizeof FAKE_SHOTS[0]);
static time_t g_fake_t0 = 0;
static void fake_index(void) {
  static WsShotIndex ix;
  memset(&ix, 0, sizeof ix);
  g_fake_t0 = time(NULL);
  for (int i = 0; i < FAKE_N && i < BD_HISTORY_MAX; i++) {
    WsShotRec *r = &ix.r[ix.n++];
    r->id = FAKE_SHOTS[i].id;
    r->dur10 = (uint32_t)FAKE_SHOTS[i].dur10;
    r->ts = (uint32_t)(g_fake_t0 - FAKE_SHOTS[i].ago_min * 60);
    snprintf(r->name, sizeof r->name, "%s", FAKE_SHOTS[i].name);
    r->name_hash = shot_name_hash(r->name, strlen(r->name));
  }
  cs_take_index(&ix);
}
static uint32_t g_hist_id = 0, g_hist_due = 0;
static void on_hist(uint32_t id) {
  printf("machine <- GET /api/shots/%u (shot history)\n", (unsigned)id);
  g_hist_id = id;
  g_hist_due = SDL_GetTicks() + 400;
}
static bool fake_load(int i, bd_shot *t, bd_profile_shape *sh) {
  if (!FAKE_SHOTS[i].file) return false;
  char *js = read_file(FAKE_SHOTS[i].file, NULL);
  const bool ok = js && parse_shot(js, t);
  if (ok && sh && !parse_profile_shape(js, sh)) memset(sh, 0, sizeof *sh);
  free(js);
  if (!ok) return false;
  t->id = (int)FAKE_SHOTS[i].id;
  t->ts = (uint32_t)(g_fake_t0 - FAKE_SHOTS[i].ago_min * 60);
  t->dur10 = FAKE_SHOTS[i].dur10;
  snprintf(t->profile, sizeof t->profile, "%s", FAKE_SHOTS[i].name);
  return true;
}
static void hist_step(uint32_t now) {
  if (!g_hist_due || now < g_hist_due) return;
  g_hist_due = 0;
  static bd_shot s, p;
  static bd_profile_shape sh;
  int at = -1;
  for (int i = 0; i < FAKE_N; i++) if (FAKE_SHOTS[i].id == g_hist_id) at = i;
  if (at < 0 || !fake_load(at, &s, &sh)) { printf("machine: shot %u not available\n", (unsigned)g_hist_id); cs_hist_loaded(4, NULL, NULL, NULL, 0); return; }
  int pst = 3;                                    // PREV_MISSING
  for (int i = at + 1; i < FAKE_N; i++)
    if (!strcmp(FAKE_SHOTS[i].name, FAKE_SHOTS[at].name)) { pst = fake_load(i, &p, NULL) ? 2 : 4; break; }
  cs_hist_loaded(2 /* PREV_READY */, &s, &sh, &p, pst);
}

static void prev_step(uint32_t now) {
  if (!g_prev_due || now < g_prev_due) return;
  g_prev_due = 0;
  static bd_shot t;
  char *js = read_file("shot_340.json", NULL);   // the stored shot before the recorded one (same profile)
  if (js && parse_shot(js, &t)) cs_prev_loaded(2 /* PREV_READY */, &t);
  else cs_prev_loaded(3 /* PREV_MISSING */, NULL);
  free(js);
}

// ---- fake Wi-Fi: a scan answers after 1.2 s; a join succeeds after 2 s unless the password starts with "wrong"
static char g_ssid[33] = "Home Wi-Fi";
static char g_mhost[64] = "gaggiuino.local";   // machine address set on the screen
static bool g_no_cfg = false;                   // no Wi-Fi set up at all
static bool g_wifi_auth = false;                // Wi-Fi refuses the password (with g_nowifi)
static bool host_ok(void) {                     // the fake machine answers only at its real name / address
  return !strcmp(g_mhost, "gaggiuino.local") || !strcmp(g_mhost, "gaggiuino") || !strcmp(g_mhost, "192.168.1.50");
}
static void on_mhost(const char *h) { snprintf(g_mhost, sizeof g_mhost, "%s", h); printf("screen: machine address \"%s\" (saved)%s\n", h, host_ok() ? "" : ", nothing answers there"); }
static void on_retry(void) { printf("screen: Wi-Fi try again\n"); }
static uint32_t g_scan_due = 0, g_join_due = 0;
static char g_join_ssid[33];
static bool g_join_ok = false;
static void on_wifi_scan(void) { printf("screen: Wi-Fi scan\n"); g_scan_due = SDL_GetTicks() + 1200; }
static void on_wifi_join(const char *ssid, const char *pass) {
  printf("screen: join \"%s\" (password %zu characters, not shown)\n", ssid, strlen(pass));
  snprintf(g_join_ssid, sizeof g_join_ssid, "%s", ssid);
  g_join_ok = strncmp(pass, "wrong", 5) != 0;
  g_join_due = SDL_GetTicks() + 2000;
}
static void on_forget(void) { printf("screen: forget \"%s\"\n", g_ssid); g_no_cfg = true; }
static void wifi_step(uint32_t now) {
  if (g_scan_due && now >= g_scan_due) {
    g_scan_due = 0;
    static const bd_wifi_net N[] = {{"Home Wi-Fi", -55, true}, {"Home Wi-Fi 2", -57, true}, {"Home Wi-Fi Guest", -66, false},
                                {"Hallway-2G", -72, true}, {"Neighbour-2G", -81, true}};
    cs_wifi_scan(N, 5, false);
  }
  if (g_join_due && now >= g_join_due) {
    g_join_due = 0;
    if (g_join_ok) { snprintf(g_ssid, sizeof g_ssid, "%s", g_join_ssid); g_no_cfg = false; }
    printf("screen: join %s\n", g_join_ok ? "OK" : "FAILED (back to the previous network)");
    cs_wifi_join(g_join_ok ? BD_JOIN_OK : BD_JOIN_FAILED);
  }
}

// ---- shot playback: sim/data/ws_demo_shot.bin (LMD Single, 32.7 s, stored as shot 341; length-prefixed WebSocket frames) through the real decoder ----
struct Frame { uint32_t at; std::vector<uint8_t> b; };
static std::vector<Frame> g_shot;
static size_t g_shot_i = 0;
static uint32_t g_shot_t0 = 0;
static bool g_shot_on = false;
static int g_speed = 1;                  // shot playback speed (10 for "Shot > review")

static void load_shot(void) {
  size_t n;
  uint8_t *b = (uint8_t *)read_file("ws_demo_shot.bin", &n);
  if (!b) { printf("no ws_demo_shot.bin\n"); return; }
  // timing: shot snapshots carry the shot time; sensor frames take the time of the snapshot before them; frames
  // after the last snapshot follow at 300 ms (the machine's sensor rate) so "Done" comes as on the machine
  uint32_t at = 0, first = UINT32_MAX;
  size_t last_snap = 0;
  for (size_t i = 0; i + 4 <= n;) {
    uint32_t L = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
    const uint8_t *fr = b + i + 4;
    i += 4 + L;
    char act[24];
    const uint8_t *d;
    size_t dl;
    if (!pb_envelope(fr, L, act, sizeof act, &d, &dl)) continue;
    if (!strcmp(act, "d_shot_snap")) {
      WsShot s;
      pb_shot(d, dl, &s);
      if (first == UINT32_MAX) first = s.time10;
      at = (s.time10 - first) * 100 + 500;
      last_snap = g_shot.size();
    } else if (strcmp(act, "d_sensor_snap")) continue;
    g_shot.push_back({at, std::vector<uint8_t>(fr, fr + L)});
  }
  for (size_t k = last_snap + 1; k < g_shot.size(); k++) g_shot[k].at = g_shot[k - 1].at + 300;
  while (!g_shot.empty() && g_shot.size() > last_snap + 34) g_shot.pop_back();   // keep ~10 s after the shot
  // EMU_SHOT_CUT=<s>: the shot stops early (like an empty or cut-off shot): snapshots after that time are dropped,
  // the sensor frames go on, so "Done" comes from the missing snapshots as on the machine
  if (const char *cut = getenv("EMU_SHOT_CUT")) {
    const uint32_t lim = (uint32_t)(atof(cut) * 1000) + 500;
    std::vector<Frame> keep;
    for (auto &f : g_shot) {
      char act[24]; const uint8_t *d; size_t dl;
      pb_envelope(f.b.data(), f.b.size(), act, sizeof act, &d, &dl);
      if (f.at > lim && !strcmp(act, "d_shot_snap")) continue;
      if (f.at > lim + 10000) break;
      keep.push_back(f);
    }
    g_shot.swap(keep);
    printf("shot cut at %.1f s\n", atof(cut));
  }
  const uint32_t last = g_shot.empty() ? 0 : g_shot.back().at;
  free(b);
  printf("shot recording: %zu frames, %.1f s\n", g_shot.size(), last / 1000.0);
}

static void shot_step(uint32_t now) {
  if (!g_shot_on) return;
  while (g_shot_i < g_shot.size()) {
    Frame &f = g_shot[g_shot_i];
    if ((now - g_shot_t0) * (uint32_t)g_speed < f.at) break;
    char act[24];
    const uint8_t *d;
    size_t dl;
    pb_envelope(f.b.data(), f.b.size(), act, sizeof act, &d, &dl);
    if (!strcmp(act, "d_shot_snap")) {
      WsShot s;
      pb_shot(d, dl, &s);
      cs_shot_point(&s);
      if (g_m.scales_present && g_m.scale_type != BD_SCALE_PREDICTIVE) g_m.weight = s.weight10 / 10.0f;
    } else {
      WsSensor s;
      pb_sensor(d, dl, &s);
      g_m.temperature = s.temperature;
      g_m.pressure = s.pressure;
      g_m.brew_active = s.brew_active;
    }
    g_shot_i++;
  }
  if (g_shot_i >= g_shot.size()) {
    g_shot_on = false;
    g_m.brew_active = false;
    g_m.pressure = 0;
    g_speed = 1;
    printf("shot playback done\n");
  }
}

static const int SHOT_PROFILE = 7;      // the profile the recorded shot was pulled with (EMU_SHOT_PROFILE overrides)
static void start_shot(void) {
  if (g_shot.empty() || g_shot_on) return;
  const int pid = getenv("EMU_SHOT_PROFILE") ? atoi(getenv("EMU_SHOT_PROFILE")) : SHOT_PROFILE;
  if (g_active_id != pid) printf("note: switching the fake machine to profile %d, the recording's profile\n", pid);
  if (g_active_id != pid) on_select(pid);
  g_shot_i = 0;
  g_shot_t0 = SDL_GetTicks();
  g_shot_on = true;
  g_m.weight = 0;
  printf("shot started\n");
}

// ---- window: device frame (bezel) around the 800x480 screen + a control panel (a second LVGL display) ----------
#define BEZEL 28
#define SX (20 + BEZEL)                 // screen position in the window
#define SY (20 + BEZEL)
#define PW 372                          // panel size
#define BTN_H 27                        // panel buttons: height, row pitch
#define BTN_P 30
#define PH (H + 2 * BEZEL)
#define PX (20 + W + 2 * BEZEL + 20)
#define PY 20
#define WIN_W (PX + PW + 20)
#define WIN_H (PH + 40)
static uint16_t g_pfb[PW * PH];
static uint16_t g_pbuf[PW * 60];
static lv_display_t *g_dev_disp, *g_pan_disp;
static int g_px, g_py;
static bool g_pdown;
static bool g_pdirty = true;
static bool g_heating = false;
static lv_obj_t *g_status;
static void pflush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  const uint16_t *c = (const uint16_t *)px;
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) g_pfb[y * PW + x] = *c++;
  g_pdirty = true;
  lv_display_flush_ready(d);
}
static void ptouch_cb(lv_indev_t *d, lv_indev_data_t *o) {
  (void)d;
  o->point.x = g_px;
  o->point.y = g_py;
  o->state = g_pdown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ---- actions (panel buttons, keys and the "button <name>" stdin command) ----------------------------------------
static void go_home(void) { ui_test_home(); lv_display_trigger_activity(g_dev_disp); }
static void set_heating(bool on) {
  g_heating = on;
  g_m.temperature = on ? 62.0f : 90.1f;
  g_m.warmup_remaining = on ? 240 : 0;
}
static lv_timer_t *g_release_t = NULL;     // the pressure-release sequence (t_release): one at a time
static void release_cancel(void) { if (g_release_t) { lv_timer_delete(g_release_t); g_release_t = NULL; } }
static void a_ready(void) {
  release_cancel();
  g_shot_on = false; g_m.brew_active = false; g_m.pressure = 0;
  g_offline = g_nowifi = g_no_cfg = g_wifi_auth = false;
  snprintf(g_mhost, sizeof g_mhost, "%s", "gaggiuino.local");
  g_m.steam_active = false; strcpy(g_m.mode, "HOME");
  memset(&g_st, 0, sizeof g_st);
  set_heating(false);
  go_home();
}
static void a_heating(void) { a_ready(); set_heating(true); }
static void a_shot(void) { a_ready(); g_speed = 1; start_shot(); }
static void a_review(void) { a_ready(); g_speed = 10; start_shot(); }
#ifdef EMU_WEB
static void a_idle(void) { go_home(); ui_test_idle(); }   // the demo page: the fake machine stays as its switches say
#else
static void a_idle(void) { a_ready(); ui_test_idle(); }
#endif
static void a_screen_off(void) { a_ready(); ui_test_screen_off(); }
static void a_profiles(void) { go_home(); ui_test_profiles(1, 0); }
static void a_weight(void) { go_home(); ui_test_open_editor(); }
static void a_temp(void) { go_home(); ui_test_open_temp_editor(); }
// a shot that is playing stops (as when the brew switch goes off)
static void stop_shot(void) {
  if (!g_shot_on) return;
  g_shot_on = false; g_m.brew_active = false; g_m.pressure = 0; g_speed = 1;
  printf("shot stopped\n");
}
// what the machine itself does needs it on and reachable: switched back on (the switches show it)
static void machine_up(void) { g_offline = g_nowifi = g_no_cfg = g_wifi_auth = false; }
// the steam session playing: 0 none, else its speed (1, or 10 for "Steam, fast")
static int steam_speed(void) { return g_st.on ? g_st.speed : g_m.steam_active ? 1 : 0; }
// each steam button plays at its own speed: pressed again it stops, pressed while the other one plays it starts over
// at its speed; steam replaces a shot
static void a_steam_go(int speed) {
  go_home();
  const int cur = steam_speed();
  if (cur == speed) { steam_set(false); return; }
  if (cur) steam_set(false);
  machine_up(); stop_shot(); release_cancel(); g_heating = false;
  steam_set(true, speed);
}
static void a_steam(void) { a_steam_go(1); }
static void a_steam_fast(void) { a_steam_go(10); }
static void a_menu(void) { go_home(); ui_test_menu(-1); }
static void a_display(void) { go_home(); ui_test_menu(0); }
static void a_scale_pg(void) { go_home(); ui_test_menu(1); }
static void a_wifi(void) { go_home(); ui_test_menu(2); }
static void a_about(void) { go_home(); ui_test_menu(3); }
static void a_wifi_info(void) { go_home(); ui_test_menu(4); }
static void a_password(void) { go_home(); ui_test_wifi_password("Home Wi-Fi 2"); }
static void a_offline(void) { a_ready(); g_offline = true; }
static void a_nowifi(void) { a_ready(); g_nowifi = true; }
static void a_nocfg(void) { a_ready(); g_no_cfg = true; }
static void a_wifipw(void) { a_ready(); g_nowifi = g_wifi_auth = true; }
static void a_addr(void) { go_home(); ui_test_menu(6); }
static void a_history(void) { go_home(); ui_test_menu(7); }
static uint32_t g_vnow = 1;                 // the UI's clock (bd_ui_tick), runs faster with the fast steam
static void t_skip(void) { g_vnow += 20000; }   // jump 20 s (timeouts) without waiting
static void t_scale(void) { g_m.scales_present = !g_m.scales_present; }
static void t_scale_type(void) {   // Bluetooth -> wired -> predictive -> Bluetooth
  const int t = g_m.scale_type;
  g_m.scale_type = t == BD_SCALE_BT ? BD_SCALE_WIRED : t == BD_SCALE_WIRED ? BD_SCALE_PREDICTIVE : BD_SCALE_BT;
}
static void t_water(void) { g_m.water_level = g_m.water_level > 20 ? 12 : 100; }
static void steam_stop(void) { if (g_m.steam_active || g_st.on) steam_set(false); }
// fast steam back to normal speed from where it is (the screen's clock runs 10x with it: a notice would flash past)
static void steam_normal_speed(void) {
  if (!g_st.on || g_st.speed <= 1) return;
  const uint32_t now = SDL_GetTicks();
  g_st.t0 = now - (now - g_st.t0) * (uint32_t)g_st.speed;
  g_st.speed = 1;
  printf("steam: back to normal speed\n");
}
static void t_machine(void) {                // off: whatever it was doing stops
  g_offline = !g_offline;
  if (g_offline) { stop_shot(); steam_stop(); release_cancel(); }
}
static void t_wifi(void) { g_nowifi = !g_nowifi; }
static void t_heating(void) {                // a cold boiler: no shot or steam going on
  if (!g_heating) { stop_shot(); steam_stop(); }
  set_heating(!g_heating);
}
static void t_resid(void) { g_m.pressure = g_m.pressure > 0.5f ? 0 : 1.4f; printf("machine: idle pressure %.1f bar\n", g_m.pressure); }   // pressure left in the system
static void t_notif(void) {   // as the machine sends it (d_notif): the update notice (INFO, the longest real text)
  static int k = 0;            // and test notices of the other types; the pressure release has its own button
  machine_up();
  steam_normal_speed();
  static const int T[4] = {NOTIF_INFO, NOTIF_WARN, NOTIF_SUCCESS, NOTIF_ERROR};
  static const char *const KIND[4] = {"", "warning", "success", "error"};
  static char test[48];
  const int i = k++ % 4;
  snprintf(test, sizeof test, "Emulator test %s %d", KIND[i], (k - 1) / 4 + 1);   // fills the notices list (not machine texts)
  const char *const m = i ? test : "Release c929939 now available for download!";
  printf("machine -> notification \"%s\"\n", m);
  cs_notice(T[i], m);
}

// the machine's power-on / after-flush sequence: INFO "Dropping beats in: 3 / 2 / 1"
// ~1 s apart, then INFO "Releasing pressure!"
static void t_release(void) {               // pressed again, it starts over
  static int step = 0;
  step = 0;
  machine_up();
  stop_shot(); steam_stop();                 // the machine releases pressure while idle (power-on, after a flush)
  release_cancel();
  g_release_t = lv_timer_create([](lv_timer_t *tm) {
    static const char *const M[4] = {"Dropping beats in: 3", "Dropping beats in: 2", "Dropping beats in: 1", "Releasing pressure!"};
    printf("machine -> notification \"%s\"\n", M[step]);
    cs_notice(NOTIF_INFO, M[step]);
    if (++step == 4) { lv_timer_delete(tm); g_release_t = NULL; }
  }, 1000, NULL);
  lv_timer_ready(g_release_t);
}
// ---- fake core (brewdeck_core.h): commands go to the fake machine above; the state is in core_state.cpp --------
// a save changes the stored profile (GET /api/profile/<id>, the list's details): as the machine does
static void emu_stored(int id, float grams, int deg) {
  for (int i = 0; i < g_list.count; i++) {
    if (g_list.p[i].id != id) continue;
    if (grams >= 0) g_list.p[i].weight = grams;
    if (deg > 0) g_list.p[i].temp = (float)deg;
    cs_take_profiles(&g_list);
  }
}
// the device core's rules (core_state.cpp cs_cmd_*), then the fake machine instead of the network
bd_result bd_select_profile(int id, bool save) {
  const char *name;
  const bd_result r = cs_cmd_select(id, &name);
  if (r != BD_OK) return r;
  if (getenv("EMU_IGNORE")) { printf("machine: ignores the pick (EMU_IGNORE)\n"); return BD_OK; }
  on_select(id);
  if (save) printf("machine <- save as boot profile once active: \"%s\" (c_save_act_prof)\n", name);
  return BD_OK;
}
bd_result bd_set_stop_weight(int profile_id, float grams, bool save) {
  int w10;
  const char *name;
  const bd_result r = cs_cmd_weight(profile_id, grams, &w10, &name);
  if (r != BD_OK) return r;
  if (getenv("EMU_IGNORE")) { printf("machine: ignores the weight (EMU_IGNORE)\n"); return BD_OK; }
  on_weight(name, w10);
  if (!save) printf("machine: (not saved, only for now)\n");
  else emu_stored(profile_id, grams, -1);
  return BD_OK;
}
bd_result bd_set_brew_temp(int profile_id, int deg, bool save) {
  const char *name;
  const bd_result r = cs_cmd_temp(profile_id, deg, &name);
  if (r != BD_OK) return r;
  if (getenv("EMU_IGNORE")) { printf("machine: ignores the temperature (EMU_IGNORE)\n"); return BD_OK; }
  on_temp(name, deg);
  if (!save) printf("machine: (not saved, only for now)\n");
  else emu_stored(profile_id, -1, deg);
  return BD_OK;
}
bd_result bd_flush_mode(bd_flush mode) {
  int pm;
  const bd_result r = cs_cmd_flush(mode, &pm);
  if (r != BD_OK) return r;
  if (getenv("EMU_IGNORE")) { printf("machine: ignores the flush change (EMU_IGNORE)\n"); return BD_OK; }
  on_flush(pm);
  return BD_OK;
}
bd_result bd_tare(void) {
  const bd_result r = cs_cmd_tare();
  if (r != BD_OK) return r;
  if (getenv("EMU_IGNORE")) { printf("machine: ignores the tare (EMU_IGNORE)\n"); return BD_OK; }
  on_tare();
  return r;
}
bd_result bd_load_previous_shot(const char *profile, int dur10) { on_prev(profile, dur10); return BD_OK; }
bd_result bd_load_stored_shot(uint32_t id) { on_hist(id); return BD_OK; }
void bd_wifi_scan(void) { on_wifi_scan(); }
void bd_wifi_join(const char *ssid, const char *pass) { on_wifi_join(ssid, pass); }
void bd_wifi_forget(void) { on_forget(); }
void bd_wifi_retry(void) { on_retry(); }
void bd_set_machine_address(const char *host) { on_mhost(host); }
void bd_set_backlight(int pct) { on_backlight(pct); }
const bd_core_info *bd_core(void) {
  bd_core_info *i = cs_info();
  i->uptime_s = SDL_GetTicks() / 1000;
  i->free_internal = i->free_psram = 0;                    // not measured here
  return i;
}
bool bd_local_time(int *h, int *m, int *wday) {
  const time_t t = time(NULL);
  const struct tm *lt = localtime(&t);
  if (h) *h = lt->tm_hour;
  if (m) *m = lt->tm_min;
  if (wday) *wday = lt->tm_wday;
  return true;
}
// the UI's storage: in memory (the device keeps it in flash); the screen settings start as on a new screen
static std::map<std::string, std::vector<uint8_t>> g_store;
int32_t bd_store_get_int(const char *key, int32_t def) {
  auto it = g_store.find(key);
  if (it == g_store.end() || it->second.size() != 4) return def;
  int32_t v;
  memcpy(&v, it->second.data(), 4);
  return v;
}
void bd_store_set_int(const char *key, int32_t v) { g_store[key] = std::vector<uint8_t>((uint8_t *)&v, (uint8_t *)&v + 4); }
size_t bd_store_get_blob(const char *key, void *buf, size_t cap) {
  auto it = g_store.find(key);
  if (it == g_store.end()) return 0;
  if (buf) memcpy(buf, it->second.data(), it->second.size() < cap ? it->second.size() : cap);
  return it->second.size();
}
void bd_store_set_blob(const char *key, const void *buf, size_t n) { g_store[key] = std::vector<uint8_t>((const uint8_t *)buf, (const uint8_t *)buf + n); }
void bd_store_erase(const char *key) { g_store.erase(key); }

struct Action { const char *name; void (*fn)(void); };
static const Action SCREENS[] = {
  {"Ready", a_ready}, {"Heating", a_heating}, {"Shot", a_shot},
  {"Shot > review", a_review}, {"Idle", a_idle}, {"Screen off", a_screen_off}, {"Steam", a_steam}, {"Steam, fast", a_steam_fast},
  {"Profiles", a_profiles}, {"Weight", a_weight}, {"Temperature", a_temp}, {"Menu", a_menu},
  {"Display", a_display}, {"Scale", a_scale_pg}, {"About", a_about},
  {"Wi-Fi list", a_wifi}, {"Password", a_password}, {"Wi-Fi info", a_wifi_info},
  {"Offline", a_offline}, {"No Wi-Fi", a_nowifi}, {"No Wi-Fi set", a_nocfg}, {"Wi-Fi password", a_wifipw},
  {"Machine addr.", a_addr}, {"Shot history", a_history}, {"Splash", ui_splash}, {"Stop shot", stop_shot}};
static const Action MACHINE[] = {
  {"Scale on/off", t_scale}, {"Scale type", t_scale_type}, {"Water", t_water},
  {"Machine on/off", t_machine}, {"Wi-Fi on/off", t_wifi}, {"Heat on/off", t_heating},
  {"+20 s", t_skip}, {"Notification", t_notif}, {"Pressure release", t_release}, {"Pressure left", t_resid}};

LV_FONT_DECLARE(it_400_13);
LV_FONT_DECLARE(it_600_16);
static void on_action(lv_event_t *e) {
  const Action *a = (const Action *)lv_event_get_user_data(e);
  printf("panel: %s\n", a->name);
  a->fn();
}
static lv_obj_t *panel_label(lv_obj_t *p, const char *t, const lv_font_t *f, uint32_t col) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  return l;
}
static void panel_buttons(lv_obj_t *p, const Action *a, int n, int y) {
  for (int i = 0; i < n; i++) {
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 108, BTN_H);
    lv_obj_set_pos(b, 16 + (i % 3) * 114, y + (i / 3) * BTN_P);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x232327), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A40), LV_STATE_PRESSED);
    lv_obj_set_clickable(b, true);
    lv_obj_add_event_cb(b, on_action, LV_EVENT_CLICKED, (void *)&a[i]);
    lv_obj_center(panel_label(b, a[i].name, &it_400_13, 0xE8E8EA));
  }
}
static void build_panel(void) {
  lv_obj_t *scr = lv_display_get_screen_active(g_pan_disp);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x141416), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(scr, false);
  lv_obj_t *t = panel_label(scr, "Go to screen", &it_600_16, 0x8E8E96);
  lv_obj_set_pos(t, 16, 16);
  const int ns = (int)(sizeof SCREENS / sizeof SCREENS[0]);
  panel_buttons(scr, SCREENS, ns, 42);
  const int y2 = 42 + ((ns + 2) / 3) * BTN_P + 10;
  t = panel_label(scr, "Fake machine", &it_600_16, 0x8E8E96);
  lv_obj_set_pos(t, 16, y2);
  const int nm = (int)(sizeof MACHINE / sizeof MACHINE[0]);
  panel_buttons(scr, MACHINE, nm, y2 + 26);
  g_status = panel_label(scr, "", &it_400_13, 0x8E8E96);
  lv_obj_set_pos(g_status, 16, y2 + 26 + ((nm + 2) / 3) * BTN_P + 6);
  lv_obj_set_width(g_status, PW - 32);
  lv_label_set_long_mode(g_status, LV_LABEL_LONG_MODE_WRAP);
}
static char g_status_txt[256];
static void panel_status(void) {
  char *b = g_status_txt;
  snprintf(b, sizeof g_status_txt, "Machine %s%s%s, Wi-Fi %s\nScale %s (%s), water %d %%\n%s%s, shot %s%s. Keys: S shot, I idle",
           g_offline ? "off" : "on", host_ok() ? "" : " at ", host_ok() ? "" : g_mhost,
           g_no_cfg ? "not set up" : g_nowifi ? (g_wifi_auth ? "refuses pw" : "off") : "on",
           g_m.scales_present ? "on" : "gone",
           g_m.scale_type == BD_SCALE_BT ? "Bluetooth" : g_m.scale_type == BD_SCALE_WIRED ? "wired" : "predictive",
           g_m.water_level, g_heating ? "heating" : "ready", g_m.steam_active ? (g_st.on && g_st.speed > 1 ? ", steam (10x)" : ", steam") : "",
           g_shot_on ? "playing" : "idle", g_shot_on && g_speed > 1 ? " (10x)" : "");
  if (g_status && strcmp(lv_label_get_text(g_status), b)) lv_label_set_text(g_status, b);
}
static bool run_named(const char *name) {
  if (ui_test_splash_shown()) return false;   // as on the device: the splash covers the screen until it goes or a tap
  for (const Action &a : SCREENS) if (!strcasecmp(a.name, name)) { a.fn(); return true; }
  for (const Action &a : MACHINE) if (!strcasecmp(a.name, name)) { a.fn(); return true; }
  return false;
}

#ifndef EMU_WEB
// ---- stdin commands (for scripts / checks without looking at the window), one per line:
//   key s          press a key (same as the keyboard)      tap 742 153    tap at x y (press + release)
//   drag x1 y1 x2 y2   press, move, release                save out.raw   write the frame (RGB565, sim/topng.py)
//   wait 1500      ms before the next command                quit
//   button Shot > review   press a panel button (its label)     savewin f.bmp   the whole window
//   click 1194 164 mouse click in window coordinates (screen at 48,48; panel at 896,20)
//   redraw         redraw the whole screen (a save before and after checks that partial redraws left nothing behind)
static char g_cmd_buf[512];
static char g_savewin[400] = "";
static int g_click_up = 0, g_click_x, g_click_y;   // release a "click" a few passes later         // write the whole window (BMP) after the next frame
static size_t g_cmd_n = 0;
static uint32_t g_cmd_wait = 0;
static int g_tap_phase = 0, g_tx = 0, g_ty = 0, g_tx2 = 0, g_ty2 = 0, g_tap_ms = 120;   // g_tap_ms: how long pressed
static bool cmd_line(const char *l, uint32_t now, SDL_Keycode *key) {
  int a, b, c, d;
  char f[400];
  if (sscanf(l, "key %c", (char *)f) == 1) *key = (SDL_Keycode)f[0];
  else if (sscanf(l, "hold %d %d %d", &a, &b, &c) == 3) { g_tx = g_tx2 = a; g_ty = g_ty2 = b; g_tap_ms = c; g_tap_phase = 1; }
  else if (sscanf(l, "tap %d %d", &a, &b) == 2) { g_tx = g_tx2 = a; g_ty = g_ty2 = b; g_tap_ms = 120; g_tap_phase = 1; }
  else if (sscanf(l, "drag %d %d %d %d", &a, &b, &c, &d) == 4) { g_tx = a; g_ty = b; g_tx2 = c; g_ty2 = d; g_tap_ms = 120; g_tap_phase = 1; }
  else if (sscanf(l, "wait %d", &a) == 1) g_cmd_wait = now + a;
  else if (sscanf(l, "click %d %d", &a, &b) == 2) {   // real mouse events in window coordinates (tests the routing)
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_MOUSEBUTTONDOWN; ev.button.button = SDL_BUTTON_LEFT; ev.button.x = a; ev.button.y = b;
    SDL_PushEvent(&ev);
    g_click_up = 1; g_click_x = a; g_click_y = b;
  }
  else if (!strncmp(l, "button ", 7)) { if (!run_named(l + 7)) printf("no button \"%s\"\n", l + 7); }
  else if (!strncmp(l, "savewin ", 8) && sscanf(l, "savewin %399s", f) == 1) { snprintf(g_savewin, sizeof g_savewin, "%s", f); g_dirty = true; }
  else if (sscanf(l, "save %399s", f) == 1) {
    FILE *o = fopen(f, "wb");
    if (o) { fwrite(g_fb, 2, W * H, o); fclose(o); printf("saved %s\n", f); }
  } else if (sscanf(l, "savewin %399s", f) == 1) {
    snprintf(g_savewin, sizeof g_savewin, "%s", f);
  } else if (!strncmp(l, "redraw", 6)) {
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
  } else if (!strncmp(l, "quit", 4)) return false;
  fflush(stdout);
  return true;
}
// tap/drag by time (LVGL reads touch every 30 ms): ~100 ms pressed (moving for a drag), then ~60 ms released
static uint32_t g_tap_t0 = 0;
static void tap_step(void) {
  const uint32_t now = SDL_GetTicks();
  if (g_tap_phase == 1) { g_tap_t0 = now; g_mx = g_tx; g_my = g_ty; g_mdown = true; g_tap_phase = 2; return; }
  const uint32_t t = now - g_tap_t0;
  if (g_tap_phase == 2) {
    const int k = t > 100 ? 100 : (int)t;
    g_mx = g_tx + (g_tx2 - g_tx) * k / 100;
    g_my = g_ty + (g_ty2 - g_ty) * k / 100;
    if (t >= (uint32_t)g_tap_ms) { g_mdown = false; g_tap_phase = 3; }
  } else if (g_tap_phase == 3 && t >= (uint32_t)g_tap_ms + 60) {
    g_tap_phase = 0;
  }
}

// device body: rounded black bezel with a thin edge, like the Sunton panel's frame
static void fill_round(SDL_Renderer *r, int x, int y, int w, int h, int rad) {
  for (int j = 0; j < h; j++) {
    int dx = 0;
    const int dy = j < rad ? rad - j : j >= h - rad ? j - (h - rad - 1) : 0;
    if (dy) dx = rad - (int)sqrtf((float)(rad * rad - dy * dy));
    SDL_RenderDrawLine(r, x + dx, y + j, x + w - 1 - dx, y + j);
  }
}
static void draw_bezel(SDL_Renderer *r) {
  SDL_SetRenderDrawColor(r, 0x3C, 0x3C, 0x42, 255);
  fill_round(r, 19, 19, W + 2 * BEZEL + 2, H + 2 * BEZEL + 2, 22);
  SDL_SetRenderDrawColor(r, 0x08, 0x08, 0x09, 255);
  fill_round(r, 20, 20, W + 2 * BEZEL, H + 2 * BEZEL, 21);
  SDL_SetRenderDrawColor(r, 0x1A, 0x1A, 0x1D, 255);   // thin line around the glass
  SDL_Rect g = {SX - 2, SY - 2, W + 4, H + 4};
  SDL_RenderDrawRect(r, &g);
  SDL_SetRenderDrawColor(r, 0x14, 0x14, 0x16, 255);   // panel card
  fill_round(r, PX, PY, PW, PH, 16);
}

#endif  // !EMU_WEB

// ---- shared by the desktop window and the web page: the fake machine set up, one loop pass ----------------------
static void emu_setup(void) {
  cs_core_info("emulator " __DATE__, BD_RESET_POWER_ON);
  fake_index();
  memset(&g_m, 0, sizeof g_m);
  g_m.temperature = 90.1f;
  g_m.steam_set = 155;                                       // d_settings boiler.steamSetPoint (155 on the machine the recording came from)
  g_m.target = 90;
  g_m.water_level = 100;
  g_m.scales_present = true;
  g_m.scale_type = BD_SCALE_BT;
  strcpy(g_m.mode, "HOME");
  strcpy(g_m.machine, "gaggiuino");
  strcpy(g_m.core_ver, "c929939");                           // REST coreVersion / coreType on the machine
  strcpy(g_m.core_type, "STM32U585-PCB-PCA");
  strcpy(g_m.front_ver, "fake-front");                       // d_settings versions (real values not seen yet)
  strcpy(g_m.static_ver, "fake-static");
  load_profiles();
  load_shot();
  load_steam();
  if (g_pan_disp) build_panel();                            // the desktop window's control panel (the web page has HTML buttons)
}
static uint32_t last_live = 0, last_tick = 0, last_info = 0;
static void emu_step(uint32_t now) {
  if (!last_tick) last_tick = now;
  const int sp = g_st.on && g_st.speed > 1 ? g_st.speed : 1;   // fast steam: the screen's clock runs 10x too
  g_vnow += (now - last_tick) * sp;                          // the UI's clock (fast steam: 10x)
  last_tick = now;

  shot_step(now);
  prev_step(now);
  hist_step(now);
  if (g_autoflush_ms) {                                    // fake timed flush
    const uint32_t t = now - g_autoflush_ms;
    g_m.pressure = t < 5000 ? 1.4f : 0;
    if (t >= 5000) { g_autoflush_ms = 0; strcpy(g_m.mode, "HOME"); printf("machine: auto flush done\n"); }
  }
  wifi_step(now);
  if (g_heating) {                                         // warm up slowly
    g_m.temperature += (g_m.target - g_m.temperature) * 0.0015f;
    if (g_m.warmup_remaining > 0 && now % 1000 < 20) g_m.warmup_remaining--;
    if (g_m.temperature > 89.6f) { g_heating = false; g_m.warmup_remaining = 0; printf("ready\n"); }
  }
  {                                                        // steam: boiler heats to the set point, back after
    static uint32_t st_t = 0;
    if (now - st_t >= 100 / (uint32_t)sp) {
      st_t = now;
      steam_step(now);                                     // recorded steam session / pressure fading after it
      if (!g_st.on && !g_heating && !g_shot_on) g_m.temperature += (g_m.target - g_m.temperature) * 0.006f;
    }
  }
  static bool first_live = true;                           // the first pass reports at once: the screen starts online,
  if (first_live || now - last_live >= 150 / (uint32_t)sp) {   // not on "Joining Wi-Fi"; then as often as in its time
    first_live = false;
    last_live = now;
    g_link = g_nowifi || g_no_cfg ? BD_LINK_CONNECTING : g_offline || !host_ok() ? BD_LINK_NO_MACHINE : BD_LINK_ONLINE;
    LiveData d = g_m;
    if (!g_shot_on && !g_heating) d.temperature = g_m.temperature + 0.1f * sinf(now / 4000.0f);
    // the machine's readiness (dashboardReadinessState, names of its web UI): a guess of when it switches
    d.readiness = g_m.steam_active ? 5 : g_m.warmup_remaining > 0 ? 1 : d.temperature < d.target - 3.0f ? 2 : 3;
    cs_take_live(&d, g_link);
  }
  if (now - last_info >= 1000) {
    last_info = now;
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    (void)lt;
    panel_status();
    bd_network *n = cs_network();
    snprintf(n->target, sizeof n->target, "%s", g_no_cfg ? "" : g_ssid);
    snprintf(n->machine_addr, sizeof n->machine_addr, "%s", g_mhost);
    cs_set_why(g_nowifi ? (g_wifi_auth ? BD_WHY_WIFI_AUTH : BD_WHY_WIFI_NO_AP) : !host_ok() ? BD_WHY_NAME_UNKNOWN
               : g_offline ? BD_WHY_NO_REPLY : BD_WHY_NONE);
    snprintf(n->why_ip, sizeof n->why_ip, "192.168.1.50");
    snprintf(n->screen_ip, sizeof n->screen_ip, "%s", g_nowifi || g_no_cfg ? "" : "192.168.1.42");
    snprintf(n->machine_where, sizeof n->machine_where, "%s", host_ok() ? "192.168.1.50" : g_mhost);
    const bool wifi = !(g_nowifi || g_no_cfg || g_join_due);
    snprintf(n->ssid, sizeof n->ssid, "%s", wifi ? g_ssid : "");
    n->rssi = wifi ? -55 : 0;
    n->channel = wifi ? 6 : 0;
    snprintf(n->ap, sizeof n->ap, "%s", wifi ? "02:00:00:00:00:01" : "");
    cs_network_done();
  }
  g_flush_px = 0;
  cs_tick(g_vnow);
  bd_ui_tick(g_vnow);                                        // our UI (ui_bridge.cpp) draws
  { static const bool fl = getenv("EMU_FLUSHLOG") != NULL;
    if (fl && g_flush_px >= 100000) printf("flush %u px at %u ms\n", (unsigned)g_flush_px, (unsigned)SDL_GetTicks()); }
}

#ifndef EMU_WEB
// ---- main -------------------------------------------------------------------------------------
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) { printf("SDL: %s\n", SDL_GetError()); return 1; }
  SDL_Window *win = SDL_CreateWindow("Brewdeck emulator (fake machine)", SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, W, H);
  SDL_Texture *ptex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, PW, PH);

  bd_ui_init();                                              // our UI: LVGL on the board layer above, the screens
  ui_test_splash_end();                                      // scripted runs start on Ready (the setting stays On)
  g_dev_disp = lv_display_get_default();
  g_pan_disp = lv_display_create(PW, PH);                    // the control panel: a second LVGL display
  lv_display_set_color_format(g_pan_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(g_pan_disp, g_pbuf, NULL, sizeof g_pbuf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(g_pan_disp, pflush_cb);
  lv_indev_t *pin = lv_indev_create();
  lv_indev_set_type(pin, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(pin, ptouch_cb);
  lv_indev_set_display(pin, g_pan_disp);
  lv_display_set_default(g_dev_disp);

  emu_setup();
  lv_display_set_default(g_dev_disp);
  printf("%s", HELP);

  fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
  for (bool run = true; run;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) run = false;
      else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION) {
        // window -> screen or panel coordinates; a press stays with the area it started in
        static int owner = 0;                        // 0 none, 1 screen, 2 panel
        const int x = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
        const int y = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
        if (e.type == SDL_MOUSEBUTTONDOWN) {
          owner = x >= SX && x < SX + W && y >= SY && y < SY + H ? 1 : x >= PX && x < PX + PW && y >= PY && y < PY + PH ? 2 : 0;
        }
        if (owner == 1 || (owner == 0 && !g_mdown)) {
          g_mx = x - SX < 0 ? 0 : x - SX >= W ? W - 1 : x - SX;
          g_my = y - SY < 0 ? 0 : y - SY >= H ? H - 1 : y - SY;
        }
        if (owner == 2) { g_px = x - PX; g_py = y - PY; }
        if (e.type == SDL_MOUSEBUTTONDOWN) { if (owner == 1) g_mdown = true; if (owner == 2) g_pdown = true; }
        if (e.type == SDL_MOUSEBUTTONUP) { g_mdown = false; g_pdown = false; owner = 0; }
      } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        switch (e.key.keysym.sym) {
          case SDLK_ESCAPE: case SDLK_q: run = false; break;
          case SDLK_s: start_shot(); break;
          case SDLK_b: g_m.scales_present = !g_m.scales_present; printf("scale %s\n", g_m.scales_present ? "connected" : "gone"); break;
          case SDLK_p:
            t_scale_type();
            printf("scale type %s\n", g_m.scale_type == BD_SCALE_BT ? "Bluetooth"
                                      : g_m.scale_type == BD_SCALE_WIRED ? "wired" : "predictive");
            break;
          case SDLK_h: t_heating(); printf("%s\n", g_heating ? "heating" : "ready"); break;
          case SDLK_l: g_m.water_level = g_m.water_level > 20 ? 12 : 100; printf("water %d %%\n", g_m.water_level); break;
          case SDLK_o: g_offline = !g_offline; printf("machine %s\n", g_offline ? "OFFLINE" : "back"); break;
          case SDLK_n: g_nowifi = !g_nowifi; printf("Wi-Fi %s\n", g_nowifi ? "GONE" : "back"); break;
          case SDLK_i: ui_test_idle(); printf("idle now\n"); break;
          case SDLK_m: steam_set(!g_m.steam_active); break;
          case SDLK_r: load_profiles(); break;
        }
      }
    }
    const uint32_t now = SDL_GetTicks();
    if (g_click_up && ++g_click_up > 4) {
      SDL_Event ev;
      memset(&ev, 0, sizeof ev);
      ev.type = SDL_MOUSEBUTTONUP; ev.button.button = SDL_BUTTON_LEFT; ev.button.x = g_click_x; ev.button.y = g_click_y;
      SDL_PushEvent(&ev);
      g_click_up = 0;
    }
    if (g_tap_phase) tap_step();
    else if (now >= g_cmd_wait) {
      char c;
      while (read(0, &c, 1) == 1) {
        if (c != '\n') { if (g_cmd_n + 1 < sizeof g_cmd_buf) g_cmd_buf[g_cmd_n++] = c; continue; }
        g_cmd_buf[g_cmd_n] = 0;
        g_cmd_n = 0;
        SDL_Keycode k = 0;
        if (!cmd_line(g_cmd_buf, now, &k)) run = false;
        if (k) { SDL_Event ke; memset(&ke, 0, sizeof ke); ke.type = SDL_KEYDOWN; ke.key.keysym.sym = k; SDL_PushEvent(&ke); }
        break;                                       // one command per loop pass
      }
    }
    emu_step(now);

    if (g_dirty || g_pdirty) {
      g_dirty = g_pdirty = false;
      SDL_UpdateTexture(tex, NULL, g_fb, W * 2);
      SDL_UpdateTexture(ptex, NULL, g_pfb, PW * 2);
      const Uint8 m = (Uint8)(g_backlight * 255 / 100);   // backlight shown as darkening
      SDL_SetTextureColorMod(tex, m, m, m);
      SDL_SetRenderDrawColor(ren, 0x2A, 0x2A, 0x2E, 255);  // desk
      SDL_RenderClear(ren);
      draw_bezel(ren);
      SDL_Rect sr = {SX, SY, W, H}, pr = {PX, PY, PW, PH};
      SDL_RenderCopy(ren, tex, NULL, &sr);
      SDL_RenderCopy(ren, ptex, NULL, &pr);
      if (g_savewin[0]) {
        int ow, oh;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
        SDL_SaveBMP(sf, g_savewin);
        SDL_FreeSurface(sf);
        printf("saved %s\n", g_savewin);
        g_savewin[0] = 0;
      }
      SDL_RenderPresent(ren);
    } else {
      SDL_Delay(5);
    }
  }
  SDL_Quit();
  return 0;
}
#endif  // !EMU_WEB
