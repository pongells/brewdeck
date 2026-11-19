// The Profiles page (one row per profile), the originals of edited values and the profile snapshots (restore panel).
#include "../ui_int.h"

// ---- profile picker (a full page) -----------------------------------------------------------------------------------
// The active profile first, then recently used (newest shot of each profile in the machine's shot index, the newest
// 50 shots), then the rest by name. A tap selects the profile with bd_select_profile (only while online and idle in
// HOME; otherwise the tap only closes the page); the header and the check mark follow once the machine reports it.
// one profile per row in columns (x on the screen): name 24..400, stop weight right at 476, temperature right at 540,
// last used right at 676, Restore / the active check 688..776. Header row at 96, the list scrolls below it.
static const int PK_HDR = 96, PK_HDR_H = 28, PK_Y = PK_HDR + PK_HDR_H, PK_RH = 56, PK_W = 752;
static const int PK_NAME_W = 376, PK_STOP_R = 476, PK_TEMP_R = 540, PK_USED_R = 676, PK_ACT_X = 688, PK_ACT_W = 88;

lv_obj_t *g_pick;                          // full-screen page
static lv_obj_t *g_pick_list;
static lv_obj_t *g_pick_sub;               // the page's subtitle ("Active, then recently used")
ProfileList g_profiles;
// profile select in flight: shown at once, until the machine reports it or the core answers that it was not taken
int g_pend_id = 0;
char g_pend_name[48];

// the machine's id of a profile named as the status names it
int prof_id(const char *name) {
  for (int i = 0; i < g_profiles.count; i++) if (!strcmp(g_profiles.p[i].name, name)) return g_profiles.p[i].id;
  return 0;
}
static const ProfileInfo *prof_by_id(int id) {
  for (int i = 0; i < g_profiles.count; i++) if (g_profiles.p[i].id == id) return &g_profiles.p[i];
  return NULL;
}
// when a profile was last used (the core works it out from the shot history), 0 = not in it
static uint32_t prof_last_used(const char *name) {
  const bd_profiles *l = bd_profile_list();
  for (int i = 0; i < l->count; i++) if (!strcmp(l->p[i].name, name)) return l->p[i].last_used;
  return 0;
}

void picker_close(void) { lv_obj_set_hidden(g_pick, true); }
void picker_open(void) {
  picker_rebuild();                                // only if what the rows show has changed (picker_sig)
  lv_obj_scroll_to_y(g_pick_list, 0, LV_ANIM_OFF);
  lv_obj_set_hidden(g_pick, false);
}
void on_pick_open(lv_event_t *e)  { (void)e; picker_open(); }
static void on_pick_close(lv_event_t *e) { (void)e; picker_close(); }
// ---- originals and "edited" -----------------------------------------------------------------------------------------
// Changes made here are saved on the machine (bd_set_* with save = true), so they outlive a machine restart. To get
// back, the screen keeps a snapshot of each profile's stop weight and brew temperature (its "original": the stored
// values the first time the screen sees the profile, or from before its first change here; ui_bridge.cpp keeps them
// in the UI's storage, key = the profile name). While a value differs from it, Ready says "Brew temp - edited" /
// "Stop weight - edited", the keypad has "Reset to <original>", and on the Profiles page the edited values are blue
// with a Restore button (a long press on the row too), which offers Restore original / Keep current (snap_*). The
// snapshot is never dropped.
static bool (*g_orig_load)(const char *profile, int *w10, int *deg) = NULL;
static void (*g_orig_save)(const char *profile, int w10, int deg) = NULL;
static char g_orig_name[48] = "\x01";     // profile the cache below is for (never a real name at start)
bool g_orig_have = false;
int g_orig_w10 = -1, g_orig_deg = -1;
void ui_on_originals(bool (*load)(const char *, int *, int *), void (*save)(const char *, int, int)) {
  g_orig_load = load;
  g_orig_save = save;
}
void orig_sync(void) {
  if (!strcmp(g_orig_name, g_cur_profile)) return;
  snprintf(g_orig_name, sizeof g_orig_name, "%s", g_cur_profile);
  g_orig_have = g_cur_profile[0] && g_orig_load && g_orig_load(g_cur_profile, &g_orig_w10, &g_orig_deg);
}
static void snap_put(const char *name, int w10, int deg);
void orig_note(void) {                     // a change is about to be sent: keep the values from before it
  orig_sync();
  if (g_orig_have || !g_cur_profile[0] || !g_orig_save) return;
  if (g_act_weight10 < 0 || g_act_temp < T_MIN) return;    // the machine's values are not known yet
  g_orig_w10 = g_act_weight10;
  g_orig_deg = g_act_temp;
  g_orig_have = true;
  snap_put(g_cur_profile, g_orig_w10, g_orig_deg);   // storage + the snapshots' RAM copy
}
bool orig_temp_edited(void) { return g_orig_have && g_tset > 0 && g_tset != g_orig_deg; }
bool orig_weight_edited(void) { return g_orig_have && g_orig_w10 >= 0 && g_weight10 != g_orig_w10; }
void orig_paint(void) {
  if (!g_tt_lbl) return;
  orig_sync();
  set_text(g_tt_lbl, orig_temp_edited() ? "Brew temp \xC2\xB7 edited" : "Brew temp");
  set_text(g_tw_lbl, orig_weight_edited() ? "Stop weight \xC2\xB7 edited" : "Stop weight");
}

// ---- profile snapshots (Profiles page): the originals of every profile, restore / keep ------------------------------
// snapshots kept in RAM: the page asks for every profile's on each update while open, and storage (NVS) reads are slow
static struct { char name[48]; int w10, deg; bool have; } g_snap[MAX_PROFILES];
static int g_snap_n = 0;
static int snap_slot(const char *name) {
  for (int i = 0; i < g_snap_n; i++) if (!strcmp(g_snap[i].name, name)) return i;
  if (g_snap_n == MAX_PROFILES) return -1;
  snprintf(g_snap[g_snap_n].name, sizeof g_snap[0].name, "%s", name);   // first ask: read it from storage once
  g_snap[g_snap_n].have = g_orig_load && g_orig_load(name, &g_snap[g_snap_n].w10, &g_snap[g_snap_n].deg);
  return g_snap_n++;
}
static bool snap_get(const char *name, int *w10, int *deg) {
  const int k = snap_slot(name);
  if (k < 0) return g_orig_load && g_orig_load(name, w10, deg);
  if (g_snap[k].have) { *w10 = g_snap[k].w10; *deg = g_snap[k].deg; }
  return g_snap[k].have;
}
static void snap_put(const char *name, int w10, int deg) {   // stores it and keeps the RAM copy in step
  if (g_orig_save) g_orig_save(name, w10, deg);
  const int k = snap_slot(name);
  if (k >= 0) { g_snap[k].w10 = w10; g_snap[k].deg = deg; g_snap[k].have = true; }
}
// the first time a profile's stored values are known (the list's details), they become its snapshot
static void snap_capture(void) {
  if (!g_orig_save) return;
  bool any = false;
  for (int i = 0; i < g_profiles.count; i++) {
    const ProfileInfo *p = &g_profiles.p[i];
    int w, d;
    if (!p->detail || p->temp < T_MIN || snap_get(p->name, &w, &d)) continue;
    snap_put(p->name, (int)(p->weight * 10.0f + 0.5f), (int)(p->temp + 0.5f));
    any = true;
  }
  if (any) { g_orig_name[0] = 1; orig_paint(); }    // the active profile's cache reloads
}
// a profile's current values: what the machine runs for the active one, else the stored ones
static void prof_now(const ProfileInfo *p, int *w10, int *deg) {
  if (p->selected && !g_pend_id && g_act_weight10 >= 0 && g_act_temp > 0) {
    *w10 = g_act_weight10;
    *deg = g_act_temp;
    return;
  }
  *w10 = (int)(p->weight * 10.0f + 0.5f);
  *deg = (int)(p->temp + 0.5f);
}
static bool prof_edited(const ProfileInfo *p) {
  int sw, sd, cw, cd;
  if (!p->detail || !snap_get(p->name, &sw, &sd)) return false;
  prof_now(p, &cw, &cd);
  return cw != sw || cd != sd;
}
static void snap_values(char *out, size_t n, int w10, int deg) {
  char w[12];
  if (w10 > 0) { format_weight(w, sizeof w, w10); snprintf(out, n, "%s g \xC2\xB7 %d \xC2\xB0" "C", w, deg); }
  else snprintf(out, n, "no stop weight \xC2\xB7 %d \xC2\xB0" "C", deg);
}

// restore: the machine changes only the active profile, so another profile is selected first and its values are
// sent once the machine reports it active (ui_set_live, within RESTORE_WAIT_MS); both are saved like any change here
static bool pick_profile(int id);
Restore g_rst;
void snap_apply(void) {
  if (g_rst.w10 != g_act_weight10) {               // 0 = no stop weight: restored too
    g_weight10 = g_rst.w10;
    refresh_value();
    bd_set_stop_weight(g_rst.id, g_rst.w10 / 10.0f, true);
  }
  g_tset = g_rst.deg;
  refresh_tset();
  bd_set_brew_temp(g_rst.id, g_rst.deg, true);
  g_rst.id = 0;
  orig_paint();
}
static lv_obj_t *g_rs_back, *g_rs_title, *g_rs_orig, *g_rs_now, *g_rs_note;
static int g_rs_id = 0;
static void snap_close(void) { if (g_rs_back) lv_obj_set_hidden(g_rs_back, true); g_rs_id = 0; }
static void on_rs_restore(lv_event_t *e) {
  (void)e;
  const ProfileInfo *p = prof_by_id(g_rs_id);
  int sw, sd;
  snap_close();
  if (!p || !snap_get(p->name, &sw, &sd) || !g_can_select) return;
  g_rst.id = p->id;
  snprintf(g_rst.name, sizeof g_rst.name, "%s", p->name);
  g_rst.w10 = sw;
  g_rst.deg = sd;
  g_rst.t = stamp_nz(lv_tick_get());
  if (p->selected) snap_apply();                   // active: at once
  else if (pick_profile(p->id)) picker_close();    // else after the switch (ui_set_live)
  else g_rst.id = 0;
}
static void on_rs_keep(lv_event_t *e) {
  (void)e;
  const ProfileInfo *p = prof_by_id(g_rs_id);
  snap_close();
  if (!p || !g_orig_save) return;
  int cw, cd;
  prof_now(p, &cw, &cd);
  snap_put(p->name, cw, cd);
  g_orig_name[0] = 1;                              // the active profile's cache reloads
  orig_paint();
  picker_rebuild();
}
static lv_obj_t *rs_button(lv_obj_t *row, const char *text, bool primary, lv_event_cb_t cb) {
  lv_obj_t *b = flex(row, LV_FLEX_FLOW_ROW, 0);
  lv_obj_set_height(b, 52);
  lv_obj_set_style_pad_hor(b, 20, 0);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_radius(b, RAD_CTRL, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, primary ? COL_GOLD : COL_CARD_PR, 0);
  make_button(b, primary ? COL_GOLD_PR : COL_CARD);
  label(b, text, F_TEXT_B, primary ? COL_BG : COL_TEXT);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}
static void snap_build(void) {
  g_rs_back = box(lv_layer_top(), 0, 0, 800, 480, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_rs_back, LV_OPA_50, 0);
  lv_obj_set_clickable(g_rs_back, true);
  lv_obj_add_event_cb(g_rs_back, [](lv_event_t *) { snap_close(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *c = flex(g_rs_back, LV_FLEX_FLOW_COLUMN, 10);
  lv_obj_set_width(c, 560);
  lv_obj_set_style_pad_all(c, 24, 0);
  lv_obj_set_style_radius(c, RAD_CARD, 0);
  lv_obj_set_style_bg_color(c, COL_CARD, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_border_color(c, COL_BORDER, 0);
  lv_obj_set_clickable(c, true);                   // taps on the card do not close it
  lv_obj_center(c);
  g_rs_title = label(c, "", F_ROW_B, COL_TEXT);
  one_line(g_rs_title, 512);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  for (int k = 0; k < 2; k++) {                    // "Original" / "Now", values in a second column
    lv_obj_t *rw = flex(c, LV_FLEX_FLOW_ROW, 0);
    lv_obj_t *n = label(rw, k ? "Now" : "Original", F_TEXT, COL_MUTED);
    lv_obj_set_width(n, 96);
    (k ? g_rs_now : g_rs_orig) = label(rw, "", F_TEXT_B, k ? COL_MUTED : COL_TEXT);
  }
  g_rs_note = label(c, "", F_TEXT, COL_MUTED);
  lv_label_set_long_mode(g_rs_note, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(g_rs_note, 512);
  lv_obj_t *row = flex(c, LV_FLEX_FLOW_ROW, 10);
  lv_obj_set_style_pad_top(row, 8, 0);
  rs_button(row, "Cancel", false, [](lv_event_t *) { snap_close(); });
  rs_button(row, "Keep current", false, on_rs_keep);
  rs_button(row, "Restore original", true, on_rs_restore);
  lv_obj_set_hidden(g_rs_back, true);
}
static void on_pick_hold(lv_event_t *e) {           // a long press on an edited row: its snapshot
  const ProfileInfo *p = prof_by_id((int)(intptr_t)lv_event_get_user_data(e));
  int sw, sd, cw, cd;
  if (!p || !prof_edited(p) || !snap_get(p->name, &sw, &sd)) return;
  prof_now(p, &cw, &cd);
  if (!g_rs_back) snap_build();
  g_rs_id = p->id;
  char name[48], v[48];
  display_name(p->name, name, sizeof name, NULL);
  set_text(g_rs_title, name);
  snap_values(v, sizeof v, sw, sd);
  set_text(g_rs_orig, v);
  snap_values(v, sizeof v, cw, cd);
  set_text(g_rs_now, v);
  set_text(g_rs_note,
           p->selected ? "Restore sets the original values and saves them on the machine."
                       : "Restore selects this profile, then sets the original values and saves them on the machine.");
  lv_obj_set_hidden(g_rs_back, false);
}
static void on_pick_row(lv_event_t *e) {
  const int id = (int)(intptr_t)lv_event_get_user_data(e);
  picker_close();
  pick_profile(id);
}
// selects profile id on the machine and shows it at once; false if not sent (active already, busy, offline)
static bool pick_profile(int id) {
  const ProfileInfo *p = prof_by_id(id);
  if (!p || p->selected || !g_can_select) return false;
  // a -/+ change still waiting to be sent belongs to the OLD profile: send it now, before the switch (the core
  // refuses it if the machine has already switched), so it can never land on the new profile or poison its originals
  if (timer_pending(g_weight_tm)) weight_commit(NULL);
  if (timer_pending(g_temp_tm)) temp_commit(NULL);
  // saved as the boot profile once the machine has taken it; refused (offline, busy): nothing to show
  if (bd_select_profile(id, true) != BD_OK) return false;
  // show the choice right away; the machine's status confirms it, or the core's answer reverts it (ui_command_answer)
  g_pend_id = id;
  snprintf(g_pend_name, sizeof g_pend_name, "%s", p->name);
  for (int i = 0; i < g_profiles.count; i++) g_profiles.p[i].selected = g_profiles.p[i].id == id;
  show_profile(p->name);
  if (p->detail) {                                  // 0 = no stop weight
    int w10 = (int)(p->weight * 10.0f + 0.5f);
    g_weight10 = clamp_w10(w10);
    refresh_value();
  }
  if (p->detail && p->temp >= T_MIN && p->temp <= T_MAX) {   // its stored temperature too, until the machine's comes
    g_tset = (int)(p->temp + 0.5f);
    refresh_tset();
  }
  pv_loading();                                  // the new graph follows in about a second
  return true;
}

static int text_w(const char *s, const lv_font_t *f) {
  lv_point_t sz;
  lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return sz.x;
}

static lv_obj_t *g_pick_hdr;                     // the column names
// a right-aligned cell of a row; x_right is the screen x of its right edge (rows start at screen x 12)
static lv_obj_t *pk_cell(lv_obj_t *row, const char *t, int x_right, int w, lv_color_t col) {
  lv_obj_t *l = label(row, t, F_TEXT, col);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
  one_line(l, w);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, x_right - 12 - w, 0);
  return l;
}

static void add_row(lv_obj_t *list, const ProfileInfo *p) {
  const bool sel = p->selected;
  // the row reaches 12 px past the 24 px margin on both sides, so the active / pressed background sits around the text
  lv_obj_t *row = box(list, 0, 0, PK_W + 24, PK_RH, sel ? COL_PK_SEL : COL_BG, RAD_CTRL);
  if (!sel) lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  make_button(row, COL_CARD_PR);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_add_event_cb(row, on_pick_row, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)p->id);   // not after a hold
  lv_obj_add_event_cb(row, on_pick_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)p->id);
  box(row, 12, PK_RH - 1, PK_W, 1, COL_LINE, 0);             // row divider

  // name (+ Community), shortened with "..." when it does not fit its column
  char name[48];
  bool community;
  display_name(p->name, name, sizeof name, &community);
  const lv_font_t *nf = sel ? F_ROW_B : F_ROW;
  lv_obj_t *nrow = flex(row, LV_FLEX_FLOW_ROW, 8);
  lv_obj_set_flex_align(nrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(nrow, LV_ALIGN_LEFT_MID, 12, 0);
  int avail = PK_NAME_W;
  lv_obj_t *nl = label(nrow, name, nf, COL_TEXT);
  if (community) {
    lv_obj_t *tag = flex(nrow, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_pad_hor(tag, 7, 0);
    lv_obj_set_style_pad_ver(tag, 2, 0);
    lv_obj_set_style_radius(tag, RAD_PILL, 0);
    lv_obj_set_style_bg_color(tag, COL_PK_TAG, 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
    label(tag, "Community", F_SMALL, COL_MUTED);
    avail -= text_w("Community", F_SMALL) + 14 + 8;
  }
  if (text_w(name, nf) > avail) one_line(nl, avail);

  // stop weight and temperature as they are now (running values for the active profile); blue where they differ
  // from the profile's original (Restore puts it back)
  int cw = 0, cd = 0, sw = -1, sd = -1;
  if (p->detail) prof_now(p, &cw, &cd);
  // values known (a profile's details come a few seconds after the list)
  if (cd > 0) {
    const bool snap = snap_get(p->name, &sw, &sd);
    char b[16];
    if (cw > 0) {
      if (cw % 10) snprintf(b, sizeof b, "%d.%d g", cw / 10, cw % 10); else snprintf(b, sizeof b, "%d g", cw / 10);
      pk_cell(row, b, PK_STOP_R, 64, snap && cw != sw ? COL_BLUE : COL_TEXT);
    } else {                                       // no stop weight: infinity
      pk_cell(row, "\xE2\x88\x9E", PK_STOP_R, 64, snap && cw != sw ? COL_BLUE : COL_MUTED);
    }
    snprintf(b, sizeof b, "%d\xC2\xB0", cd);
    pk_cell(row, b, PK_TEMP_R, 52, snap && cd != sd ? COL_BLUE : COL_TEXT);
  }
  const uint32_t used = prof_last_used(p->name);
  if (used) {
    char w[32];
    fmt_when(used, w, sizeof w);
    pk_cell(row, w, PK_USED_R, 124, COL_MUTED);
  }
  // Restore (an edited profile: opens the panel with its original values), else the active profile's check
  if (prof_edited(p)) {
    lv_obj_t *bt = box(row, 0, 0, PK_ACT_W, 36, COL_CARD, 12);
    make_button(bt, COL_CARD_PR);
    lv_obj_align(bt, LV_ALIGN_LEFT_MID, PK_ACT_X - 12, 0);
    lv_obj_center(label(bt, "Restore", F_TEXT_B, COL_TEXT));
    lv_obj_add_event_cb(bt, on_pick_hold, LV_EVENT_CLICKED, (void *)(intptr_t)p->id);
  } else if (sel) {
    lv_obj_t *ck = flex(row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -12, 0);
    icon_check(ck, COL_GOLD);
  }
}

// fingerprint of everything the rows show: rebuilding ~100 objects is most of the page's opening time, and usually
// nothing has changed since the last time (the day is in it for "today" / "yesterday")
static uint32_t picker_sig(void) {
  uint32_t h = FNV_SEED;
  auto mix = [&h](const void *p, size_t n) { h = fnv1a(h, p, n); };
  const ProfileList *l = &g_profiles;
  mix(&l->count, sizeof l->count);
  for (int i = 0; i < l->count; i++) {
    const ProfileInfo *p = &l->p[i];
    mix(&p->id, sizeof p->id); mix(p->name, strlen(p->name)); mix(&p->selected, 1); mix(&p->detail, 1);
    mix(&p->phases, sizeof p->phases); mix(&p->weight, sizeof p->weight); mix(&p->temp, sizeof p->temp);
    const uint32_t used = prof_last_used(p->name);
    mix(&used, sizeof used);
    // its snapshot and current values (edited ones blue, Restore button)
    int sw = -1, sd = -1, cw, cd;
    snap_get(p->name, &sw, &sd);
    prof_now(p, &cw, &cd);
    mix(&sw, sizeof sw); mix(&sd, sizeof sd); mix(&cw, sizeof cw); mix(&cd, sizeof cd);
  }
  const int day = today_key();
  mix(&day, sizeof day);
  return h;
}
static uint32_t g_pick_sig = 0;
void picker_rebuild(void) {
  const uint32_t sig = picker_sig();
  if (g_profiles.count && sig == g_pick_sig) return;   // the rows on the page are still right
  g_pick_sig = sig;
  lv_obj_clean(g_pick_list);
  const ProfileList *l = &g_profiles;
  set_hidden(g_pick_hdr, l->count == 0);
  if (l->count == 0) {
    EmptyState es = empty_state(g_pick_list, IC_PROFILES);
    empty_set(es, "Profiles not loaded yet", "They come from the machine, a few seconds after it answers.");
    return;
  }
  // order: the active profile first, then recently used (newest shot first), then the never used ones by name
  int idx[sizeof l->p / sizeof l->p[0]];
  uint32_t used[sizeof l->p / sizeof l->p[0]];
  const int n = l->count;
  for (int i = 0; i < n; i++) { idx[i] = i; used[i] = prof_last_used(l->p[i].name); }
  for (int i = 1; i < n; i++)                     // insertion sort, at most a few dozen profiles
    for (int j = i; j > 0; j--) {
      const int a = idx[j - 1], b = idx[j];
      bool swap;
      if (l->p[a].selected != l->p[b].selected) swap = l->p[b].selected;
      else if (used[a] != used[b]) swap = used[b] > used[a];
      else {
        char na[48], nb[48];
        display_name(l->p[a].name, na, sizeof na, NULL);
        display_name(l->p[b].name, nb, sizeof nb, NULL);
        const char *ka = na, *kb = nb;             // by name, ignoring leading symbols ("[UT] Tube Fill" under U)
        while (*ka && !isalnum((unsigned char)*ka)) ka++;
        while (*kb && !isalnum((unsigned char)*kb)) kb++;
        swap = strcasecmp(kb, ka) < 0;
      }
      if (!swap) break;
      idx[j - 1] = b; idx[j] = a;
    }
  lv_obj_t *col = flex(g_pick_list, LV_FLEX_FLOW_COLUMN, 0);
  for (int i = 0; i < n; i++) add_row(col, &l->p[idx[i]]);
}

void build_picker(lv_obj_t *scr) {
  g_pick = box(scr, 0, 0, 800, 480, COL_BG, 0);
  lv_obj_set_clickable(g_pick, true);               // nothing below can be used
  lv_obj_set_hidden(g_pick, true);
  lv_obj_set_pos(label(g_pick, "Profiles", F_TITLE, COL_TEXT), 24, 32);
  g_pick_sub = label(g_pick, "Active, then recently used", F_TEXT, COL_MUTED);
  lv_obj_align(g_pick_sub, LV_ALIGN_TOP_RIGHT, -92, 41);
  lv_obj_t *cl = box(g_pick, 724, 24, 52, 52, COL_CARD, RAD_CTRL);
  make_button(cl, COL_CARD_PR);
  lv_obj_t *cw = flex(cl, LV_FLEX_FLOW_ROW, 0);
  lv_obj_center(cw);
  icon_close(cw, COL_TEXT);
  lv_obj_add_event_cb(cl, on_pick_close, LV_EVENT_CLICKED, NULL);
  // column names over the list (they stay while it scrolls; hidden while there are no profiles)
  g_pick_hdr = box(g_pick, 0, PK_HDR, 800, PK_HDR_H, COL_BG, 0);
  lv_obj_set_style_bg_opa(g_pick_hdr, LV_OPA_TRANSP, 0);
  lv_obj_set_pos(label(g_pick_hdr, "Profile", F_SMALL, COL_FAINT), 24, 7);
  static const char *const HDR[3] = {"Stop", "Temp", "Last used"};
  static const int HDR_R[3] = {PK_STOP_R, PK_TEMP_R, PK_USED_R};
  for (int k = 0; k < 3; k++) {
    lv_obj_t *h = label(g_pick_hdr, HDR[k], F_SMALL, COL_FAINT);
    lv_obj_update_layout(h);
    lv_obj_set_pos(h, HDR_R[k] - lv_obj_get_width(h), 7);
  }
  box(g_pick_hdr, 24, PK_HDR_H - 1, PK_W, 1, COL_LINE, 0);
  // the list: x 12..788 so the rows' backgrounds can reach 12 px past the 24 px margin
  g_pick_list = box(g_pick, 12, PK_Y, 800 - 24, 480 - PK_Y, COL_BG, 0);
  lv_obj_set_style_bg_opa(g_pick_list, LV_OPA_TRANSP, 0);
  lv_obj_set_clickable(g_pick_list, true);
  lv_obj_set_scrollable(g_pick_list, true);
  lv_obj_set_scroll_dir(g_pick_list, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(g_pick_list, 16, 0);
  lv_obj_set_style_width(g_pick_list, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(g_pick_list, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(g_pick_list, COL_OUTLINE, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(g_pick_list, LV_OPA_COVER, LV_PART_SCROLLBAR);
}

void ui_set_profiles(const ProfileList *l) {
  g_profiles = *l;
  snap_capture();                                  // new profiles' stored values become their snapshots
  if (g_pend_id)                                 // a select is in flight: keep showing the choice
    for (int i = 0; i < g_profiles.count; i++) g_profiles.p[i].selected = g_profiles.p[i].id == g_pend_id;
  if (!lv_obj_is_hidden(g_pick)) {
    int32_t sy = lv_obj_get_scroll_y(g_pick_list);
    picker_rebuild();
    lv_obj_scroll_to_y(g_pick_list, sy, LV_ANIM_OFF);
  }
}
