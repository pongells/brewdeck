// See core_state.h.
#include "core_state.h"
#include <stddef.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "net.h"
#include "stamp.h"

// stored shots, profile shapes, Wi-Fi networks and the enum values are the API's own (live.h): handed over as they are

static bd_machine g_m;
static bd_network g_net;
static bd_profiles g_pl;
static bd_profile_shape g_shape;
static bd_history *g_hist;
static bd_shot *g_live;
static bd_core_info g_info;
static uint32_t g_seq_m, g_seq_pl, g_seq_sh, g_seq_hi, g_seq_net;

const bd_machine *bd_machine_state(void) { return &g_m; }
const bd_network *bd_network_state(void) { return &g_net; }
const bd_profiles *bd_profile_list(void) { return &g_pl; }
const bd_profile_shape *bd_active_shape(void) { return &g_shape; }
const bd_history *bd_shot_history(void) {
  if (!g_hist) {
    g_hist = (bd_history *)bd_alloc(sizeof(bd_history), BD_MEM_PSRAM);
    if (g_hist) memset(g_hist, 0, sizeof *g_hist);
  }
  return g_hist;
}
const bd_shot *bd_live_shot(void) { return g_live; }
uint32_t bd_seq_machine(void) { return g_seq_m; }
uint32_t bd_seq_profiles(void) { return g_seq_pl; }
uint32_t bd_seq_shape(void) { return g_seq_sh; }
uint32_t bd_seq_history(void) { return g_seq_hi; }
uint32_t bd_seq_network(void) { return g_seq_net; }
bd_core_info *cs_info(void) { return &g_info; }

static void cpy(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s ? s : ""); }
const char *cs_profile_name(int id) {
  for (int i = 0; i < g_pl.count; i++) if (g_pl.p[i].id == id) return g_pl.p[i].name;
  return id && id == g_m.profile_id ? g_m.profile_name : NULL;
}

// ---- shots: start, end, the live shot ------------------------------------------------------------------------------
// A shot starts at its first point or when the machine reports brewing, whichever comes first. It ends 0.3 s after
// the machine stopped brewing with no new point (the last points come just after the flag drops), or when no new
// point came for 3 s; 15 s while the machine still reports brewing (a WebSocket reconnect mid-shot takes over 5 s)
// (cs_tick).
static const uint32_t SHOT_QUIET_MS = 300, SHOT_NO_DATA_MS = 3000, SHOT_STALL_MS = 15000;
// a point after the end carries the shot on while the machine reports brewing, or this soon after the end (a late
// point, a gap); later it is a stray resend of the finished shot (dropped) unless it starts near 0 s (a new shot)
static const uint32_t SHOT_RESUME_MS = 3000;
static const uint16_t SHOT_NEW_T10 = 20;
static bool g_shot_on = false;
static uint32_t g_now = 0;                       // the last cs_tick's time (this file has no clock of its own)
static uint32_t g_shot_end_ms = 0;               // when the last shot ended (stamp_nz), 0 before any
static bool g_shot_brewed = false;                // the machine reported brewing during this shot
static uint32_t g_shot_rx = 0;                    // points received (the stored shot stops growing at BD_SHOT_MAX)
static uint32_t g_shot_seen_rx = 0, g_shot_seen_t = 0;
static void shot_event(bd_event_type t) {
  bd_event e;
  memset(&e, 0, sizeof e);
  e.type = t;
  bd_ui_event(&e);
}
static void shot_begin(void) {
  g_shot_on = true;
  g_shot_brewed = g_m.brewing;
  g_shot_rx = g_shot_seen_rx = 0;
  g_shot_seen_t = 0;
  if (!g_live) g_live = (bd_shot *)bd_alloc(sizeof(bd_shot), BD_MEM_PSRAM);
  if (g_live) {
    memset(g_live, 0, sizeof *g_live);
    g_live->ts = (uint32_t)time(NULL);            // small while the clock is not set (as the machine's own)
    cpy(g_live->profile, sizeof g_live->profile, g_m.running.name[0] ? g_m.running.name : g_m.profile_name);
  }
  shot_event(BD_EV_SHOT_START);
  bd_ui_wake();
}
static void shot_end(void) {
  g_shot_on = false;
  g_shot_end_ms = stamp_nz(g_now);
  shot_event(BD_EV_SHOT_END);
}

void cs_take_live(const LiveData *d, int link) {
  const bool was_brewing = g_m.brewing, was_steaming = g_m.steaming;
  char was_mode[sizeof g_m.mode];
  cpy(was_mode, sizeof was_mode, g_m.mode);
  g_m.link = (bd_link_state)link;
  g_m.temperature = d->temperature;
  g_m.pressure = d->pressure;
  g_m.weight = d->weight;
  g_m.water_level = d->water_level;
  g_m.warmup_remaining = d->warmup_remaining;
  g_m.readiness = (bd_readiness)d->readiness;
  g_m.brewing = d->brew_active;
  g_m.steaming = d->steam_active;
  g_m.hot_water = d->hot_water_active;
  cpy(g_m.mode, sizeof g_m.mode, d->mode);
  g_m.target_temp = d->target;
  g_m.scale_present = d->scales_present;
  g_m.scale_type = (bd_scale_type)d->scale_type;
  g_m.steam_set = d->steam_set;
  cpy(g_m.profile_name, sizeof g_m.profile_name, d->profile);
  for (int i = 0; i < g_pl.count; i++) if (!strcmp(g_pl.p[i].name, d->profile)) g_m.profile_id = g_pl.p[i].id;
  cpy(g_m.running.name, sizeof g_m.running.name, d->act_profile);
  g_m.running.stop_weight = d->act_weight;
  g_m.running.brew_temp = d->act_temp;
  cpy(g_m.core_version, sizeof g_m.core_version, d->core_ver);
  cpy(g_m.core_board, sizeof g_m.core_board, d->core_type);
  cpy(g_m.screen_version, sizeof g_m.screen_version, d->front_ver);
  cpy(g_m.web_version, sizeof g_m.web_version, d->static_ver);
  g_seq_m++;
  if (g_m.brewing && !was_brewing && !g_shot_on) shot_begin();
  if (g_m.brewing && g_shot_on) g_shot_brewed = true;
  if (!g_m.brewing && was_brewing && g_shot_on && !g_shot_rx) shot_end();   // no points at all: nothing to wait for
  const bool flushing = !strncmp(g_m.mode, "FLUSH", 5), was_flushing = !strncmp(was_mode, "FLUSH", 5);
  if ((g_m.steaming && !was_steaming) || (flushing && !was_flushing)) bd_ui_wake();
}
void cs_set_why(int why) { g_m.why = (bd_why)why; }

static uint32_t g_hist_hash[BD_HISTORY_MAX];      // name_hash of each history entry (the machine's index has it)
static bool g_hist_ok = false;
static void last_used_all(void) {                 // newest stored shot of each profile: the index is newest first
  for (int i = 0; i < g_pl.count; i++) {
    bd_profile *p = &g_pl.p[i];
    p->last_used = 0;
    if (!g_hist_ok || !g_hist) continue;
    const uint32_t h = shot_name_hash(p->name, strlen(p->name));
    for (int k = 0; k < g_hist->n; k++)
      if (g_hist_hash[k] == h || !strncmp(g_hist->e[k].profile, p->name, sizeof g_hist->e[k].profile - 1)) {
        p->last_used = g_hist->e[k].ts;
        break;
      }
  }
}
void cs_take_profiles(const ProfileList *l) {
  g_pl.count = l->count < BD_MAX_PROFILES ? l->count : BD_MAX_PROFILES;
  for (int i = 0; i < g_pl.count; i++) {
    bd_profile *p = &g_pl.p[i];
    const ProfileInfo *q = &l->p[i];
    p->id = q->id;
    cpy(p->name, sizeof p->name, q->name);
    p->active = q->selected;
    p->detail = q->detail;
    p->phases = q->phases;
    p->stop_weight = q->weight;
    p->brew_temp = q->temp;
    if (q->selected && g_m.profile_id != q->id) { g_m.profile_id = q->id; g_seq_m++; }
  }
  last_used_all();
  g_seq_pl++;
}
void cs_take_shape(const bd_profile_shape *s) { memcpy(&g_shape, s, sizeof g_shape); g_seq_sh++; }
void cs_take_index(const WsShotIndex *ix) {
  bd_history *h = (bd_history *)bd_shot_history();
  if (!h) return;
  h->n = ix->n < BD_HISTORY_MAX ? ix->n : BD_HISTORY_MAX;
  for (int i = 0; i < h->n; i++) {
    h->e[i].id = ix->r[i].id;
    h->e[i].dur10 = ix->r[i].dur10;
    h->e[i].ts = ix->r[i].ts;
    cpy(h->e[i].profile, sizeof h->e[i].profile, ix->r[i].name);
    g_hist_hash[i] = ix->r[i].name_hash;
  }
  g_hist_ok = true;
  last_used_all();
  g_seq_pl++;                       // the profiles' last_used changed with it
  g_seq_hi++;
}
bd_network *cs_network(void) { return &g_net; }
void cs_network_done(void) { g_seq_net++; }
void cs_core_info(const char *build, bd_reset reset) {
  g_info.api_version = BD_API_VERSION;
  cpy(g_info.build, sizeof g_info.build, build);
  g_info.reset = reset;
}

void cs_shot_point(const WsShot *s) {
  bd_shot_point p;
  pb_shot_point(s, &p);
  const bool back = g_live && g_live->n && p.t + 5 < g_live->pt[g_live->n - 1].t;   // time went back: a new shot
  if (g_shot_on && back) { shot_end(); shot_begin(); }
  else if (!g_shot_on) {
    const bool more = g_live && g_live->n && !back;
    if (more && (g_m.brewing || (g_shot_end_ms && g_now - g_shot_end_ms <= SHOT_RESUME_MS)))
      g_shot_on = true;                                   // carry on with it
    else if (more && p.t > SHOT_NEW_T10) return;          // a stray point of the finished shot: not a shot
    else shot_begin();
  }
  g_shot_rx++;
  if (g_live) {                     // the live shot as recorded on this screen
    if (g_live->n < BD_SHOT_MAX) g_live->pt[g_live->n++] = p;
    g_live->dur10 = p.t;
  }
  bd_event e;
  e.type = BD_EV_SHOT_POINT;
  e.u.point = &p;
  bd_ui_event(&e);
}
// The machine's notifications. Three are recognised here by their text, so a UI never has to parse them (matched by
// the machine's wording: a firmware update that rewords them turns them into ordinary notices):
//   "Fill the water tank!"          pushed ~1.4/s while the tank is low: dropped (water_level says it)
//   "Dropping beats in: N", "Releasing pressure!"   -> BD_EV_PRESSURE_RELEASE (count N, -1 releasing)
//   "Release <name> now available for download!"   -> bd_machine.update = <name>, a NOTICE with update = true
void cs_notice(int type, const char *msg) {
  if (!msg || !msg[0] || strcasestr(msg, "water tank")) return;
  bd_event e;
  const char *db = strcasestr(msg, "dropping beats");
  if (db || strcasestr(msg, "releasing pressure")) {
    int n = 0;
    const char *c = db ? strchr(db, ':') : NULL;
    if (c) sscanf(c + 1, "%d", &n);
    e.type = BD_EV_PRESSURE_RELEASE;
    e.u.release.count = db ? (n < 0 ? 0 : n) : -1;
    bd_ui_event(&e);
    bd_ui_wake();
    return;
  }
  e.type = BD_EV_NOTICE;
  e.u.notice.type = (bd_notice_type)type;
  e.u.notice.text = msg;
  e.u.notice.update = strcasestr(msg, "available for download") != NULL;
  if (e.u.notice.update) {
    char rel[32] = "";
    if (sscanf(msg, "Release %31s", rel) != 1) rel[0] = 0;
    snprintf(g_m.update, sizeof g_m.update, "%s", rel);
    g_m.update_available = true;
    g_seq_m++;
  }
  bd_ui_event(&e);
  if (!e.u.notice.update) bd_ui_wake();           // the update notice repeats every 2 min: it does not wake the screen
}
void cs_prev_loaded(int state, const bd_shot *t) {
  bd_event e;
  memset(&e, 0, sizeof e);
  e.type = BD_EV_SHOT_LOADED;
  e.u.loaded.state = (bd_load)state;
  e.u.loaded.shot = t;
  bd_ui_event(&e);
}
void cs_hist_loaded(int state, const bd_shot *s, const bd_profile_shape *sh, const bd_shot *prev, int prev_state) {
  bd_event e;
  e.type = BD_EV_SHOT_LOADED;
  e.u.loaded.history = true;
  e.u.loaded.state = (bd_load)state;
  e.u.loaded.shot = s;
  e.u.loaded.shape = sh;
  e.u.loaded.previous = prev;
  e.u.loaded.previous_state = (bd_load)prev_state;
  bd_ui_event(&e);
}
void cs_wifi_scan(const bd_wifi_net *n, int count, bool scanning) {
  bd_event e;
  e.type = BD_EV_WIFI_SCAN;
  e.u.scan.nets = n;
  e.u.scan.n = count;
  e.u.scan.scanning = scanning;
  bd_ui_event(&e);
}
void cs_wifi_join(int state) {
  bd_event e;
  e.type = BD_EV_WIFI_JOIN;
  e.u.join.state = (bd_join)state;
  bd_ui_event(&e);
}

// ---- commands: checks before sending ------------------------------------------------------------------------------
static bool idle_home(void) {
  return !g_m.brewing && !g_m.steaming && !g_m.hot_water && !strcmp(g_m.mode, "HOME");
}
bd_result cs_check_idle(void) {
  if (g_m.link != BD_LINK_ONLINE) return BD_ERR_OFFLINE;
  return idle_home() ? BD_OK : BD_ERR_BUSY;
}
bd_result cs_check_active(int profile_id) {       // as the WebSocket code checks before sending: the running profile's name
  const char *name = cs_profile_name(profile_id);
  if (!name) return BD_ERR_UNKNOWN_ID;
  return g_m.running.name[0] && strcmp(g_m.running.name, name) ? BD_ERR_PROFILE_CHANGED : BD_OK;
}
bd_result cs_check_flush(bd_flush mode) {
  if (g_m.link != BD_LINK_ONLINE) return BD_ERR_OFFLINE;
  if (mode == BD_FLUSH_OFF) return !strncmp(g_m.mode, "FLUSH", 5) || idle_home() ? BD_OK : BD_ERR_BUSY;
  return idle_home() ? BD_OK : BD_ERR_BUSY;
}

// ---- commands: the whole rule set of each, shared by the device core (core.cpp) and the emulator's fake core ------
// BD_OK = allowed: the answer is now watched and the caller sends it; anything else = refused, nothing to send
bd_result cs_cmd_select(int id, const char **name) {
  const bd_result r = cs_check_idle();
  if (r != BD_OK) return r;
  const char *n = cs_profile_name(id);
  if (!n) return BD_ERR_UNKNOWN_ID;
  if (cs_select_pending()) return BD_ERR_PENDING;
  cs_watch_select(n);
  *name = n;
  return BD_OK;
}
bd_result cs_cmd_weight(int profile_id, float grams, int *w10, const char **name) {
  bd_result r = cs_check_idle();
  if (r != BD_OK) return r;
  if (!(grams >= 0.0f && grams <= BD_STOP_WEIGHT_MAX)) return BD_ERR_RANGE;   // 0 = no stop weight
  if ((r = cs_check_active(profile_id)) != BD_OK) return r;
  *w10 = (int)(grams * 10.0f + 0.5f);
  *name = cs_profile_name(profile_id);
  cs_watch_weight(*w10);
  return BD_OK;
}
bd_result cs_cmd_temp(int profile_id, int deg, const char **name) {
  bd_result r = cs_check_idle();
  if (r != BD_OK) return r;
  if (deg < BD_BREW_TEMP_MIN || deg > BD_BREW_TEMP_MAX) return BD_ERR_RANGE;   // 0 = heater off: never
  if ((r = cs_check_active(profile_id)) != BD_OK) return r;
  *name = cs_profile_name(profile_id);
  cs_watch_temp(deg);
  return BD_OK;
}
bd_result cs_cmd_flush(bd_flush mode, int *pb_mode) {
  const bd_result r = cs_check_flush(mode);
  if (r != BD_OK) return r;
  *pb_mode = mode == BD_FLUSH_ON ? PB_MODE_FLUSH : mode == BD_FLUSH_TIMED ? PB_MODE_FLUSH_AUTO : PB_MODE_HOME;
  cs_watch_flush(mode == BD_FLUSH_ON ? "FLUSH" : mode == BD_FLUSH_TIMED ? "FLUSH_AUTO" : "HOME");
  return BD_OK;
}
bd_result cs_cmd_tare(void) {
  const bd_result r = cs_check_idle();
  if (r != BD_OK) return r;
  if (!g_m.scale_present) return BD_ERR_NO_SCALE;
  cs_watch_tare();
  return BD_OK;
}

// ---- command answers ----------------------------------------------------------------------------------------------
static const uint32_t ANSWER_MS = 5000;           // the machine reports a change within ~1 s; 5 s = it did not take it
static const uint32_t ANSWER_PROFILE_MS = 7000;   // weight / temperature are sent 1 s after the last change
static struct { bool on; char want[48]; uint32_t t; } g_w_sel, g_w_fl;
static struct { bool on; int want; uint32_t t; } g_w_wt, g_w_tp, g_w_tare;
void cs_watch_select(const char *profile) { g_w_sel.on = true; cpy(g_w_sel.want, sizeof g_w_sel.want, profile); g_w_sel.t = 0; }
void cs_watch_flush(const char *mode) { g_w_fl.on = true; cpy(g_w_fl.want, sizeof g_w_fl.want, mode); g_w_fl.t = 0; }
void cs_watch_weight(int w10) { g_w_wt.on = true; g_w_wt.want = w10; g_w_wt.t = 0; }
void cs_watch_temp(int deg) { g_w_tp.on = true; g_w_tp.want = deg; g_w_tp.t = 0; }
void cs_watch_tare(void) { g_w_tare.on = true; g_w_tare.t = 0; }
bool cs_select_pending(void) { return g_w_sel.on; }
void cs_answer(bd_command cmd, bd_answer a) {
  bd_event e;
  e.type = BD_EV_COMMAND;
  e.u.command.cmd = cmd;
  e.u.command.answer = a;
  bd_ui_event(&e);
}
static void watch(bool *on, const char *want, const char *now_is, uint32_t *t, uint32_t now, bd_command cmd) {
  if (!*on) return;
  if (!*t) *t = stamp_nz(now);                    // the clock starts at the first pass after the command
  if (!strcmp(now_is, want)) { *on = false; cs_answer(cmd, BD_ANSWER_OK); }
  else if (now - *t > ANSWER_MS) { *on = false; cs_answer(cmd, BD_ANSWER_NOT_TAKEN); }
}
static void watch_n(bool *on, bool done, uint32_t *t, uint32_t now, uint32_t limit, bd_command cmd) {
  if (!*on) return;
  if (!*t) *t = stamp_nz(now);
  if (done) { *on = false; cs_answer(cmd, BD_ANSWER_OK); }
  else if (now - *t > limit) { *on = false; cs_answer(cmd, BD_ANSWER_NOT_TAKEN); }
}
void cs_tick(uint32_t now) {
  g_now = now;
  watch(&g_w_sel.on, g_w_sel.want, g_m.profile_name, &g_w_sel.t, now, BD_CMD_SELECT);
  watch(&g_w_fl.on, g_w_fl.want, g_m.mode, &g_w_fl.t, now, BD_CMD_FLUSH);
  watch_n(&g_w_wt.on, (int)lroundf(g_m.running.stop_weight * 10.0f) == g_w_wt.want, &g_w_wt.t, now, ANSWER_PROFILE_MS,
          BD_CMD_STOP_WEIGHT);
  watch_n(&g_w_tp.on, (int)lroundf(g_m.running.brew_temp) == g_w_tp.want, &g_w_tp.t, now, ANSWER_PROFILE_MS,
          BD_CMD_BREW_TEMP);
  watch_n(&g_w_tare.on, fabsf(g_m.weight) < 0.3f, &g_w_tare.t, now, ANSWER_MS, BD_CMD_TARE);
  if (g_shot_on && g_shot_rx) {                   // a shot that stopped sending points: ended
    if (g_shot_rx != g_shot_seen_rx || !g_shot_seen_t) { g_shot_seen_rx = g_shot_rx; g_shot_seen_t = stamp_nz(now); }
    else if (now - g_shot_seen_t > (g_shot_brewed && !g_m.brewing ? SHOT_QUIET_MS
                                    : g_m.brewing ? SHOT_STALL_MS : SHOT_NO_DATA_MS)) shot_end();
  }
}

// ---- blank screen -------------------------------------------------------------------------------------------------
void cs_blank_screen(void) {     // black, row by row: after a reset the frame buffer can still hold the last picture
  const bd_display_info *di = bd_display();
  uint16_t *row = (uint16_t *)bd_alloc((size_t)di->width * 2, BD_MEM_ANY);
  if (!row) return;
  memset(row, 0, (size_t)di->width * 2);
  for (int y = 0; y < di->height; y++) bd_display_flush(0, y, di->width - 1, y, row);
  bd_free(row);
}
