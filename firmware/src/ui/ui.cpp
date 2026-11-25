// Brewdeck's screens (800 x 480, 24 px margin): ui_init and the live data (ui_set_live); screens in screens/.
#include "ui_int.h"

static const float PRESS_ON = 0.8f;          // bar: the pressure pill turns orange while it reads above 0.8
static const uint32_t PRESS_OFF_MS = 2000;   // reads 0.8 or less this long -> off (no flicker between 0.8 and 0.9)
// degC: boiler above target + this = Cooling; below target - this = Heating (when the machine reports no readiness)
static const float TEMP_MARGIN = 3.0f;
static const uint32_t READY_HOLD_MS = 30000; // after a heating phase, Ready shows once READY held this long
static const uint32_t HEAT_ARM_MS = 10000;   // Heating this long in a row is a heating phase (arms the Ready hold)
static const float FLUSH_WATER_BAR = 0.5f;   // flush mode with less pressure than this: no water running
static const uint32_t RESTORE_WAIT_MS = 10000;   // a restore waits this long for its profile to become active

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
  build_update();       // a firmware update arriving over Wi-Fi: top layer, above all of it
  refresh_value();
}

// ---- live data ------------------------------------------------------------------------------------------------------
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
      g_profiles.p[i].active = strcmp(g_profiles.p[i].name, g_menu_live.profile_name) == 0;
    if (g_menu_live.profile_name[0]) show_profile(g_menu_live.profile_name);
    g_shape_id = -1;                               // re-apply its weight too
    if (g_shape_loaded) ui_set_shape(&g_last_shape);
  } else if (cmd == BD_CMD_FLUSH && g_fl_want != FL_UNKNOWN) {
    g_fl_want = FL_UNKNOWN;
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

// ---- live machine state -------------------------------------------------------------------------------------------
// the link: the offline screen's timers; offline, everything that needs the machine is off. false = offline
static bool live_link(int link) {
  g_off_active = true;
  if (link != g_off_link) {                         // offline screen: remember when the link was lost
    if (link != BD_LINK_ONLINE && g_off_link == BD_LINK_ONLINE) { g_off_since = lv_tick_get(); g_upd_shown = false; }
    if (link != BD_LINK_ONLINE) g_off_phase = lv_tick_get();
    if (link == BD_LINK_ONLINE) g_off_ever_online = true;
    g_off_link = link;
  }
  g_can_select = false;
  g_can_tare = false;
  if (link != BD_LINK_ONLINE) {
    g_cmd_ready = g_can_change = false;
    steppers_paint();
    g_fl_idle = false;
    g_fl_mode = FL_UNKNOWN;
    g_fl_want = FL_UNKNOWN;
    fl_show();
    // Ready keeps its last look: a short drop passes unnoticed, a longer one brings the offline screen (offline.cpp).
    // Commands are refused (g_can_*), and again by the core.
    // Nothing runs that we know of: a busy flag left from steam / flush would keep the screen lit all night.
    g_idle_busy = false;
    return false;
  }
  return true;
}

// the heating state: the machine's readiness, held at Heating until Ready is steady
typedef struct { int rd; bool cooling, heating_now, heating; int hold_rd; } LiveHeat;
static LiveHeat live_heat(const bd_machine *d) {
  // Ready / Warming up / Heating / Standby / Waiting from the machine's readiness (the names of its web UI);
  // the temperature rule only when it is not reported. "Cooling" (boiler > target + 3, after steam) stays the
  // screen's own: the machine has no such state.
  LiveHeat h;
  const int rd = h.rd = d->readiness;
  const bool rd_ok = rd >= 0 && rd <= 7;
  const bool cooling = d->target_temp > 0 && d->temperature > d->target_temp + TEMP_MARGIN && rd != 1;
  const bool heating_now = rd_ok ? (rd == 1 || rd == 2) && !cooling
                                 : d->warmup_remaining > 0 ||
                                       (d->target_temp > 0 && d->temperature < d->target_temp - TEMP_MARGIN);
  // After a heating phase, Ready only after READY_HOLD_MS of it without a break: while warming up the machine's
  // readiness goes READY / HEATING a few times as the boiler overshoots and dips. Until then the pill (and the idle
  // clock) keep saying Heating / Warming up; a state other than READY (Cooling, Standby, ...) ends it.
  static bool s_hold = false;                 // a heating phase was seen and Ready has not been steady yet
  static uint32_t s_ready_since = 0;
  static int s_hold_rd = 2;                   // what the pill said before (1 Warming up / 2 Heating)
  static uint32_t s_heat_since = 0;
  // only a real heating phase arms the hold (Warming up, or Heating for HEAT_ARM_MS in a row): at temperature the
  // machine also reports HEATING for a second or so now and then
  if (heating_now) {
    if (!s_heat_since) s_heat_since = stamp_nz(lv_tick_get());
    if (rd == 1 || lv_tick_elaps(s_heat_since) >= HEAT_ARM_MS) { s_hold = true; s_hold_rd = rd == 1 ? 1 : 2; }
    s_ready_since = 0;
  } else s_heat_since = 0;
  if (!heating_now && s_hold) {
    if (cooling || !rd_ok || rd != 3) s_hold = false;              // not READY (cooling, standby, ...): show that
    else if (!s_ready_since) s_ready_since = stamp_nz(lv_tick_get());
    else if (lv_tick_elaps(s_ready_since) >= READY_HOLD_MS) {
      s_hold = false;
      bd_log("ui: ready steady for %u s, showing Ready", (unsigned)(READY_HOLD_MS / 1000));
    }
  }
  h.cooling = cooling;
  h.heating_now = heating_now;
  h.heating = heating_now || s_hold;
  h.hold_rd = s_hold_rd;
  return h;
}

// what the machine is busy with ("Brewing", "Steam", ...; "Ready" and busy = false when idle)
static const char *live_activity(const bd_machine *d, bool *busy) {
  const char *state = "Ready";
  *busy = true;
  if (d->brewing) state = "Brewing";
  else if (d->steaming || strcmp(d->mode, "STEAM") == 0) state = "Steam";
  else if (d->hot_water) state = "Hot water";
  else if (strncmp(d->mode, "FLUSH", 5) == 0) state = "Flush";
  else if (strcmp(d->mode, "DESCALE") == 0) state = "Descale";
  else *busy = false;
  return state;
}

// the flush button follows the machine's mode; flush mode without water running does not count as busy
static void live_flush(const bd_machine *d, bool busy) {
  const int fm = !strcmp(d->mode, "FLUSH") ? BD_FLUSH_ON : !strcmp(d->mode, "FLUSH_AUTO") ? BD_FLUSH_TIMED
               : !strcmp(d->mode, "HOME") ? BD_FLUSH_OFF : FL_UNKNOWN;
  g_fl_mode = fm;
  g_fl_idle = fm == BD_FLUSH_OFF && !busy && d->commands_ready;
  if (g_fl_want == fm) g_fl_want = FL_UNKNOWN;          // reported: follow the machine again
  fl_show();
  // flush mode left on with no water running does not keep the screen awake (only a running pump counts)
  g_idle_busy = busy && !(strncmp(d->mode, "FLUSH", 5) == 0 && !d->brewing && d->pressure < FLUSH_WATER_BAR);
  // a shot / steam / flush counts as activity: "Dim after" starts again when it ends, so the screen does not go
  // straight to the idle clock after steaming without a touch
  if (busy) lv_display_trigger_activity(NULL);
}

// residual pressure (the next shot would not start from nominal pressure): idle in HOME and the pill reads above
// PRESS_ON -> the pressure pill turns orange and the idle clock shows the pressure, until it reads PRESS_ON or less
// for PRESS_OFF_MS (the machine releases it itself, after "Dropping beats"). Idle is ~0.1-0.6 bar, after steam
// 1.7-2.9; the core logs the idle pressure range every minute.
static void live_pressure(const bd_machine *d, bool busy) {
  char b[32];
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
  if (g_press_warn != was) bd_log("ui: pressure warning %s (%.2f bar)", g_press_warn ? "on" : "off", d->pressure);
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

// the state pill, the idle clock's state word and temperature, the Tare button
static void live_state(const bd_machine *d, const LiveHeat *h, bool busy, const char *state) {
  g_idle_word = h->heating ? 1 : h->cooling ? 2 : 0;
  g_idle_state = g_press_warn ? 3 : g_idle_word;
  snprintf(g_idle_temp, sizeof g_idle_temp, "%.1f", d->temperature);
  g_can_tare = g_can_change && d->scale_present;
  set_tc(g_tare_lbl, g_can_tare ? COL_TEXT : COL_OUTLINE);
  if (busy) set_pill(COL_BLUE_BG, COL_BLUE, COL_BLUE, state);
  else if (h->heating)
    set_pill(lv_color_hex(0x2A2210), COL_GOLD, COL_GOLD,
             (h->heating_now ? h->rd : h->hold_rd) == 1 ? "Warming up" : "Heating");
  else if (h->rd == 4) set_pill(COL_CARD, COL_MUTED, COL_PALE_TX, "Standby");
  else if (h->rd == 0) set_pill(COL_CARD, COL_MUTED, COL_PALE_TX, "Waiting");
  else if (h->rd == 7) set_pill(COL_BLUE_BG, COL_BLUE, COL_BLUE, "Maintenance");
  else if (h->cooling) set_pill(COL_CARD, COL_BLUE, COL_PALE_TX, "Cooling");   // after steam
  else set_pill(COL_PILL_BG, COL_GREEN, COL_PILL_TX, "Ready");

}

// the big temperature and the water pill
static void live_temp_water(const bd_machine *d) {
  char b[32];
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

}

// the active profile's name, brew temperature and stop weight; the Profiles page and a waiting restore
static void live_profile(const bd_machine *d) {
  // profile (shown without the "(community)" suffix, as in the design) + scale
  if (g_pend_id) {
    if (strcmp(d->profile_name, g_pend_name) == 0) g_pend_id = 0;          // confirmed by the machine
  }
  if (d->profile_name[0] && !g_pend_id) show_profile(d->profile_name);
  // the profile the values below belong to: a pick made here counts at once, so the machine's running profile (pushed
  // right at the switch) is taken without waiting ~1 s for the status poll to name it too
  const char *vals_for = g_pend_id ? g_pend_name : d->profile_name;
  // brew temperature: the active profile's (d_act_prof), REST targetTemperature until that is known; a pending
  // -/+ change is kept until it is sent
  {
    int t = -1;
    if (d->running.brew_temp > 0 && !strcmp(d->running.name, vals_for)) t = (int)(d->running.brew_temp + 0.5f);
    else if (d->target_temp > 0) t = (int)(d->target_temp + 0.5f);
    if (t != g_act_temp) {
      g_act_temp = t;
      if (!timer_pending(g_temp_tm)) { g_tset = t; refresh_tset(); }
    }
  }
  // stop weight: the ACTIVE profile's (d_act_prof) wins over the stored one (REST): a weight set from this screen
  // or the web UI's dashboard lives only in the active profile. Taken over when it changes, like ui_set_shape.
  // 0 = the profile has no stop weight (d_act_prof is known once its temperature is)
  if ((d->running.stop_weight > 0 || d->running.brew_temp > 0) && !strcmp(d->running.name, vals_for)) {
    const int w10 = (int)(d->running.stop_weight * 10.0f + 0.5f);
    if (w10 != g_act_weight10) {
      g_act_weight10 = w10;
      if (!timer_pending(g_weight_tm)) {
        g_weight10 = clamp_w10(w10);
        refresh_value();
      }
    }
  }
  // Profiles page open: the rows follow the machine (rebuilt only when what they show changed)
  if (!lv_obj_is_hidden(g_pick)) picker_rebuild();
  // a restore waiting for its profile to become active (Profiles page, snap_*): send its values once the machine runs
  // it (the core checks the active profile too), or give up after RESTORE_WAIT_MS
  if (g_rst.id) {
    if (!strcmp(d->running.name, g_rst.name) && g_can_select) snap_apply();
    else if (lv_tick_elaps(g_rst.t) > RESTORE_WAIT_MS) {
      g_rst.id = 0;
      bd_log("ui: restore dropped (profile not active)");
    }
  }
}

static void live_scale(const bd_machine *d) {
  char b[32];
  // scale pill: predictive (no scale used, the weight is only estimated during a shot) / connected / not connected.
  // With no scale connected the machine keeps reporting the last reading, so the weight is hidden.
  const bool pred = d->scale_type == BD_SCALE_PREDICTIVE;
  const int ic = pred ? 2 : (d->scale_type == BD_SCALE_BT ? 0 : 1);
  // "No scale" is orange like "Refill" (a missing scale is a problem); predictive is a choice: grey
  const bool noscale = !pred && !d->scale_present;
  const lv_color_t icc = pred ? COL_MUTED : (d->scale_present ? COL_BLUE : COL_WARN);
  set_bg(g_scale_pill, COL_WARN_BG);
  foot_pill(g_scale_pill, noscale, 12, 14);
  set_hidden(g_sc_ic[0], ic != 0);
  set_hidden(g_sc_ic[1], ic != 1);
  set_hidden(g_sc_ic[2], ic != 2);
  set_hidden(g_sc_ic[3], ic != 2);
  for (int i = 0; i < 4; i++) set_line_c(g_sc_ic[i], icc);
  set_text(g_scale_name, pred ? "Predictive" : (d->scale_present ? "Scale" : "No scale"));
  set_tc(g_scale_name, noscale ? COL_WARN_TX : COL_MUTED);
  set_hidden(g_scale_lbl, pred || !d->scale_present);
  set_tc(g_scale_lbl, COL_TEXT);
  if (d->scale_present) {
    snprintf(b, sizeof b, "%.1f g", d->weight);
    set_text(g_scale_lbl, b);
  }
}

// Called on every machine-state change (several times a second); only widgets whose value changed are touched.
void ui_set_live(const bd_machine *d) {
  const int link = d->link;
  // a boiler reading that is not a number in 0..400, or a pressure not in 0..20 bar (a sensor fault or a bad frame), is
  // shown as 0: the big digits font has no "-" or "nan", and NaN * 10 or * 100 into int16 (steam chart) is undefined
  bd_machine safe;
  const bool temp_ok = d->temperature >= 0.0f && d->temperature < 400.0f;
  const bool press_ok = d->pressure >= 0.0f && d->pressure <= 20.0f;
  if (!temp_ok || !press_ok) {
    safe = *d;
    if (!temp_ok) safe.temperature = 0.0f;
    if (!press_ok) safe.pressure = 0.0f;
    d = &safe;
  }
  g_live_brew = link == BD_LINK_ONLINE && d->brewing;   // the idle clock wakes for a shot
  static bool pv_msg_shown = false;                // the preview's "loading" line, once, until a profile arrives
  if (!g_shape_loaded && !pv_msg_shown) {
    pv_msg_shown = true;
    lv_obj_set_hidden(g_pv_msg, false);
  }
  g_menu_live = *d;
  g_menu_link = link;
  if (!live_link(link)) return;
  const LiveHeat h = live_heat(d);
  bool busy;
  const char *state = live_activity(d, &busy);
  st_update(d, (d->steaming || strcmp(d->mode, "STEAM") == 0) && !d->brewing);
  g_can_select = !busy && strcmp(d->mode, "HOME") == 0;
  g_cmd_ready = d->commands_ready;
  g_can_change = g_can_select && g_cmd_ready;
  steppers_paint();
  live_flush(d, busy);
  live_pressure(d, busy);
  live_state(d, &h, busy, state);
  live_temp_water(d);
  live_profile(d);
  live_scale(d);
}

// ---- test hooks -----------------------------------------------------------------------------------------------------
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
    case 2: ui_test_menu(MP_LIST); break;
    case 3: ui_test_menu(MP_ABOUT); break;
    case 4: ui_test_menu(MP_WIFI); break;
    case 5: ui_test_menu(MP_HISTORY); break;
    case 6: ui_test_open_editor(); break;
    case 7: ui_test_idle(); break;
    case 8: ui_bench_glow(true); break;
    default: break;
  }
  return N[i];
}
// close every overlay: back to Ready (emulator)
void ui_test_home(void) {
  if (g_br_state != BR_HIDDEN) br_hide();
  menu_close();
  picker_close();
  editor_close();
  idle_hide();
}
void ui_test_wifi_password(const char *ssid) {
  on_menu_open(NULL);
  snprintf(g_wj_ssid, sizeof g_wj_ssid, "%s", ssid);
  memset(g_pw, 0, sizeof g_pw);
  g_wj_state = BD_JOIN_IDLE;
  g_layer = 0; g_shift = g_caps = false; g_pw_show = true;
  menu_show_page(MP_PASSWORD);
}
// -2 close, -1 the menu list, 0.. a page (MP_*)
void ui_test_menu(int page) {
  if (page == -2) { menu_close(); return; }
  if (page == MP_ADDRESS) { open_address_page(); return; }
  on_menu_open(NULL);
  if (page >= 0) menu_show_page(page);
}
void ui_test_review(int mode, int t10) {
  if (g_br_state != BR_REVIEW || !g_br_n) return;
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
  (void)all;
  if (open) picker_open();
  else picker_close();
}
void ui_test_cursor(int on) {
  g_cursor_on = on != 0;
  lv_obj_set_style_bg_opa(g_cursor, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}
