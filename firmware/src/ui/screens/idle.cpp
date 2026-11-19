// Idle clock, screen off (against image retention) and the splash at start.
#include "../ui_int.h"

// Screen off, against image retention: after SCREEN_OFF_MIN without a touch (or the "Dim after" time if longer; not
// when dimming is Off) and nothing running, or after the offline screen has shown untouched for SCREEN_OFF_OFFLINE_MS
// (machine switched off at night): a plain black picture AND the backlight off. Black matters too: LCD image retention
// comes from the cells holding one picture, lit or not. Wakes on a touch (only wakes, to the screen below, not the
// clock), a shot / steam / flush, or the machine coming back when it was off because of the offline screen.
static const int SCREEN_OFF_MIN = 30;
static const uint32_t SCREEN_OFF_OFFLINE_MS = 120000;

// with the Display setting "Idle clock: Bouncing" the clock block bounces around the screen like the DVD logo; it also
// does the job of the anti-retention shift used when it stands still. 1 px per 40 ms on each axis (25 px/s, 45
// degrees); each wall it hits gives the clock the next muted tint (all as dim as the grey: it is a night screen). Only
// the block's strip is redrawn per step (~350x190 px).
static const uint32_t IDLE_BOUNCE_MS = 40;
static const int IDLE_EDGE = 24;                  // the screens' margin
static const uint32_t SHIFT_MS = 180000;          // still clock: moves to its next place this often
static const uint32_t BREW_WAKE_MS = 2000;        // a brew keeps the clock up at most this long for its screen

// ---- idle screen ----------------------------------------------------------------------------------------------------
// After g_idle_min minutes without a touch, while nothing runs: a quiet clock + temperature on black, backlight dimmed.
// The first touch only wakes it (taps do not reach the buttons below); a shot or steam wakes it too.
int g_idle_min = 10, g_bright = 100, g_idle_bright = 50;   // the menu sets the first two; idle: dim, not dark
lv_obj_t *g_idle;
static lv_obj_t *g_idle_clock, *g_idle_dot, *g_idle_txt, *g_idle_col;
void bl_set(int pct) { bd_set_backlight(g_blank_on ? 0 : pct); }   // off while the screen is off
int g_clock_h = -1, g_clock_m = -1;
bool g_idle_busy = false;      // shot / steam / hot water / flush running (set by ui_set_live)
char g_idle_temp[16] = "";
int g_idle_state = 0;          // 0 ready, 1 heating, 2 cooling (after steam), 3 pressure left
bool g_press_warn = false;     // idle with residual pressure
float g_press_val = 0;
int g_idle_word = 0;           // with state 3: the word under it (0 ready, 1 heating, 2 cooling)
void ui_set_clock(int h, int m) { g_clock_h = h; g_clock_m = m; }

bool g_idle_force = false;     // test hook: go idle now

bool g_idle_bounce = false;    // Display setting, saved in NVS "screen"/"bounce"
static int g_ib_x, g_ib_y, g_ib_dx = 1, g_ib_dy = 1, g_ib_tint = 0;
static int g_idle_shift = -1;         // still clock: current anti-retention place (-1 = place it again)
static const uint32_t IDLE_TINTS[] = {0x6A6A72, 0x5E7486, 0x86765A, 0x5E8270, 0x86626A, 0x726690};
static void idle_bounce_start(void) {           // from the centre
  lv_obj_update_layout(g_idle_col);
  g_ib_x = (800 - lv_obj_get_width(g_idle_col)) / 2;
  g_ib_y = (480 - lv_obj_get_height(g_idle_col)) / 2;
  g_ib_tint = 0;
  lv_obj_set_style_text_color(g_idle_clock, lv_color_hex(IDLE_TINTS[0]), 0);
  lv_obj_align(g_idle_col, LV_ALIGN_TOP_LEFT, g_ib_x, g_ib_y);
}
static void on_idle_bounce(lv_timer_t *t) {
  (void)t;
  if (!g_idle_bounce || lv_obj_is_hidden(g_idle) || g_blank_on) return;
  const int w = lv_obj_get_width(g_idle_col), h = lv_obj_get_height(g_idle_col);
  const int xmax = 800 - IDLE_EDGE - w, ymax = 480 - IDLE_EDGE - h;
  bool hit = false;
  g_ib_x += g_ib_dx;
  g_ib_y += g_ib_dy;
  if (g_ib_x <= IDLE_EDGE) { g_ib_x = IDLE_EDGE; if (g_ib_dx < 0) { g_ib_dx = 1; hit = true; } }
  if (g_ib_x >= xmax) { g_ib_x = xmax > IDLE_EDGE ? xmax : IDLE_EDGE; if (g_ib_dx > 0) { g_ib_dx = -1; hit = true; } }
  if (g_ib_y <= IDLE_EDGE) { g_ib_y = IDLE_EDGE; if (g_ib_dy < 0) { g_ib_dy = 1; hit = true; } }
  if (g_ib_y >= ymax) { g_ib_y = ymax > IDLE_EDGE ? ymax : IDLE_EDGE; if (g_ib_dy > 0) { g_ib_dy = -1; hit = true; } }
  if (hit) {
    g_ib_tint = (g_ib_tint + 1) % (int)(sizeof IDLE_TINTS / sizeof IDLE_TINTS[0]);
    lv_obj_set_style_text_color(g_idle_clock, lv_color_hex(IDLE_TINTS[g_ib_tint]), 0);
  }
  lv_obj_set_pos(g_idle_col, g_ib_x, g_ib_y);
}
void idle_hide(void) {
  g_idle_force = false;
  if (lv_obj_is_hidden(g_idle)) return;
  lv_obj_set_hidden(g_idle, true);
  bl_set(g_bright);
}
static void on_idle_touch(lv_event_t *e) { (void)e; idle_hide(); }
void on_idle_timer(lv_timer_t *t) {
  (void)t;
  const bool hidden = lv_obj_is_hidden(g_idle);
  const bool want = g_idle_min > 0 && !g_idle_busy && g_br_state == BR_HIDDEN &&
                    (g_idle_force || lv_display_get_inactive_time(NULL) >= (uint32_t)g_idle_min * 60000u);
  // a brew wakes the clock: keep it until the brewing screen opens on the first point (br_start hides it in the same
  // frame), at most BREW_WAKE_MS: hiding it first, then opening the brewing screen, would cost two full redraws
  static uint32_t busy_since = 0;
  if (!g_idle_busy) busy_since = 0;
  else if (!busy_since) busy_since = stamp_nz(lv_tick_get());
  const bool wait_brew =
      g_live_brew && g_br_state == BR_HIDDEN && busy_since && lv_tick_elaps(busy_since) < BREW_WAKE_MS;
  if (!want) { if (!hidden && (g_idle_busy || g_br_state != BR_HIDDEN) && !wait_brew) idle_hide(); return; }
  char b[48];
  if (g_clock_h >= 0) snprintf(b, sizeof b, "%02d:%02d", g_clock_h, g_clock_m);
  else snprintf(b, sizeof b, "%s", g_idle_temp[0] ? g_idle_temp : "0.0");   // no time yet: the temperature
  set_text(g_idle_clock, b);
  static const char *const ST[3] = {"ready", "heating", "cooling"};
  const int st = g_idle_state >= 0 && g_idle_state <= 3 ? g_idle_state : 0;
  char sts[32];
  // pressure left in the system: the bar after the word
  if (st == 3) snprintf(sts, sizeof sts, "%s \xC2\xB7 %.1f bar", ST[g_idle_word % 3], g_press_val);
  else snprintf(sts, sizeof sts, "%s", ST[st]);
  if (g_clock_h >= 0) snprintf(b, sizeof b, "%s\xC2\xB0 \xC2\xB7 %s", g_idle_temp, sts);
  else snprintf(b, sizeof b, "%s", sts);
  set_text(g_idle_txt, b);
  set_bg(g_idle_dot, st == 1 ? COL_GOLD : st == 2 ? COL_BLUE : st == 3 ? COL_WARN : COL_GREEN);
  // still clock, against image retention: the block moves a few px every SHIFT_MS (8 places, <= 12 px from the centre)
  if (!g_idle_bounce) {
    static const int8_t DX[8] = {0, 10, -8, 12, -12, 6, -4, 8}, DY[8] = {0, -6, 8, 4, -8, 10, -10, -2};
    const int k = (int)(lv_tick_get() / SHIFT_MS) % 8;
    if (hidden || k != g_idle_shift) {
      g_idle_shift = k;
      lv_obj_set_style_text_color(g_idle_clock, lv_color_hex(IDLE_TINTS[0]), 0);
      lv_obj_align(g_idle_col, LV_ALIGN_CENTER, DX[k], DY[k]);
    }
  }
  if (hidden) {
    if (g_idle_bounce) idle_bounce_start();
    picker_close();
    editor_close();
    menu_close();
    lv_obj_set_hidden(g_idle, false);
    lv_obj_move_to_index(g_idle, -1);
    if (!lv_obj_is_hidden(g_off)) lv_obj_move_to_index(g_off, -1);   // offline stays on top
    bl_set(g_idle_bright < g_bright ? g_idle_bright : g_bright);   // never brighter than set
  }
}
// ---- screen off (against image retention) ---------------------------------------------------------------------------
lv_obj_t *g_blank;
bool g_blank_on = false, g_blank_offline = false;
static bool g_blank_force = false;
static uint32_t g_off_shown_ms = 0;              // when the offline screen appeared (0 = not shown)
static void screen_wake(void) {
  if (!g_blank_on) return;
  g_blank_on = g_blank_offline = false;
  lv_obj_set_hidden(g_blank, true);
  lv_display_trigger_activity(NULL);
  idle_hide();                                    // straight to the screen below, not to the dimmed clock
  bl_set(g_bright);
  bd_log("ui: screen on");
}
static void on_blank_touch(lv_event_t *e) { (void)e; screen_wake(); }
static void on_blank_timer(lv_timer_t *t) {
  (void)t;
  const bool off_shown = !lv_obj_is_hidden(g_off);
  if (!off_shown) g_off_shown_ms = 0;
  else if (!g_off_shown_ms) g_off_shown_ms = stamp_nz(lv_tick_get());
  const uint32_t inact = lv_display_get_inactive_time(NULL);
  if (g_blank_on) {
    if (g_idle_busy || g_br_state != BR_HIDDEN || (g_blank_offline && !off_shown)) screen_wake();
    else lv_obj_move_to_index(g_blank, -1);          // stays on top of layers shown later (no-op when it is already)
    return;
  }
  if (g_idle_busy || g_br_state != BR_HIDDEN || g_in_wifi_pages) { g_blank_force = false; return; }
  const uint32_t idle_ms = (uint32_t)(g_idle_min > SCREEN_OFF_MIN ? g_idle_min : SCREEN_OFF_MIN) * 60000u;
  const bool by_idle = g_idle_min > 0 && inact >= idle_ms;
  const bool by_offline =
      off_shown && lv_tick_elaps(g_off_shown_ms) >= SCREEN_OFF_OFFLINE_MS && inact >= SCREEN_OFF_OFFLINE_MS;
  if (!g_blank_force && !by_idle && !by_offline) return;
  g_blank_force = false;
  g_blank_on = true;
  g_blank_offline = by_offline && !by_idle;
  picker_close();
  editor_close();
  lv_obj_set_hidden(g_blank, false);
  lv_obj_move_to_index(g_blank, -1);
  bl_set(0);
  bd_log("ui: screen off (%s)", g_blank_offline ? "offline screen untouched for 2 min" : "no touch");
}
void build_blank(lv_obj_t *scr) {
  g_blank = box(scr, 0, 0, 800, 480, lv_color_black(), 0);
  lv_obj_set_clickable(g_blank, true);
  lv_obj_add_event_cb(g_blank, on_blank_touch, LV_EVENT_PRESSED, NULL);
  lv_obj_set_hidden(g_blank, true);
  lv_timer_create(on_blank_timer, 500, NULL);
}
void ui_test_screen_off(void) { g_blank_force = true; }

// ---- splash ---------------------------------------------------------------------------------------------------------
// After boot, 5 s (a tap anywhere closes it at once): the screen's own name and, as clearly, that it is not an
// official Gaggiuino product. Top layer, over everything; deleted when it goes. Started by ui_bridge.cpp (bd_ui_init)
// when the setting is on, not by ui_init, so the sim renders and scripted emulator runs do not show it.
static lv_obj_t *g_splash;
bool g_splash_on = true;                  // Display setting "Splash at start" (read at boot by ui_bridge.cpp)
static void splash_end(void) { if (g_splash) { lv_obj_delete(g_splash); g_splash = NULL; } }
static void on_splash_timer(lv_timer_t *t) { (void)t; splash_end(); }   // runs once (repeat count 1)
static void on_splash_tap(lv_event_t *e) { (void)e; splash_end(); }
void ui_test_splash_end(void) { splash_end(); }   // emulator: start without the splash (scripted runs)
void ui_splash(void) {
  if (g_splash) return;
  g_splash = box(lv_layer_top(), 0, 0, 800, 480, COL_BG, 0);
  lv_obj_set_clickable(g_splash, true);
  lv_obj_add_event_cb(g_splash, on_splash_tap, LV_EVENT_PRESSED, NULL);
  // the offline screen's rhythm: logo 104..200, name, the "unofficial" line close under it, "Built" on the
  // offline screen's details line (430)
  lv_obj_t *lg = logo(g_splash, 96);
  lv_obj_set_pos(lg, 352, 104);
  lv_obj_t *name = label(g_splash, "Brewdeck", F_SPLASH, COL_TEXT);
  lv_obj_set_style_text_letter_space(name, -1, 0);
  lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 230);
  lv_obj_t *sub = label(g_splash, "Unofficial Wi-Fi screen for Gaggiuino machines", F_TEXT, COL_MUTED);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 304);
  char b[40], mon[4] = "";
  int day = 0, year = 0;
  if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) == 3) snprintf(b, sizeof b, "Built %d %s %d", day, mon, year);
  else snprintf(b, sizeof b, " ");
  lv_obj_t *bl = label(g_splash, b, F_TEXT, COL_FAINT);
  lv_obj_align(bl, LV_ALIGN_TOP_MID, 0, 430);
  lv_timer_t *t = lv_timer_create(on_splash_timer, 5000, NULL);
  lv_timer_set_repeat_count(t, 1);
}

void build_idle(lv_obj_t *scr) {
  g_idle = box(scr, 0, 0, 800, 480, lv_color_black(), 0);
  lv_obj_set_clickable(g_idle, true);
  lv_obj_add_event_cb(g_idle, on_idle_touch, LV_EVENT_PRESSED, NULL);
  lv_obj_t *col = g_idle_col = flex(g_idle, LV_FLEX_FLOW_COLUMN, 14);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(col, LV_ALIGN_CENTER, 0, 0);      // placed when shown (still, or bouncing)
  g_idle_clock = label(col, "07:52", F_HERO, lv_color_hex(IDLE_TINTS[0]));
  lv_obj_t *row = flex(col, LV_FLEX_FLOW_ROW, 10);
  g_idle_dot = box(row, 0, 0, 8, 8, COL_GREEN, 4);
  lv_obj_set_style_bg_opa(g_idle_dot, LV_OPA_80, 0);
  g_idle_txt = label(row, "90.1\xC2\xB0 \xC2\xB7 ready", F_TITLE, COL_IDLE_TX);
  lv_obj_set_hidden(g_idle, true);
  lv_timer_create(on_idle_timer, 500, NULL);
  lv_timer_create(on_idle_bounce, IDLE_BOUNCE_MS, NULL);
}
