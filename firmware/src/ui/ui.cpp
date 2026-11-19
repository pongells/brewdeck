// The screens (LVGL): Ready, keypads, profile picker, brewing, shot review, steam, menu, Wi-Fi, offline, idle,
// splash. 800 x 480, 24 px margin on every screen. This file: ui_init and the live data from the core
// (ui_set_live); each screen is in screens/, what they share in ui_int.h.
#include "ui_int.h"

static const float PRESS_ON = 0.8f;          // bar: the pressure pill turns orange while it reads above 0.8
static const uint32_t PRESS_OFF_MS = 2000;   // reads 0.8 or less this long -> off (no flicker between 0.8 and 0.9)

void ui_init(void) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(scr, false);
  build_ready(scr);
  build_steam(scr);     // above Ready, below the editor / picker / menu
  build_editor(scr);
  build_picker(scr);
  build_menu(scr);
  lv_obj_add_event_cb(g_menu_btn, on_menu_open, LV_EVENT_CLICKED, NULL);
  build_brew(scr);
  build_offline(scr);   // drawn above everything else while shown
  build_idle(scr);
  build_blank(scr);     // screen off: above everything
  build_notif();        // machine notifications: top layer, over all of it
  build_frame();        // pressure-release warning frame (top layer)
  refresh_value();
}

// ---- live data ---------------------------------------------------------------------------------
static void set_pill(lv_color_t bg, lv_color_t dot, lv_color_t tx, const char *text) {
  set_bg(g_pill, bg);
  set_bg(g_pill_dot, dot);
  set_tc(g_pill_lbl, tx);
  set_text(g_pill_lbl, text);
}

// the core's answer to a command (BD_EV_COMMAND): not taken -> back to what the machine has (a newer change made on
// the screen since, still waiting to be sent, is kept)
void ui_command_answer(int cmd, int answer) {
  if (answer == BD_ANSWER_OK) return;              // ui_set_live follows the machine anyway
  if (cmd == BD_CMD_SELECT && g_pend_id) {
    g_pend_id = 0;
    for (int i = 0; i < g_profiles.count; i++)
      g_profiles.p[i].selected = strcmp(g_profiles.p[i].name, g_menu_live.profile) == 0;
    if (g_menu_live.profile[0]) show_profile(g_menu_live.profile);
    g_shape_id = -1;                               // re-apply its weight too
    if (g_shape_loaded) ui_set_shape(&g_last_shape);
  } else if (cmd == BD_CMD_FLUSH && g_fl_want >= 0) {
    g_fl_want = -1;
    fl_show();
  } else if (cmd == BD_CMD_STOP_WEIGHT && g_act_weight10 >= 0 &&
             !timer_pending(g_weight_tm)) {
    g_weight10 = clamp_w10(g_act_weight10);
    refresh_value();
  } else if (cmd == BD_CMD_BREW_TEMP && g_act_temp > 0 && !timer_pending(g_temp_tm)) {
    g_tset = g_act_temp;
    refresh_tset();
  }
}

// Called on every machine-state change (several times a second); only widgets whose value changed are touched.
void ui_set_live(const LiveData *d, int link) {
  char b[64];
  LiveData safe;                                    // a boiler reading that is not a number in 0..400 (a sensor fault)
  if (!(d->temperature >= 0.0f && d->temperature < 400.0f)) {   // is shown as 0: the big digits font has no "-" or
    safe = *d;                                      // "nan", and NaN * 10 into int16 (steam chart) is undefined
    safe.temperature = 0.0f;
    d = &safe;
  }
  g_live_brew = link == LINK_ONLINE && d->brew_active;   // brewing screen: "Done" once this drops after a shot
  if (g_live_brew && g_br_state == 1) g_br_seen_brew = true;
  static bool pv_msg_shown = false;                // the preview's "loading" line, once, until a profile arrives
  if (!g_shape_loaded && !pv_msg_shown) {
    pv_msg_shown = true;
    lv_obj_set_hidden(g_pv_msg, false);
  }
  g_menu_live = *d;
  g_menu_link = link;
  g_off_active = true;
  if (link != g_off_link) {                         // offline screen: remember when the link was lost
    if (link != LINK_ONLINE && g_off_link == LINK_ONLINE) { g_off_since = lv_tick_get(); g_upd_shown = false; }
    if (link != LINK_ONLINE) g_off_phase = lv_tick_get();
    if (link == LINK_ONLINE) g_off_ever_online = true;
    g_off_link = link;
  }
  g_can_select = false;
  g_can_tare = false;
  if (link != LINK_ONLINE) {
    g_fl_idle = false;
    g_fl_mode = -1;
    g_fl_want = -1;
    fl_show();
    // Ready itself keeps its last look: a short drop passes unnoticed, a longer one brings the offline screen after
    // OFF_DELAY_MS (a greyed "Offline" Ready in between only added a third look). Commands are refused above (g_can_*)
    // and again in the network task.
    g_idle_busy = false;                            // offline: nothing runs that we know of; a busy flag left from
    return;                                         // steam / flush kept the screen lit all night
  }
  // Ready / Warming up / Heating / Standby / Waiting from the machine's readiness (the names of its web UI);
  // the temperature rule only when it is not reported. "Cooling" (boiler > target + 3, after steam) stays the
  // screen's own: the machine has no such state.
  const int rd = d->readiness;
  const bool rd_ok = rd >= 0 && rd <= 7;
  const bool cooling = d->target > 0 && d->temperature > d->target + 3.0f && rd != 1;
  const bool heating_now = rd_ok ? (rd == 1 || rd == 2) && !cooling
                                 : d->warmup_remaining > 0 || (d->target > 0 && d->temperature < d->target - 3.0f);
  // Ready only after READY_HOLD_MS of it without a break (while warming up the machine's readiness
  // goes READY / HEATING a few times as the boiler overshoots and dips, and Ready popped up in between). Until then
  // the pill (and the idle clock) keep saying Heating / Warming up; a state other than READY (Cooling, Standby, ...) ends it.
  static const uint32_t READY_HOLD_MS = 30000;
  static bool s_hold = false;                 // a heating phase was seen and Ready has not been steady yet
  static uint32_t s_ready_since = 0;
  static int s_hold_rd = 2;                   // what the pill said before (1 Warming up / 2 Heating)
  static uint32_t s_heat_since = 0;
  // only a real heating phase arms the hold (Warming up, or Heating for 10 s in a row): at temperature the machine also
  // reports HEATING for a second or so now and then, which must not keep Heating up for 30 s
  if (heating_now) {
    if (!s_heat_since) s_heat_since = stamp_nz(lv_tick_get());
    if (rd == 1 || lv_tick_elaps(s_heat_since) >= 10000) { s_hold = true; s_hold_rd = rd == 1 ? 1 : 2; }
    s_ready_since = 0;
  } else s_heat_since = 0;
  if (!heating_now && s_hold) {
    if (cooling || !rd_ok || rd != 3) s_hold = false;              // not READY (cooling, standby, ...): show that
    else if (!s_ready_since) s_ready_since = stamp_nz(lv_tick_get());
    else if (lv_tick_elaps(s_ready_since) >= READY_HOLD_MS) {
      s_hold = false;
      Serial_log("ui: ready steady for 30 s, showing Ready");
    }
  }
  const bool heating = heating_now || s_hold;
  const char *state = "Ready";
  bool busy = true;
  if (d->brew_active) state = "Brewing";
  else if (d->steam_active || strcmp(d->mode, "STEAM") == 0) state = "Steam";
  else if (d->hot_water_active) state = "Hot water";
  else if (strncmp(d->mode, "FLUSH", 5) == 0) state = "Flush";
  else if (strcmp(d->mode, "DESCALE") == 0) state = "Descale";
  else busy = false;
  st_update(d, (d->steam_active || strcmp(d->mode, "STEAM") == 0) && !d->brew_active);
  g_can_select = !busy && strcmp(d->mode, "HOME") == 0;
  {   // flush button: the machine's mode
    const int fm = !strcmp(d->mode, "FLUSH") ? 2 : !strcmp(d->mode, "FLUSH_AUTO") ? 5 : !strcmp(d->mode, "HOME") ? 7 : -1;
    g_fl_mode = fm;
    g_fl_idle = fm == 7 && !busy;
    if (g_fl_want == fm) g_fl_want = -1;           // reported: follow the machine again
    fl_show();
  }
  // flush mode left on with no water running does not keep the screen awake (only a running pump counts)
  g_idle_busy = busy && !(strncmp(d->mode, "FLUSH", 5) == 0 && !d->brew_active && d->pressure < 0.5f);
  // a shot / steam / flush counts as activity: "Dim after" starts again when it ends (else steaming without touching
  // the screen goes straight to the idle clock when steam ends)
  if (busy) lv_display_trigger_activity(NULL);
  // residual pressure (the next shot would not start from nominal pressure): idle in HOME and the pill reads above
  // PRESS_ON -> the pressure pill turns orange and the idle clock shows the pressure, until it reads PRESS_ON or less
  // for PRESS_OFF_MS (the machine releases it itself, after "Dropping beats"). Measured: idle 0.14-0.60 bar, after
  // steam 1.7-2.9; the core logs the idle pressure range every minute.
  {
    const bool idle_home = !busy && !strcmp(d->mode, "HOME");
    const bool was = g_press_warn;
    if (!idle_home) g_press_warn = false;
    else {
      // compare what the pill shows (one decimal): orange exactly when it reads 0.9 bar or more
      static uint32_t s_low_since = 0;
      const bool above = (int)(d->pressure * 10.0f + 0.5f) > (int)(PRESS_ON * 10.0f + 0.5f);
      if (above) { g_press_warn = true; s_low_since = 0; }
      else if (g_press_warn) {
        if (!s_low_since) s_low_since = stamp_nz(lv_tick_get());
        else if (lv_tick_elaps(s_low_since) >= PRESS_OFF_MS) { g_press_warn = false; s_low_since = 0; }
      }
    }
    if (g_press_warn != was) {
      char m[64];
      snprintf(m, sizeof m, "ui: pressure warning %s (%.2f bar)", g_press_warn ? "on" : "off", d->pressure);
      Serial_log(m);
    }
    g_press_val = d->pressure;
    // the pressure pill: orange while the warning is on, else grey with the blue gauge (pressure colour)
    snprintf(b, sizeof b, "%.1f bar", d->pressure < 0 ? 0.0f : d->pressure);
    set_text(g_pr_lbl, b);
    set_bg(g_pr_pill, COL_WARN_BG);
    foot_pill(g_pr_pill, g_press_warn, 12, 14);
    for (int i = 0; i < 2; i++) set_line_c(g_pr_ic[i], g_press_warn ? COL_WARN : COL_PRESS);
    set_tc(g_pr_name, g_press_warn ? COL_WARN_TX : COL_MUTED);
    set_tc(g_pr_lbl, g_press_warn ? COL_WARN_TX : COL_TEXT);
  }
  g_idle_word = heating ? 1 : cooling ? 2 : 0;
  g_idle_state = g_press_warn ? 3 : g_idle_word;
  snprintf(g_idle_temp, sizeof g_idle_temp, "%.1f", d->temperature);
  g_can_tare = g_can_select && d->scales_present;
  set_tc(g_tare_lbl, g_can_tare ? COL_TEXT : COL_OUTLINE);
  if (busy) set_pill(COL_BLUE_BG, COL_BLUE, COL_BLUE, state);
  else if (heating) set_pill(lv_color_hex(0x2A2210), COL_GOLD, COL_GOLD, (heating_now ? rd : s_hold_rd) == 1 ? "Warming up" : "Heating");
  else if (rd == 4) set_pill(COL_CARD, COL_MUTED, COL_PALE_TX, "Standby");
  else if (rd == 0) set_pill(COL_CARD, COL_MUTED, COL_PALE_TX, "Waiting");
  else if (rd == 7) set_pill(COL_BLUE_BG, COL_BLUE, COL_BLUE, "Maintenance");
  else if (cooling)   // after steam
    set_pill(COL_CARD, COL_BLUE, COL_PALE_TX, "Cooling");
  else set_pill(COL_PILL_BG, COL_GREEN, COL_PILL_TX, "Ready");

  // temperature (the state is the pill's job)
  if (d->temperature >= 99.95f) snprintf(b, sizeof b, "%.0f", d->temperature);
  else snprintf(b, sizeof b, "%.1f", d->temperature);
  set_text(g_temp_lbl, b);
  set_tc(g_temp_lbl, COL_TEXT);

  // water tank
  bool low = d->water_level < 20;
  int lvl = d->water_level < 0 ? 0 : (d->water_level > 100 ? 100 : d->water_level);
  int fh = 16 * lvl / 100;
  if (fh < 2) fh = 2;
  set_h(g_tank_fill, fh);
  set_y(g_tank_fill, 16 - fh);
  set_bg(g_tank_fill, low ? COL_WARN : COL_BLUE);
  set_bg(g_tank, COL_WARN_BG);
  foot_pill(g_tank, low, 10, 12);
  set_tc(g_tank_lbl, low ? COL_WARN_TX : COL_MUTED);
  set_tc(g_tank_pct, low ? COL_WARN_TX : COL_TEXT);
  set_text(g_tank_lbl, low ? "Refill" : "Water");
  snprintf(b, sizeof b, "%d%%", lvl);
  set_text(g_tank_pct, b);

  // profile (shown without the "(community)" suffix, as in the design) + scale
  if (g_pend_id) {
    if (strcmp(d->profile, g_pend_name) == 0) g_pend_id = 0;          // confirmed by the machine
  }
  if (d->profile[0] && !g_pend_id) show_profile(d->profile);
  // the profile the values below belong to: a pick made here counts at once, so the machine's d_act_prof (WS, pushed
  // right at the switch) is taken without waiting for the REST status (~1 s poll) to name it too (temp
  // and stop weight took ~1 s to follow a pick otherwise; measured: d_act_prof at the switch, REST 0.3-0.4 s later + poll)
  const char *vals_for = g_pend_id ? g_pend_name : d->profile;
  // brew temperature: the active profile's (d_act_prof), REST targetTemperature until that is known; a pending
  // -/+ change is kept until it is sent
  {
    int t = -1;
    if (d->act_temp > 0 && !strcmp(d->act_profile, vals_for)) t = (int)(d->act_temp + 0.5f);
    else if (d->target > 0) t = (int)(d->target + 0.5f);
    if (t != g_act_temp) {
      g_act_temp = t;
      if (!timer_pending(g_temp_tm)) { g_tset = t; refresh_tset(); }
    }
  }
  // target weight: the ACTIVE profile's (d_act_prof) wins over the stored one (REST): a weight set from this screen
  // or the web UI's dashboard lives only in the active profile. Taken over when it changes, like ui_set_shape.
  // 0 = the profile has no stop weight (d_act_prof is known once its temperature is)
  if ((d->act_weight > 0 || d->act_temp > 0) && !strcmp(d->act_profile, vals_for)) {
    const int w10 = (int)(d->act_weight * 10.0f + 0.5f);
    if (w10 != g_act_weight10) {
      g_act_weight10 = w10;
      if (!timer_pending(g_weight_tm)) {
        g_weight10 = clamp_w10(w10);
        refresh_value();
      }
    }
  }
  if (!lv_obj_is_hidden(g_pick)) picker_rebuild();   // open: the rows follow the machine (rebuilt only when what they show changed)
  // a restore waiting for its profile to become active (Profiles page, snap_*): send its values once the machine runs
  // it (the core checks the active profile too), or give up after 10 s
  if (g_rst.id) {
    if (!strcmp(d->act_profile, g_rst.name) && g_can_select) snap_apply();
    else if (lv_tick_elaps(g_rst.t) > 10000) { g_rst.id = 0; Serial_log("ui: restore dropped (profile not active)"); }
  }
  // no scale connected: the machine keeps reporting the last reading (seen: 389.4 g long after the scale went
  // away), so the weight is hidden and the pill says "No scale"
  // scale pill: predictive (no scale used, the weight is only estimated during a shot) / connected / not connected
  const bool pred = d->scale_type == SCALE_PREDICTIVE;
  const int ic = pred ? 2 : (d->scale_type == SCALE_BT ? 0 : 1);
  // "No scale" is orange like "Refill" (a missing scale is a problem); predictive is a choice: grey
  const bool noscale = !pred && !d->scales_present;
  const lv_color_t icc = pred ? COL_MUTED : (d->scales_present ? COL_BLUE : COL_WARN);
  set_bg(g_scale_pill, COL_WARN_BG);
  foot_pill(g_scale_pill, noscale, 12, 14);
  set_hidden(g_sc_ic[0], ic != 0);
  set_hidden(g_sc_ic[1], ic != 1);
  set_hidden(g_sc_ic[2], ic != 2);
  set_hidden(g_sc_ic[3], ic != 2);
  for (int i = 0; i < 4; i++) set_line_c(g_sc_ic[i], icc);
  set_text(g_scale_name, pred ? "Predictive" : (d->scales_present ? "Scale" : "No scale"));
  set_tc(g_scale_name, noscale ? COL_WARN_TX : COL_MUTED);
  set_hidden(g_scale_lbl, pred || !d->scales_present);
  set_tc(g_scale_lbl, COL_TEXT);
  if (d->scales_present) {
    snprintf(b, sizeof b, "%.1f g", d->weight);
    set_text(g_scale_lbl, b);
  }
}

// ---- test hooks --------------------------------------------------------------------------------
void ui_test_open_editor(void) { editor_open(false); }
void ui_test_open_temp_editor(void) { editor_open(true); }
void ui_test_key(char key)     { editor_key(key); }
void ui_test_done(void)        { editor_done(); }
void ui_test_cancel(void)      { editor_close(); }
void ui_test_step(int tenths)  { step_weight(tenths); }
void ui_test_idle(void) { g_idle_force = true; }
void ui_test_wake(void) { lv_display_trigger_activity(NULL); idle_hide(); }
// benchmark (debug /bench): shows screen i (0..), NULL past the last; the caller times a full redraw of it
const char *ui_bench_show(int i) {
  static const char *const N[] = {"ready", "profiles", "menu", "about", "wifi list", "shot history", "weight keypad",
                                  "idle clock", "ready + glow"};
  ui_bench_glow(false);
  if (i < 0 || i >= (int)(sizeof N / sizeof N[0])) { ui_test_home(); ui_test_wake(); return NULL; }
  ui_test_home();
  ui_test_wake();
  switch (i) {
    case 1: ui_test_profiles(1, 0); break;
    case 2: ui_test_menu(-1); break;
    case 3: ui_test_menu(3); break;
    case 4: ui_test_menu(2); break;
    case 5: ui_test_menu(7); break;
    case 6: ui_test_open_editor(); break;
    case 7: ui_test_idle(); break;
    case 8: ui_bench_glow(true); break;
    default: break;
  }
  return N[i];
}
void ui_test_home(void) {                     // close every overlay: back to Ready/Heating (emulator)
  if (g_br_state != 0) br_hide();
  menu_close();
  picker_close();
  editor_close();
  idle_hide();
}
void ui_test_wifi_password(const char *ssid) {
  on_menu_open(NULL);
  snprintf(g_wj_ssid, sizeof g_wj_ssid, "%s", ssid);
  memset(g_pw, 0, sizeof g_pw);
  g_wj_state = WIFI_JOIN_IDLE;
  g_layer = 0; g_shift = g_caps = false; g_pw_show = true;
  menu_show_page(5);
}
void ui_test_menu(int page) {                // -2 close, -1 main list, 0.. pages
  if (page == -2) { menu_close(); return; }
  if (page == 6) { open_address_page(); return; }
  on_menu_open(NULL);
  if (page >= 0) menu_show_page(page);
}
void ui_test_review(int mode, int t10) {
  if (g_br_state != 3 || !g_br_n) return;
  if (mode == 0 || g_rv_prev_state == RV_PREV_READY) g_rv_mode = mode;
  int best = 0;
  for (int i = 1; i < g_br_n; i++) if (abs((int)g_br[i].t - t10) < abs((int)g_br[best].t - t10)) best = i;
  g_rv_i = best;
  rv_axis();
  rv_values();
  rv_toggle_style();
  lv_obj_invalidate(g_rv_chart);
}

void ui_test_profiles(int open, int all) {
  (void)all;                                       // one full list
  if (open) picker_open();
  else picker_close();
}
void ui_test_cursor(int on) {
  g_cursor_on = on != 0;
  lv_obj_set_style_bg_opa(g_cursor, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}
