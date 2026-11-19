// The Ready screen (header, temperature, profile preview, brew temperature and stop weight steppers, footer, flush
// button) and the keypad for both values.
#include "../ui_int.h"

// ---- weight handling ---------------------------------------------------------------------------
void format_weight(char *out, size_t n, int tenths) {
  snprintf(out, n, "%d.%d", tenths / 10, tenths % 10);
}

// originals and "edited" marks: see orig_note (profiles.cpp)
bool (*g_orig_load)(const char *profile, int *w10, int *deg) = NULL;
void (*g_orig_save)(const char *profile, int w10, int deg) = NULL;
void (*g_orig_drop)(const char *profile) = NULL;
char g_orig_name[48] = "\x01";            // profile the cache below is for (never a real name at start)
bool g_orig_have = false;
int g_orig_w10 = -1, g_orig_deg = -1;
lv_obj_t *g_tt_lbl, *g_tw_lbl;              // "Brew temp" / "Stop weight" on Ready
lv_obj_t *g_reset_btn, *g_reset_lbl;        // keypad: "Reset to ..."
static lv_obj_t *g_value_unit;          // "g" after the stop weight (hidden for no stop weight)
void refresh_value(void) {
  char buf[12];
  if (g_weight10 <= 0) snprintf(buf, sizeof buf, "\xE2\x88\x9E");   // no stop weight: infinity (U+221E)
  else format_weight(buf, sizeof buf, g_weight10);
  lv_label_set_text(g_value_lbl, buf);
  if (g_value_unit) lv_obj_set_hidden(g_value_unit, g_weight10 <= 0);
  orig_paint();
}

bool g_can_select = false;       // online, idle, mode HOME (set by ui_set_live)
void step_weight(int delta10) {
  if (!g_can_select) return;            // like step_temp: nothing to change while busy / offline
  if (g_weight10 <= 0 && delta10 < 0) return;      // no stop weight: nothing below it
  int v = g_weight10 <= 0 ? W_MIN10 : g_weight10 + delta10;
  if (v < W_MIN10) v = W_MIN10;
  if (v > W_MAX10) v = W_MAX10;
  if (v == g_weight10) return;
  g_weight10 = v;
  refresh_value();
  weight_changed(false);
}

static void on_minus(lv_event_t *e) { (void)e; step_weight(-W_STEP10); }
static void on_plus(lv_event_t *e)  { (void)e; step_weight(+W_STEP10); }

// ---- brew temperature ------------------------------------------------------------
static lv_obj_t *g_tset_lbl;
int g_tset = -1;                 // shown target, -1 = not known yet
int g_act_temp = -1;             // last active-profile temperature taken over
lv_timer_t *g_temp_tm;           // user data 1 while a change waits to be sent
void refresh_tset(void) {
  char buf[8];
  if (g_tset < 0) snprintf(buf, sizeof buf, "0"); else snprintf(buf, sizeof buf, "%d", g_tset);
  lv_label_set_text(g_tset_lbl, buf);
  lv_obj_set_style_text_color(g_tset_lbl, g_tset > 0 ? COL_TEXT : COL_OUTLINE, 0);
  orig_paint();
}
void temp_commit(lv_timer_t *t) {
  (void)t;
  if (g_temp_tm) { lv_timer_pause(g_temp_tm); lv_timer_set_user_data(g_temp_tm, NULL); }
  if (g_can_select && g_cur_profile[0] && g_tset >= T_MIN && bd_set_brew_temp(prof_id(g_cur_profile), g_tset, true) == BD_OK) orig_note();
  else if (g_act_temp > 0 && g_tset != g_act_temp) {   // not sent / refused: back to the machine's value, or
    g_tset = g_act_temp;                               // the screen keeps showing a temperature the machine never got
    refresh_tset();                                    //
  }
}
static void step_temp(int delta) {
  if (!g_can_select) return;            // nothing to change while busy / offline
  int v = g_tset < T_MIN ? T_START : g_tset + delta;   // unknown: start at 60, not at 1
  if (v < T_MIN) v = T_MIN;
  if (v > T_MAX) v = T_MAX;
  if (v == g_tset) return;
  g_tset = v;
  refresh_tset();
  if (!g_temp_tm) g_temp_tm = lv_timer_create(temp_commit, 1000, NULL);
  lv_timer_set_user_data(g_temp_tm, (void *)1);
  lv_timer_reset(g_temp_tm);
  lv_timer_resume(g_temp_tm);
}
static void on_t_minus(lv_event_t *e) { (void)e; step_temp(-1); }
static void on_t_plus(lv_event_t *e)  { (void)e; step_temp(+1); }

// ---- editor ---------------------------------------------------------------------------------
// One full-screen keypad for both targets: weight in g with one decimal, or the brew
// temperature in whole degC (the "." key is greyed). Done sends the value at once (weight_changed / temp_commit).
static bool g_ed_temp;                 // false: target weight, true: brew temperature
static char g_ed_was[24] = "";
lv_obj_t *g_ed_title, *g_key_dot, *g_key_dot_lbl;
static char g_ed_cur[8] = "";          // the value when the keypad opened (shown grey until the first key)
static void editor_layout(void) {
  // empty field = the current value in grey with the cursor in front of it (the first key
  // replaces it) and the accepted range below; while typing the cursor follows the digits and "Was ..." is shown
  const bool empty = g_entry[0] == 0;
  lv_label_set_text(g_entry_lbl, empty ? g_ed_cur : g_entry);
  lv_obj_set_style_text_color(g_entry_lbl, empty ? COL_OUTLINE : COL_TEXT, 0);
  lv_obj_set_x(g_entry_lbl, empty ? 24 + 4 + 6 : 24);
  lv_obj_update_layout(g_entry_lbl);
  const int w = lv_obj_get_width(g_entry_lbl);
  lv_obj_set_x(g_cursor, empty ? 24 : 24 + w + 6);
  // the unit 10 px after the digits (empty) or after the cursor (typing): close enough to read as one value even
  // while the cursor blinks off (was 23 px: "93    °C")
  lv_obj_set_x(g_unit_lbl, (empty ? 24 + 4 + 6 + w : 24 + w + 6 + 4) + 10);
  char b[48];
  if (!empty) lv_label_set_text(g_was_lbl, g_ed_was);
  else if (g_ed_temp) { snprintf(b, sizeof b, "Type a temperature from %d to %d \xC2\xB0" "C", T_MIN, T_MAX); lv_label_set_text(g_was_lbl, b); }
  else { snprintf(b, sizeof b, "Type a weight from %d to %d g, 0 for none", W_MIN10 / 10, W_MAX10 / 10); lv_label_set_text(g_was_lbl, b); }
}

void editor_open(bool temp) {
  g_ed_temp = temp;
  g_entry[0] = 0;
  char buf[24], was[24];
  if (temp) {
    if (g_tset > 0) snprintf(was, sizeof was, "Was %d \xC2\xB0" "C", g_tset); else snprintf(was, sizeof was, " ");
  } else if (g_weight10 > 0) {
    format_weight(buf, sizeof buf, g_weight10);
    snprintf(was, sizeof was, "Was %s g", buf);
  } else {                                        // no stop weight: nothing to show in the entry (no infinity in F_HERO)
    buf[0] = 0;
    snprintf(was, sizeof was, "Was no stop weight");
  }
  snprintf(g_ed_was, sizeof g_ed_was, "%s", was);
  if (temp) { if (g_tset > 0) snprintf(g_ed_cur, sizeof g_ed_cur, "%d", g_tset); else g_ed_cur[0] = 0; }
  else snprintf(g_ed_cur, sizeof g_ed_cur, "%s", buf);
  lv_label_set_text(g_ed_title, temp ? "Brew temp" : "Stop weight");
  lv_label_set_text(g_unit_lbl, temp ? "\xC2\xB0" "C" : "g");
  lv_obj_set_style_text_color(g_key_dot_lbl, temp ? COL_OUTLINE : COL_TEXT, 0);
  if (temp) lv_obj_set_clickable(g_key_dot, false); else lv_obj_set_clickable(g_key_dot, true);
  {                                                // "Reset to <original>" while the value differs from it
    orig_sync();
    const bool show = temp ? orig_temp_edited() : orig_weight_edited();
    char r[32];
    if (temp) snprintf(r, sizeof r, "Reset to %d \xC2\xB0" "C", g_orig_deg);
    else if (g_orig_w10 <= 0) snprintf(r, sizeof r, "Reset to no stop weight");
    else { char w[12]; format_weight(w, sizeof w, g_orig_w10); snprintf(r, sizeof r, "Reset to %s g", w); }
    lv_label_set_text(g_reset_lbl, r);
    if (show) lv_obj_set_hidden(g_reset_btn, false); else lv_obj_set_hidden(g_reset_btn, true);
  }
  editor_layout();
  g_cursor_on = true;
  lv_obj_set_style_bg_opa(g_cursor, LV_OPA_COVER, 0);
  lv_obj_set_hidden(g_editor, false);
}

void editor_close(void) { lv_obj_set_hidden(g_editor, true); }

void editor_key(char k) {
  size_t len = strlen(g_entry);
  const char *dot = strchr(g_entry, '.');
  if (k == 'b') {
    if (len > 0) g_entry[len - 1] = 0;
  } else if (k == '.') {
    if (dot || g_ed_temp) return;                      // whole degrees only                                   // only one decimal point
    if (len == 0) strcpy(g_entry, "0.");
    else { g_entry[len] = '.'; g_entry[len + 1] = 0; }
  } else if (k >= '0' && k <= '9') {
    if (dot) {
      if (strlen(dot + 1) >= 1) return;                // one decimal place
      g_entry[len] = k;
      g_entry[len + 1] = 0;
    } else if (len == 1 && g_entry[0] == '0') {
      g_entry[0] = k;                                  // replace a lone leading zero
    } else if (len < (g_ed_temp ? 3u : 2u)) {          // two integer digits at most (temperature: three)
      g_entry[len] = k;
      g_entry[len + 1] = 0;
    } else {
      return;
    }
  }
  editor_layout();
}

void editor_done(void) {
  if (g_ed_temp) {
    if (g_entry[0] != 0) {
      int v = atoi(g_entry);
      if (v < T_MIN) v = T_MIN;
      if (v > T_MAX) v = T_MAX;
      if (v != g_tset) {
        g_tset = v;
        refresh_tset();
        temp_commit(NULL);                             // sent at once, like the weight's Done
      }
    }
    editor_close();
    return;
  }
  if (g_entry[0] != 0) {
    float f = (float)atof(g_entry);
    int v = (int)(f * 10.0f + 0.5f);
    if (v < W_MIN10 && v != 0) v = W_MIN10;       // 0 = no stop weight
    if (v > W_MAX10) v = W_MAX10;
    if (v != g_weight10) {
      g_weight10 = v;
      refresh_value();
      weight_changed(true);
    }
  }
  editor_close();
}

void on_reset(lv_event_t *e) {             // back to the profile's value from before the first change here
  (void)e;
  orig_sync();
  if (!g_orig_have) { editor_close(); return; }
  if (g_ed_temp) snprintf(g_entry, sizeof g_entry, "%d", g_orig_deg);
  else if (g_orig_w10 <= 0) snprintf(g_entry, sizeof g_entry, "0");
  else format_weight(g_entry, sizeof g_entry, g_orig_w10);
  editor_done();                                   // sends it like Done (and the save follows, ws.cpp); the original
}                                                  // (the profile's snapshot) stays
static void on_open_editor(lv_event_t *e) { (void)e; editor_open(false); }
static void on_open_temp_editor(lv_event_t *e) { (void)e; editor_open(true); }
void on_cancel(lv_event_t *e)      { (void)e; editor_close(); }
void on_done(lv_event_t *e)        { (void)e; editor_done(); }
void on_key(lv_event_t *e)         { editor_key((char)(intptr_t)lv_event_get_user_data(e)); }

void on_blink(lv_timer_t *t) {
  (void)t;
  if (lv_obj_is_hidden(g_editor)) return;
  g_cursor_on = !g_cursor_on;
  lv_obj_set_style_bg_opa(g_cursor, g_cursor_on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

// ---- profile preview + target weight from the active profile (display only) -------------------
void ui_set_shape(const ProfileShape *sh) {
  static bd_preview bp;                            // the core's preview for our box (preview.cpp geometry)
  if (sh != &g_last_shape) g_last_shape = *sh;
  bd_profile_preview(sh, PV_W, PV_H, &bp);
  const PvSeries *pv = (const PvSeries *)bp.s;
  // draw order: limits (dim, 2 px) under targets (3 px). Dashed is kept for "compared with" (brewing chart, review)
  static const int order[PV_SERIES] = {PV_P_LIMIT, PV_F_LIMIT, PV_P_TARGET, PV_F_TARGET};
  for (int oi = 0; oi < PV_SERIES; oi++) {
    const int k = order[oi];
    const bool target = k == PV_P_TARGET || k == PV_F_TARGET;
    const lv_color_t base = (k == PV_P_TARGET || k == PV_P_LIMIT) ? COL_PRESS : COL_FLOW;
    const lv_color_t col = target ? base : lv_color_mix(base, COL_BG, 102);   // limit: 40 %, pre-mixed (no dots)
    for (int i = 0; i < pv[k].n; i++) {
      g_pv_pts[k][i].x = pv[k].pts[i].x;
      g_pv_pts[k][i].y = pv[k].pts[i].y;
    }
    for (int sg = 0; sg < PV_MAX_SEG; sg++) {
      lv_obj_t *l = g_pv_line[k][sg];
      if (sg >= pv[k].nseg) { if (l) lv_obj_set_hidden(l, true); continue; }
      const int a = pv[k].seg[sg], n = pv[k].seg[sg + 1] - a;
      if (!l) {
        l = g_pv_line[k][sg] = line(g_chart, &g_pv_pts[k][a], (uint16_t)n, target ? 3 : 2, col);
      }
      else lv_line_set_points(l, (const lv_point_precise_t *)&g_pv_pts[k][a], (uint16_t)n);
      lv_obj_set_hidden(l, false);
    }
  }
  if (sh->n == 0) { set_text(g_pv_msg, "No phases"); lv_obj_set_hidden(g_pv_msg, false); }
  else lv_obj_set_hidden(g_pv_msg, true);
  g_shape_loaded = true;
  // stop weight: the STORED profile's, only until the machine reports the running one (d_act_prof, ui_set_live):
  // a graph re-read after a save must not put an older stored weight over the running one (seen: a restore to
  // "no stop weight" went back to 5.0 g when the graph came in a moment later)
  const int w10 = (int)(sh->stop_weight * 10.0f + 0.5f);
  if (g_act_weight10 < 0 && w10 > 0 && (sh->id != g_shape_id || w10 != g_shape_weight10)) {
    g_weight10 = clamp_w10(w10);
    refresh_value();
  }
  g_shape_id = sh->id;
  g_shape_weight10 = w10;
}

void pv_loading(void) {
  for (int k = 0; k < PV_SERIES; k++)
    for (int sg = 0; sg < PV_MAX_SEG; sg++)
      if (g_pv_line[k][sg]) lv_obj_set_hidden(g_pv_line[k][sg], true);
  set_text(g_pv_msg, "Loading...");
  lv_obj_set_hidden(g_pv_msg, false);
}

void show_profile(const char *raw) {      // header + editor label (without the "(community)" suffix)
  char b[48];
  snprintf(g_cur_profile, sizeof g_cur_profile, "%s", raw);
  display_name(raw, b, sizeof b, NULL);
  set_text(g_prof_lbl, b);
  set_text(g_ed_prof_lbl, b);
  orig_paint();
}

// ---- Ready screen ---------------------------------------------------------------------------
// a footer item is a pill only while it needs attention (orange); otherwise plain icon + text
void foot_pill(lv_obj_t *p, bool warn, int32_t pl, int32_t pr) {
  const lv_opa_t opa = warn ? LV_OPA_COVER : LV_OPA_TRANSP;
  if (lv_obj_get_style_bg_opa(p, LV_PART_MAIN) != opa) lv_obj_set_style_bg_opa(p, opa, 0);
  if (!warn) pl = pr = 0;
  if (lv_obj_get_style_pad_left(p, LV_PART_MAIN) != pl) lv_obj_set_style_pad_left(p, pl, 0);
  if (lv_obj_get_style_pad_right(p, LV_PART_MAIN) != pr) lv_obj_set_style_pad_right(p, pr, 0);
}

// a -/value/+ group is one control: one card behind three transparent parts that darken while pressed
static void stepper_card(lv_obj_t *scr, int x, int w) { box(scr, x, 304, w, 64, COL_CARD, RAD_CTRL); }
void stepper_part(lv_obj_t *o) {
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
}

void build_ready(lv_obj_t *scr) {
  // header, left: state pill (the water pill is in the footer, next to the scale)
  lv_obj_t *hl = flex(scr, LV_FLEX_FLOW_ROW, 14);
  lv_obj_set_pos(hl, 24, 24);
  lv_obj_set_height(hl, 52);

  lv_obj_t *pill = flex(hl, LV_FLEX_FLOW_ROW, 8);
  g_pill = pill;
  lv_obj_set_height(pill, 40);
  lv_obj_set_style_pad_left(pill, 16, 0);
  lv_obj_set_style_pad_right(pill, 16, 0);
  lv_obj_set_style_bg_color(pill, COL_PILL_BG, 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pill, RAD_PILL, 0);
  g_pill_dot = box(pill, 0, 0, 10, 10, COL_GREEN, 5);
  g_pill_lbl = label(pill, "Ready", F_TEXT_B, COL_PILL_TX);

  //   label(hl, "07:44", F_TEXT, COL_MUTED);

  // header, right: settings (the menu) and, left of it, a "Profiles" button that opens the profile page. The active
  // profile's name is information, after the state pill (like the brewing screen), not a field: a field with a down
  // chevron read as a dropdown, but it opens a whole page.
  lv_obj_t *menu = box(scr, 724, 24, 52, 52, COL_CARD, RAD_CTRL);
  make_button(menu, COL_CARD_PR);
  g_menu_btn = menu;
  lv_obj_center(line_icon(menu, IC_SETTINGS, 24, COL_TEXT));

  lv_point_t tsz;
  lv_text_get_size(&tsz, "Profiles", F_TEXT_B, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  const int32_t pw = 14 + 24 + 10 + tsz.x + 18, px = 724 - 8 - pw;
  lv_obj_t *prof = box(scr, px, 24, pw, 52, COL_CARD, RAD_CTRL);
  make_button(prof, COL_CARD_PR);
  lv_obj_add_event_cb(prof, on_pick_open, LV_EVENT_CLICKED, NULL);
  lv_obj_align(line_icon(prof, IC_PROFILES, 24, COL_BLUE), LV_ALIGN_LEFT_MID, 14, 0);
  lv_obj_align(label(prof, "Profiles", F_TEXT_B, COL_TEXT), LV_ALIGN_LEFT_MID, 14 + 24 + 10, 0);

  // machine notices: a bell left of Profiles (same size as the other header buttons), hidden until the first notice;
  // the buttons stay put, only the room for the name shrinks when it shows (ntc_refresh)
  g_hdr_left = hl;
  g_hdr_btn_x = px;
  ntc_build_btn(scr, px - 8 - 52);
  lv_obj_t *pl = label(hl, PROFILE_NAME, F_ROW_B, COL_TEXT);
  g_prof_lbl = pl;
  lv_obj_set_flex_grow(pl, 1);
  one_line(pl, 0);

  // left: boiler temperature
  lv_obj_t *tcol = flex(scr, LV_FLEX_FLOW_COLUMN, 0);
  lv_obj_set_pos(tcol, 24, 96);
  lv_obj_set_size(tcol, 328, 160);
  lv_obj_set_flex_align(tcol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_top(tcol, 34, 0);  // the digits sit on the preview's bottom line (y 224)
  lv_obj_t *trow = flex(tcol, LV_FLEX_FLOW_ROW, 0);
  lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  // no marker before the number (it adds little on Ready; brewing / steam keep their dots, they have several values side by side)
  lv_obj_t *temp = label(trow, "90.1", F_HERO, COL_TEXT);
  g_temp_lbl = temp;
  lv_obj_set_style_text_letter_space(temp, -6, 0);
  lv_obj_t *deg = label(trow, "\xC2\xB0", F_VALUE, COL_MUTED);
  lv_obj_set_style_pad_top(deg, 2, 0);
  lv_obj_set_style_translate_x(deg, -7, 0);

  // left, bottom: target temperature, -/+ like the target weight
  lv_obj_t *tt = g_tt_lbl = label(scr, "Brew temp", F_TEXT, COL_MUTED);
  lv_obj_set_pos(tt, 24, 275);
  stepper_card(scr, 24, 328);
  lv_obj_t *tminus = box(scr, 24, 304, 64, 64, COL_CARD, RAD_CTRL);
  make_button(tminus, COL_CARD_PR);
  stepper_part(tminus);
  lv_obj_t *tmi = flex(tminus, LV_FLEX_FLOW_ROW, 0);
  lv_obj_center(tmi);
  icon_minus(tmi, COL_TEXT);
  lv_obj_add_event_cb(tminus, on_t_minus, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(tminus, on_t_minus, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
  lv_obj_t *tplus = box(scr, 288, 304, 64, 64, COL_CARD, RAD_CTRL);
  make_button(tplus, COL_CARD_PR);
  stepper_part(tplus);
  lv_obj_t *tpi = flex(tplus, LV_FLEX_FLOW_ROW, 0);
  lv_obj_center(tpi);
  icon_plus(tpi, COL_TEXT);
  lv_obj_add_event_cb(tplus, on_t_plus, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(tplus, on_t_plus, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
  lv_obj_t *tvb = box(scr, 96, 304, 184, 64, COL_BG, RAD_CTRL);   // value (tap to type), like the weight's
  lv_obj_set_style_bg_opa(tvb, LV_OPA_TRANSP, 0);
  make_button(tvb, COL_CARD_PR);
  lv_obj_set_style_bg_opa(tvb, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_t *tval = flex(tvb, LV_FLEX_FLOW_ROW, 6);
  lv_obj_set_flex_align(tval, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
  lv_obj_center(tval);
  g_tset_lbl = label(tval, "90", F_VALUE, COL_TEXT);
  lv_obj_set_style_pad_bottom(label(tval, "\xC2\xB0" "C", F_ROW, COL_MUTED), base_pad(F_VALUE, F_ROW), 0);
  lv_obj_add_event_cb(tvb, on_open_temp_editor, LV_EVENT_CLICKED, NULL);

  // right: target weight
  lv_obj_t *tw = g_tw_lbl = label(scr, "Stop weight", F_TEXT, COL_MUTED);
  lv_obj_set_pos(tw, 433, 275);   // weight at the bottom, preview above

  stepper_card(scr, 433, 343);
  lv_obj_t *minus = box(scr, 433, 304, 64, 64, COL_CARD, RAD_CTRL);
  make_button(minus, COL_CARD_PR);
  stepper_part(minus);
  lv_obj_t *mi = flex(minus, LV_FLEX_FLOW_ROW, 0);
  lv_obj_center(mi);
  icon_minus(mi, COL_TEXT);
  lv_obj_add_event_cb(minus, on_minus, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(minus, on_minus, LV_EVENT_LONG_PRESSED_REPEAT, NULL);

  lv_obj_t *plus = box(scr, 712, 304, 64, 64, COL_CARD, RAD_CTRL);
  make_button(plus, COL_CARD_PR);
  stepper_part(plus);
  lv_obj_t *pi = flex(plus, LV_FLEX_FLOW_ROW, 0);
  lv_obj_center(pi);
  icon_plus(pi, COL_TEXT);
  lv_obj_add_event_cb(plus, on_plus, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(plus, on_plus, LV_EVENT_LONG_PRESSED_REPEAT, NULL);

  // value (tap to type)
  lv_obj_t *val = box(scr, 505, 304, 199, 64, COL_BG, RAD_CTRL);
  lv_obj_set_style_bg_opa(val, LV_OPA_TRANSP, 0);
  make_button(val, COL_CARD_PR);
  lv_obj_set_style_bg_opa(val, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_t *vrow = flex(val, LV_FLEX_FLOW_ROW, 6);
  lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
  lv_obj_center(vrow);
  g_value_lbl = label(vrow, "21.0", F_VALUE, COL_TEXT);
  lv_obj_t *unit = label(vrow, "g", F_ROW, COL_MUTED);
  g_value_unit = unit;
  lv_obj_set_style_pad_bottom(unit, base_pad(F_VALUE, F_ROW), 0);
  lv_obj_add_event_cb(val, on_open_editor, LV_EVENT_CLICKED, NULL);

  // profile preview
  // colour key for the preview (and every chart): what blue and gold mean
  lv_obj_t *key = flex(scr, LV_FLEX_FLOW_ROW, 8);
  lv_obj_set_flex_align(key, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_pos(key, 433, 96);
  lv_obj_set_height(key, 24);
  static const struct { lv_color_t c; const char *t; } KEY[2] = {{COL_PRESS, "Pressure"}, {COL_FLOW, "Flow"}};
  for (int i = 0; i < 2; i++) {
    lv_obj_t *sw = icon_box(key, 14, 4);
    box(sw, 0, 0, 14, 3, KEY[i].c, 1);
    lv_obj_t *kl = label(key, KEY[i].t, F_TEXT, COL_MUTED);
    if (i == 0) lv_obj_set_style_pad_right(kl, 12, 0);
  }
  lv_obj_t *chart = icon_box(scr, PV_W, PV_H);
  lv_obj_set_pos(chart, 433, 124);
  g_chart = chart;
  g_pv_msg = label(chart, "Loading...", F_TEXT, COL_MUTED);
  lv_obj_center(g_pv_msg);
  lv_obj_set_hidden(g_pv_msg, true);
  box(scr, 433, 124 + PV_H, PV_W, 1, COL_LINE, 0);

  // footer
  lv_obj_t *fl = flex(scr, LV_FLEX_FLOW_ROW, 22);
  lv_obj_set_pos(fl, 24, 406);
  lv_obj_set_height(fl, 48);
  // footer, left: [pressure] [scale] [water] (plain unless orange, foot_pill) ... [Flush]
  lv_obj_t *tank = flex(fl, LV_FLEX_FLOW_ROW, 8);
  g_tank = tank;
  lv_obj_set_height(tank, 40);
  lv_obj_set_style_pad_left(tank, 10, 0);
  lv_obj_set_style_pad_right(tank, 12, 0);
  lv_obj_set_style_bg_color(tank, COL_WARN_BG, 0);
  lv_obj_set_style_bg_opa(tank, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tank, RAD_PILL, 0);
  g_tank_fill = icon_tank(tank, 18, COL_WARN);
  g_tank_lbl = label(tank, "Refill", F_TEXT, COL_WARN_TX);
  lv_obj_t *pct = label(tank, "18%", F_TEXT_B, COL_WARN_TX);
  g_tank_pct = pct;

  // scale pill, styled like the water pill: [type icon] Scale 101.9 g / orange "No scale" / "Predictive". The type comes
  // from the 3 scale flags of d_settings (pb_scale_cfg), presence from scalesPresent
  lv_obj_t *sc = flex(fl, LV_FLEX_FLOW_ROW, 8);
  lv_obj_set_height(sc, 40);
  lv_obj_set_style_pad_left(sc, 12, 0);
  lv_obj_set_style_pad_right(sc, 14, 0);
  lv_obj_set_style_bg_color(sc, COL_CARD, 0);
  lv_obj_set_style_bg_opa(sc, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(sc, RAD_PILL, 0);
  // one icon per scale type, all 14x20 in the same slot: Bluetooth rune, a scale (wired load cells, or type
  // unknown), "approximately" waves for predictive (weight estimated from the pump flow, no scale)
  static const lv_point_t BT_PTS[] = {{2, 5}, {12, 14}, {7, 19}, {7, 1}, {12, 6}, {2, 15}};
  static const lv_point_t HW_PTS[] = {{1, 6}, {13, 6}, {7, 6}, {7, 10}, {3, 10}, {2, 18}, {12, 18}, {11, 10}, {7, 10}};
  static const lv_point_t PR1_PTS[] = {{1, 8}, {4, 6}, {7, 8}, {10, 10}, {13, 8}};
  static const lv_point_t PR2_PTS[] = {{1, 13}, {4, 11}, {7, 13}, {10, 15}, {13, 13}};
  lv_obj_t *slot = lv_obj_create(sc);
  lv_obj_remove_style_all(slot);
  lv_obj_set_size(slot, 14, 20);
  g_sc_ic[0] = line(slot, BT_PTS, 6, 2, COL_OUTLINE);
  g_sc_ic[1] = line(slot, HW_PTS, 9, 2, COL_OUTLINE);
  g_sc_ic[2] = line(slot, PR1_PTS, 5, 2, COL_OUTLINE);
  g_sc_ic[3] = line(slot, PR2_PTS, 5, 2, COL_OUTLINE);
  for (int i = 0; i < 4; i++) lv_obj_set_style_line_rounded(g_sc_ic[i], true, 0);
  for (int i = 1; i < 4; i++) lv_obj_set_hidden(g_sc_ic[i], true);
  g_scale_pill = sc;
  lv_obj_move_to_index(sc, 0);                    // scale first, then water
  g_scale_name = label(sc, "No scale", F_TEXT, COL_OUTLINE);
  g_scale_lbl = label(sc, "0.0 g", F_TEXT_B, COL_TEXT);
  lv_obj_set_hidden(g_scale_lbl, true);
  // pressure pill: always the current pressure, styled
  // like the scale pill; orange like "Refill" while idle in HOME with residual pressure (g_press_warn)
  {
    lv_obj_t *pp = g_pr_pill = flex(fl, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_height(pp, 40);
    lv_obj_set_style_pad_left(pp, 12, 0);
    lv_obj_set_style_pad_right(pp, 14, 0);
    lv_obj_set_style_bg_color(pp, COL_CARD, 0);
    lv_obj_set_style_bg_opa(pp, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pp, RAD_PILL, 0);
    static const lv_point_t ARC[] = {{1, 15}, {2, 11}, {5, 8}, {9, 7}, {13, 8}, {16, 11}, {17, 15}};
    static const lv_point_t NEEDLE[] = {{9, 15}, {13, 10}};
    lv_obj_t *slot = lv_obj_create(pp);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, 18, 20);
    g_pr_ic[0] = line(slot, ARC, 7, 2, COL_OUTLINE);
    g_pr_ic[1] = line(slot, NEEDLE, 2, 2, COL_OUTLINE);
    for (int i = 0; i < 2; i++) lv_obj_set_style_line_rounded(g_pr_ic[i], true, 0);
    g_pr_name = label(pp, "Pressure", F_TEXT, COL_OUTLINE);
    g_pr_lbl = label(pp, "- bar", F_TEXT_B, COL_OUTLINE);
    lv_obj_move_to_index(pp, 0);                  // pressure first, then scale, water
  }
  lv_obj_t *tare = flex(fl, LV_FLEX_FLOW_ROW, 0);
  lv_obj_set_height(tare, 48);
  lv_obj_set_style_pad_left(tare, 18, 0);
  lv_obj_set_style_pad_right(tare, 18, 0);
  lv_obj_set_style_bg_color(tare, COL_CARD, 0);
  lv_obj_set_style_bg_opa(tare, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tare, RAD_CTRL, 0);
  make_button(tare, COL_CARD_PR);
  g_tare_lbl = label(tare, "Tare", F_TEXT_B, COL_TEXT);
  lv_obj_add_event_cb(tare, on_tare, LV_EVENT_CLICKED, NULL);
  // hidden: the machine tares by itself at brew start, and the scale has its own button.
  // The tare path (on_tare -> bd_tare) stays and works; clear this flag to bring the button back.
  lv_obj_set_hidden(tare, true);

  // flush: [drop] Flush, a rounded button; a fill behind the content shows the long press
  lv_obj_t *fb = flex(scr, LV_FLEX_FLOW_ROW, 10);
  lv_obj_set_height(fb, 48);
  lv_obj_set_style_pad_left(fb, 12, 0);
  lv_obj_set_style_pad_right(fb, 16, 0);
  lv_obj_set_style_bg_color(fb, COL_CARD, 0);
  lv_obj_set_style_bg_opa(fb, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(fb, RAD_CTRL, 0);
  lv_obj_set_style_clip_corner(fb, true, 0);
  make_button(fb, COL_CARD_PR);
  g_fl_btn = fb;
  g_fl_fill = box(fb, -12, 0, 0, 48, COL_BLUE, 0);   // x -12: from the button's edge, not its padded content
  lv_obj_set_style_bg_opa(g_fl_fill, 56, 0);       // the blue at 22 %
  lv_obj_set_ignore_layout(g_fl_fill, true);
  lv_obj_set_hidden(g_fl_fill, true);
  lv_obj_t *dc = icon_box(fb, 24, 24);
  g_fl_drop = line(dc, FL_DROP, sizeof FL_DROP / sizeof FL_DROP[0], 2, COL_BLUE);
  g_fl_lbl = label(fb, "Flush", F_TEXT_B, COL_OUTLINE);
  lv_obj_add_event_cb(fb, on_flush_press, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(fb, on_flush_press, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(fb, on_flush_press, LV_EVENT_PRESS_LOST, NULL);
  lv_obj_add_event_cb(fb, on_flush_tap, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(fb, on_flush_hold, LV_EVENT_LONG_PRESSED, NULL);
  lv_timer_create(on_fl_timer, 250, NULL);
  fl_show();
  lv_obj_align(fb, LV_ALIGN_TOP_RIGHT, -24, 406);
}

// ---- the stop weight's commit, the footer's scale / pressure items, Tare and the flush button ----------------------
// Tare (button hidden: the machine tares at the start of a shot, and the scale has its own button; clear its hidden
// flag in ready.cpp to bring it back): bd_tare, only with a scale connected while idle in HOME.
bool g_can_tare = false;
lv_obj_t *g_tare_lbl;
lv_obj_t *g_sc_ic[4], *g_scale_name, *g_scale_pill;   // icons: 0 Bluetooth, 1 scale, 2+3 predictive waves
lv_obj_t *g_pr_pill, *g_pr_ic[2], *g_pr_name, *g_pr_lbl;   // pressure pill (gauge arc + needle, "Pressure 0.2 bar")
// Stop weight: a new value from the keypad (Done) or 1 s after the last -/+ goes to the machine with
// bd_set_stop_weight (only that field of the active profile). Only while online and idle in HOME; the core checks again.
lv_timer_t *g_weight_tm;      // user data 1 while a +/- change waits to be sent
int g_act_weight10 = -1;       // last active-profile weight taken over
void weight_commit(lv_timer_t *t) {
  (void)t;
  if (g_weight_tm) { lv_timer_pause(g_weight_tm); lv_timer_set_user_data(g_weight_tm, NULL); }
  // sent: keep it (the machine reports it ~1 s later); refused by the core (offline, range, unknown profile) or not
  // sendable (busy / offline): back to the machine's value
  if (g_can_select && g_cur_profile[0] && bd_set_stop_weight(prof_id(g_cur_profile), g_weight10 / 10.0f, true) == BD_OK) orig_note();
  else if (g_act_weight10 >= 0 && g_weight10 != g_act_weight10) {   // not sent: back to the machine's
    g_weight10 = clamp_w10(g_act_weight10);
    refresh_value();                                                // value (the screen showed a
  }                                                                 // stop weight the machine never got)
}
void weight_changed(bool now) {
  if (now) { weight_commit(NULL); return; }
  if (!g_weight_tm) g_weight_tm = lv_timer_create(weight_commit, 1000, NULL);
  lv_timer_set_user_data(g_weight_tm, (void *)1);
  lv_timer_reset(g_weight_tm);
  lv_timer_resume(g_weight_tm);
}

void on_tare(lv_event_t *e) { (void)e; if (g_can_tare) bd_tare(); }

// Flush button: a tap turns
// the machine's FLUSH mode on (the brew switch then flushes) or off (-> HOME); a long press asks for FLUSH_AUTO (the
// machine's timed flush, "Auto Flush Duration"). On only while online and idle in HOME; off only from a flush mode.
// The button follows the mode the machine reports; a tap shows the new state at once and reverts after 5 s if the
// machine does not report it.
// Look (interactive elements look different from the info pills, and nothing is circular): a rounded button like Menu / -/+ (48 tall, radius 14), drop +
// one word. Holding fills it from the left in LVGL's long-press time (400 ms), then the timed flush starts; flush mode
// and the timed flush = dark blue button, blue text ("Flush mode on" / "Flushing N s", counting up: the machine's
// duration is in d_settings, not read, so no draining fill).
lv_obj_t *g_fl_btn, *g_fl_lbl, *g_fl_drop, *g_fl_fill;
int g_fl_mode = -1;            // machine's mode: 2 FLUSH, 5 FLUSH_AUTO, 7 HOME, other / -1 = busy or offline
bool g_fl_idle = false;        // online, HOME, nothing running
int g_fl_want = -1;            // mode asked for, until the machine reports it (or the core says it did not take it)
static uint32_t g_fl_auto_ms = 0;     // FLUSH_AUTO seen since
static const lv_color_t FL_DARK = LV_COLOR_MAKE(0x0F, 0x22, 0x33);
static int fl_cur(void) { return g_fl_want >= 0 ? g_fl_want : g_fl_mode; }
void fl_show(void) {
  if (!g_fl_btn) return;
  const int m = fl_cur();
  const bool on = m == 2, aut = m == 5, usable = on || aut || g_fl_idle;
  if (aut && !g_fl_auto_ms) g_fl_auto_ms = stamp_nz(lv_tick_get());
  if (!aut) g_fl_auto_ms = 0;
  char t[24];
  if (aut) snprintf(t, sizeof t, "Flushing %u s", (unsigned)(lv_tick_elaps(g_fl_auto_ms) / 1000 + 1));
  set_text(g_fl_lbl, aut ? t : on ? "Flush mode on" : "Flush");
  set_tc(g_fl_lbl, !usable ? COL_OUTLINE : on || aut ? COL_BLUE : COL_TEXT);
  set_line_c(g_fl_drop, usable ? COL_BLUE : COL_OUTLINE);
  set_bg(g_fl_btn, on || aut ? FL_DARK : COL_CARD);
  lv_obj_set_style_bg_color(g_fl_btn, on || aut ? lv_color_hex(0x16304A) : COL_CARD_PR, LV_STATE_PRESSED);
}
static void fl_ask(int to) {
  g_fl_want = to;
  if (bd_flush_mode(to == 2 ? BD_FLUSH_ON : to == 5 ? BD_FLUSH_TIMED : BD_FLUSH_OFF) != BD_OK) g_fl_want = -1;   // refused
  fl_show();
}
static void fl_fill_cb(void *o, int32_t v) {
  lv_obj_set_width((lv_obj_t *)o, (int32_t)(lv_obj_get_width(g_fl_btn) * v / 100));
}
void on_flush_press(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
    if (!(fl_cur() == 7 && g_fl_idle)) return;      // a tap turns it off: no fill
    lv_obj_set_hidden(g_fl_fill, false);
    lv_anim_t an;                                  // fills in the long-press time, then LONG_PRESSED starts it
    lv_anim_init(&an);
    lv_anim_set_var(&an, g_fl_fill);
    lv_anim_set_exec_cb(&an, fl_fill_cb);
    lv_anim_set_values(&an, 0, 100);
    lv_anim_set_duration(&an, LV_INDEV_DEF_LONG_PRESS_TIME);
    lv_anim_start(&an);
  } else {                                         // RELEASED / PRESS_LOST
    lv_anim_delete(g_fl_fill, fl_fill_cb);
    lv_obj_set_hidden(g_fl_fill, true);
    lv_obj_set_width(g_fl_fill, 0);
  }
}
void on_flush_tap(lv_event_t *e) {          // SHORT_CLICKED: not sent after a long press
  (void)e;
  const int m = fl_cur();
  if (m == 2 || m == 5) fl_ask(7);
  else if (g_fl_idle) fl_ask(2);
}
void on_flush_hold(lv_event_t *e) {
  (void)e;
  if (fl_cur() == 7 && g_fl_idle) fl_ask(5);
}
void on_fl_timer(lv_timer_t *t) {
  (void)t;
  if (fl_cur() == 5) fl_show();                    // seconds count
}

// ---- the keypad (stop weight / brew temperature) ---------------------------------------------------------------------
void build_editor(lv_obj_t *scr) {
  g_editor = box(scr, 0, 0, 800, 480, COL_BG, 0);
  lv_obj_set_hidden(g_editor, true);
  lv_obj_set_clickable(g_editor, true);          // swallow touches meant for the screen below

  // header: Cancel + profile name
  lv_obj_t *cancel = flex(g_editor, LV_FLEX_FLOW_ROW, 8);
  lv_obj_set_pos(cancel, 24, 24);
  lv_obj_set_height(cancel, 52);
  lv_obj_set_style_pad_left(cancel, 14, 0);
  lv_obj_set_style_pad_right(cancel, 20, 0);
  lv_obj_set_style_bg_color(cancel, COL_CARD, 0);
  lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(cancel, RAD_CTRL, 0);
  make_button(cancel, COL_CARD_PR);
  icon_close(cancel, COL_TEXT);
  label(cancel, "Cancel", F_TEXT_B, COL_TEXT);
  lv_obj_add_event_cb(cancel, on_cancel, LV_EVENT_CLICKED, NULL);

  lv_obj_t *pn = label(g_editor, PROFILE_NAME, F_TEXT, COL_MUTED);
  g_ed_prof_lbl = pn;
  lv_obj_align(pn, LV_ALIGN_TOP_RIGHT, -24, 42);

  // left column
  lv_obj_t *tl = label(g_editor, "Stop weight", F_TEXT, COL_MUTED);
  g_ed_title = tl;
  lv_obj_set_pos(tl, 24, 96);

  g_entry_lbl = label(g_editor, "", F_HERO, COL_TEXT);
  lv_obj_set_style_text_letter_space(g_entry_lbl, -5, 0);
  lv_obj_set_pos(g_entry_lbl, 24, 142);

  g_cursor = box(g_editor, 24, 115, 4, 141, COL_GOLD, 2);
  g_unit_lbl = label(g_editor, "g", F_VALUE, COL_MUTED);
  // on the entry's baseline
  lv_obj_set_y(g_unit_lbl, 142 + lv_font_get_line_height(F_HERO) - F_HERO->base_line
                               - (lv_font_get_line_height(F_VALUE) - F_VALUE->base_line));

  g_was_lbl = label(g_editor, "Was 21.0 g", F_TEXT, COL_MUTED);
  lv_obj_set_pos(g_was_lbl, 24, 256);

  g_reset_btn = flex(g_editor, LV_FLEX_FLOW_ROW, 8);   // "Reset to 21.0 g" (originals), like Cancel
  lv_obj_set_pos(g_reset_btn, 24, 296);
  lv_obj_set_height(g_reset_btn, 52);
  lv_obj_set_style_pad_hor(g_reset_btn, 20, 0);
  lv_obj_set_style_bg_color(g_reset_btn, COL_CARD, 0);
  lv_obj_set_style_bg_opa(g_reset_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_reset_btn, RAD_CTRL, 0);
  lv_obj_set_flex_align(g_reset_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  make_button(g_reset_btn, COL_CARD_PR);
  g_reset_lbl = label(g_reset_btn, "Reset to 21.0 g", F_TEXT_B, COL_TEXT);
  lv_obj_add_event_cb(g_reset_btn, on_reset, LV_EVENT_CLICKED, NULL);
  lv_obj_set_hidden(g_reset_btn, true);

  lv_obj_t *done = box(g_editor, 24, 384, 328, 72, COL_GOLD, RAD_CTRL);
  make_button(done, COL_GOLD_PR);
  lv_obj_t *dl = label(done, "Done", F_ROW_B, COL_BG);
  lv_obj_center(dl);
  lv_obj_add_event_cb(done, on_done, LV_EVENT_CLICKED, NULL);

  // keypad 3 x 4
  static const char keys[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', 'b'};
  static const int col_x[3] = {392, 523, 655};   // 24 px margin: keys 121 x 82, to x 776 / y 456
  static const int col_w[3] = {121, 122, 121};
  static const int row_y[4] = {96, 189, 281, 374};
  for (int i = 0; i < 12; i++) {
    int c = i % 3, r = i / 3;
    lv_obj_t *k = box(g_editor, col_x[c], row_y[r], col_w[c], 82, COL_CARD, RAD_CTRL);
    make_button(k, COL_CARD_PR);
    if (keys[i] == 'b') {
      lv_obj_t *wrap = flex(k, LV_FLEX_FLOW_ROW, 0);
      lv_obj_center(wrap);
      icon_backspace(wrap, COL_TEXT);
    } else {
      char t[2] = {keys[i], 0};
      lv_obj_t *kl = label(k, t, F_VALUE, COL_TEXT);
      lv_obj_center(kl);
      if (keys[i] == '.') { g_key_dot = k; g_key_dot_lbl = kl; }
    }
    lv_obj_add_event_cb(k, on_key, LV_EVENT_CLICKED, (void *)(intptr_t)keys[i]);
  }
  lv_timer_create(on_blink, 530, NULL);
}
