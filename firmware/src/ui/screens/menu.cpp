// The menu (its layer, header and list) and its Display, Scale and About pages.
#include "../ui_int.h"

// ---- menu -----------------------------------------------------------------------------------------------------------
// One column of rows that open a page; profiles and maintenance are left to the machine's web UI (one line under the
// list). Display: this screen's settings (saved by ui_bridge.cpp through ui_on_settings). Scale / Wi-Fi / About:
// status pages.
lv_obj_t *g_menu;              // the menu layer
static lv_obj_t *g_menu_title, *g_menu_back, *g_menu_main;
lv_obj_t *g_menu_page[MENU_PAGES];
int g_menu_cur = -1;           // the page shown, -1 = the menu list
int g_menu_entry = -1;         // page opened straight from the offline screen: its Back returns there
bool g_in_wifi_pages = false;  // menu open on Wi-Fi list / details / password / address: offline screen away
static lv_obj_t *g_mrow_val[7];                    // value text of each main row
static lv_obj_t *g_info[MENU_PAGES][7];            // status page values: [page][row]
static lv_obj_t *g_disp_val[2];                    // Display page: brightness, idle time
// Display page toggles: [0] idle clock Still / Bouncing, [1] splash at start On / Off
static lv_obj_t *g_disp_tog[2][2], *g_disp_tog_l[2][2];
static void (*g_settings_cb)(int bright, int idle_min, bool bounce, bool splash) = NULL;
bd_machine g_menu_live;
int g_menu_link = BD_LINK_CONNECTING;
static char g_sys_txt[3][64] = {"", "", ""};   // build (version + date), uptime (+ restart reason), memory
static char g_sys_ver[40] = "";                  // the firmware version, "" when not known
static const int IDLE_OPTS[] = {0, 1, 2, 5, 10, 20, 30, 60};
static const char *const PAGE_NAMES[MENU_PAGES] = {"Display", "Scale", "Wi-Fi", "About", "", "", "Machine address",
                                                     "Shot history"};

void ui_on_settings(void (*cb)(int bright, int idle_min, bool bounce, bool splash)) { g_settings_cb = cb; }
void ui_set_settings(int bright, int idle_min, bool bounce, bool splash) {
  g_splash_on = splash;
  if (bright >= 20 && bright <= 100) g_bright = bright;
  if (idle_min >= 0 && idle_min <= 60) g_idle_min = idle_min;
  g_idle_bounce = bounce;
  bl_set(g_bright);
}
void ui_set_sysinfo(const char *build, const char *version, const char *uptime, const char *memory) {
  // the compiler's "Nov  5 2026 00:40:12" as "5 Nov 2026, 00:40", after the version when there is one
  char mon[4], when[40]; int day, year, hh, mm;
  const bool emu = !strncmp(build, "emulator ", 9);
  if (sscanf(build + (emu ? 9 : 0), "%3s %d %d %d:%d", mon, &day, &year, &hh, &mm) == 5)
    snprintf(when, sizeof when, "%s%d %s %d, %02d:%02d", emu ? "emulator " : "", day, mon, year, hh, mm);
  else snprintf(when, sizeof when, "%s", build);
  snprintf(g_sys_ver, sizeof g_sys_ver, "%s", version ? version : "");
  if (g_sys_ver[0]) snprintf(g_sys_txt[0], sizeof g_sys_txt[0], "%s \xC2\xB7 %s", g_sys_ver, when);
  else snprintf(g_sys_txt[0], sizeof g_sys_txt[0], "%s", when);
  snprintf(g_sys_txt[1], sizeof g_sys_txt[1], "%s", uptime);
  snprintf(g_sys_txt[2], sizeof g_sys_txt[2], "%s", memory);
}

static const char *scale_kind_text(const bd_machine *d) {
  switch (d->scale_type) {
    case BD_SCALE_BT: return "Bluetooth";
    case BD_SCALE_WIRED: return "Wired";
    case BD_SCALE_PREDICTIVE: return "Predictive (no scale)";
    default: return "Unknown";
  }
}

static void menu_refresh(void) {
  char b[64];
  const bd_machine *d = &g_menu_live;
  const bool on = g_menu_link == BD_LINK_ONLINE;
  // main rows: 0 Edit profile, 1 Scale, 2 Display, 3 Maintenance, 4 Wi-Fi, 5 About
  set_text(g_mrow_val[1], !on ? "Unknown" : d->scale_type == BD_SCALE_PREDICTIVE ? "Predictive"
                                       : d->scale_present ? "Connected" : "Not connected");
  if (g_idle_min) snprintf(b, sizeof b, "%d %%, dims after %d min", g_bright, g_idle_min);
  else snprintf(b, sizeof b, "%d %%, always on", g_bright);
  set_text(g_mrow_val[2], b);
  set_text(g_mrow_val[4], g_wifi_ssid[0] ? g_wifi_ssid : "Not connected");
  {
    // build date ("15 Nov 2026, ..." / "emulator 15 Nov 2026, ...") -> "Built 15 Nov"
    char mon[4] = "";
    int day = 0;
    const char *bd = g_sys_txt[0];
    if (!strncmp(bd, "emulator ", 9)) bd += 9;
    if (g_upd_known) snprintf(b, sizeof b, "Machine update available");
    else if (g_sys_ver[0]) snprintf(b, sizeof b, "%s", g_sys_ver);
    else if (sscanf(bd, "%d %3s", &day, mon) == 2 || sscanf(bd, "%3s %d", mon, &day) == 2)
      snprintf(b, sizeof b, "Built %d %s", day, mon);
    else snprintf(b, sizeof b, "Screen firmware");
    set_text(g_mrow_val[5], b);
  }
  if (g_hx_ok && g_hx->n > 0) {                   // newest stored shot
    char w[32];
    fmt_when(g_hx->e[0].ts, w, sizeof w);
    snprintf(b, sizeof b, "Latest %s", w);
    set_text(g_mrow_val[6], b);
  } else set_text(g_mrow_val[6], on ? "Loading..." : "Unknown");
  // Display
  snprintf(b, sizeof b, "%d %%", g_bright);
  set_text(g_disp_val[0], b);
  if (g_idle_min) snprintf(b, sizeof b, "%d min", g_idle_min); else snprintf(b, sizeof b, "Off");
  set_text(g_disp_val[1], b);
  // toggles (like the review's): idle clock Still / Bouncing, splash On / Off
  for (int t = 0; t < 2; t++)
    for (int i = 0; i < 2; i++) {
      const bool on = t == 0 ? (i == 1) == g_idle_bounce : (i == 0) == g_splash_on;
      lv_obj_set_style_bg_opa(g_disp_tog[t][i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
      set_tc(g_disp_tog_l[t][i], on ? COL_TEXT : COL_MUTED);
    }
  // Scale
  set_text(g_info[MP_SCALE][0], on ? scale_kind_text(d) : "Unknown");
  set_text(g_info[MP_SCALE][1], !on ? "Unknown" : d->scale_present ? "Connected" : "Not connected");
  if (on && d->scale_present && d->scale_type != BD_SCALE_PREDICTIVE) snprintf(b, sizeof b, "%.1f g", d->weight);
  else snprintf(b, sizeof b, "-");
  set_text(g_info[MP_SCALE][2], b);
  set_text(g_info[MP_SCALE][3], "Automatic at the start of a shot");
  // Wi-Fi
  set_text(g_info[MP_WIFI_DETAILS][0], g_wifi_ssid[0] ? g_wifi_ssid : "Not connected");
  if (g_wifi_ssid[0]) snprintf(b, sizeof b, "%d dBm  \xC2\xB7  channel %d", g_wifi_rssi, g_wifi_ch);
  else snprintf(b, sizeof b, "-");
  set_text(g_info[MP_WIFI_DETAILS][1], b);
  set_text(g_info[MP_WIFI_DETAILS][2], g_wifi_ap[0] ? g_wifi_ap : "-");
  set_text(g_info[MP_WIFI_DETAILS][3], g_off_sip[0] ? g_off_sip : "-");
  snprintf(b, sizeof b, "%s  \xC2\xB7  %s", g_off_mach, on ? "online" : "not reachable");
  set_text(g_info[MP_WIFI_DETAILS][4], b);
  forget_refresh();
  // About
  for (int i = 0; i < 3; i++) set_text(g_info[MP_ABOUT][i], g_sys_txt[i][0] ? g_sys_txt[i] : "-");
  if (g_upd_rel[0]) snprintf(b, sizeof b, "Release %s \xC2\xB7 install in the web UI", g_upd_rel);
  else snprintf(b, sizeof b, "%s", g_upd_known ? "Available \xC2\xB7 install in the web UI" : "None reported");
  set_tc(g_info[MP_ABOUT][6], g_upd_known ? COL_BLUE : COL_TEXT);
  set_text(g_info[MP_ABOUT][6], b);
  if (d->core_version[0] && d->core_board[0]) snprintf(b, sizeof b, "%s \xC2\xB7 %s", d->core_version, d->core_board);
  else snprintf(b, sizeof b, "%s", d->core_version[0] ? d->core_version : "Unknown");
  set_text(g_info[MP_ABOUT][3], b);
  set_text(g_info[MP_ABOUT][4], d->screen_version[0] ? d->screen_version : "Unknown");
  set_text(g_info[MP_ABOUT][5], d->web_version[0] ? d->web_version : "Unknown");
}

void menu_show_page(int k) {            // MP_LIST = the menu list
  lv_obj_set_hidden(g_menu_main, true);
  for (int i = 0; i < MENU_PAGES; i++) lv_obj_set_hidden(g_menu_page[i], true);
  g_menu_cur = k;
  g_in_wifi_pages = k == MP_WIFI || k == MP_WIFI_DETAILS || k == MP_PASSWORD || k == MP_ADDRESS;
  if (g_in_wifi_pages && !lv_obj_is_hidden(g_off)) lv_obj_set_hidden(g_off, true);
  wifi_page_shown(k);
  if (k == MP_HISTORY) hist_page_shown();
  if (k == MP_LIST) {
    lv_obj_set_hidden(g_menu_main, false);
    lv_obj_set_hidden(g_menu_back, true);
    set_text(g_menu_title, "Menu");
    lv_obj_set_x(g_menu_title, 24);
  } else {
    lv_obj_set_hidden(g_menu_page[k], false);
    lv_obj_set_hidden(g_menu_back, false);
    set_text(g_menu_title, k == MP_WIFI_DETAILS ? (g_wifi_ssid[0] ? g_wifi_ssid : "Wi-Fi")
                           : k == MP_PASSWORD   ? g_wj_ssid
                                                : PAGE_NAMES[k]);
    lv_obj_set_x(g_menu_title, 92);
  }
  menu_refresh();
}
void on_menu_open(lv_event_t *e) {
  (void)e;
  picker_close();
  menu_show_page(MP_LIST);
  lv_obj_set_hidden(g_menu, false);
  lv_obj_move_to_index(g_menu, -1);
  if (!lv_obj_is_hidden(g_off)) lv_obj_move_to_index(g_off, -1);
}
void open_wifi_menu(void) {
  on_menu_open(NULL);
  menu_show_page(MP_WIFI);
}
void menu_close(void) {
  lv_obj_set_hidden(g_menu, true);
  g_in_wifi_pages = false;
  g_menu_cur = g_menu_entry = -1;
  if (g_off) on_off_timer(NULL);        // offline screen back in the same frame (no Ready flash in between)
}
static void on_menu_close(lv_event_t *e) { (void)e; menu_close(); }
static void on_menu_back(lv_event_t *e) {
  (void)e;
  if (g_menu_cur >= 0 && g_menu_cur == g_menu_entry) { menu_close(); return; }   // back to the offline screen
  menu_show_page(g_menu_cur == MP_WIFI_DETAILS || g_menu_cur == MP_PASSWORD ? MP_WIFI
                 : g_menu_cur == MP_ADDRESS ? MP_WIFI_DETAILS : MP_LIST);
}
static void on_menu_row(lv_event_t *e) { menu_show_page((int)(intptr_t)lv_event_get_user_data(e)); }
static void menu_timer(lv_timer_t *t) {
  (void)t;
  hist_timer();
  if (!lv_obj_is_hidden(g_menu)) menu_refresh();
}

static void settings_changed(void) {
  bl_set(g_bright);
  if (g_settings_cb) g_settings_cb(g_bright, g_idle_min, g_idle_bounce, g_splash_on);
  menu_refresh();
}
static void on_disp_step(lv_event_t *e) {
  const int k = (int)(intptr_t)lv_event_get_user_data(e);   // 0/1 brightness -/+, 2/3 idle -/+
  if (k < 2) {
    const int v = g_bright + (k ? 10 : -10);
    if (v < 20 || v > 100) return;          // 20 % floor (never dark)
    g_bright = v;
  } else {
    int i = 0;
    const int n = (int)(sizeof IDLE_OPTS / sizeof IDLE_OPTS[0]);
    while (i < n - 1 && IDLE_OPTS[i] < g_idle_min) i++;
    i += k == 3 ? 1 : -1;
    if (i < 0 || i >= n) return;
    g_idle_min = IDLE_OPTS[i];
  }
  settings_changed();
}
static void on_disp_tog(lv_event_t *e) {          // user data: toggle * 2 + option
  const int v = (int)(intptr_t)lv_event_get_user_data(e);
  bool *f = v / 2 == 0 ? &g_idle_bounce : &g_splash_on;
  const bool b = v / 2 == 0 ? (v % 2) == 1 : (v % 2) == 0;   // idle: 1 = Bouncing; splash: 0 = On
  if (b == *f) return;
  *f = b;
  settings_changed();
}
// a Display row with a two-option toggle (idle clock, splash at start)
static void disp_toggle_row(lv_obj_t *c, int t, const char *title, const char *a, const char *b) {
  lv_obj_t *r = box(c, 0, 0, 752, 80, COL_BG, 0);
  hairline(r, LV_BORDER_SIDE_BOTTOM);
  lv_obj_align(label(r, title, F_ROW, COL_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *tg = flex(r, LV_FLEX_FLOW_ROW, 2);
  lv_obj_set_style_pad_all(tg, 3, 0);
  lv_obj_set_style_radius(tg, RAD_CTRL, 0);
  lv_obj_set_style_bg_opa(tg, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(tg, COL_WELL, 0);
  const char *const T[2] = {a, b};
  for (int i = 0; i < 2; i++) {
    lv_obj_t *o = g_disp_tog[t][i] = flex(tg, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(o, 116, 46);
    lv_obj_set_style_radius(o, RAD_INNER, 0);
    lv_obj_set_style_bg_color(o, COL_PK_BTN, 0);
    lv_obj_set_clickable(o, true);
    lv_obj_add_event_cb(o, on_disp_tog, LV_EVENT_CLICKED, (void *)(intptr_t)(t * 2 + i));
    g_disp_tog_l[t][i] = label(o, T[i], F_TEXT_B, COL_MUTED);
  }
  lv_obj_align(tg, LV_ALIGN_RIGHT_MID, 0, 0);
}

// a row: label left, value (+ chevron when it opens a page) right; bottom hairline
static lv_obj_t *menu_row(lv_obj_t *col, const char *name, lv_obj_t **val, bool opens, bool grey) {
  lv_obj_t *r = box(col, 0, 0, 356, 72, COL_BG, 0);
  hairline(r, LV_BORDER_SIDE_BOTTOM);
  lv_obj_t *l = label(r, name, F_ROW, grey ? COL_OUTLINE : COL_TEXT);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *right = flex(r, LV_FLEX_FLOW_ROW, 12);
  lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
  *val = label(right, "", F_TEXT, grey ? COL_OUTLINE : COL_MUTED);
  if (opens) {
    icon_chevron_right(right, COL_MUTED);
    make_button(r, COL_CARD);
  }
  return r;
}
static lv_obj_t *menu_column(lv_obj_t *p, int x) {
  lv_obj_t *c = flex(p, LV_FLEX_FLOW_COLUMN, 0);
  lv_obj_set_pos(c, x, 0);
  hairline(c, LV_BORDER_SIDE_TOP);
  return c;
}
static lv_obj_t *small_btn(lv_obj_t *p, bool plus, int k) {
  lv_obj_t *b = box(p, 0, 0, 52, 52, COL_CARD, RAD_CTRL);
  make_button(b, COL_CARD_PR);
  stepper_part(b);                                // part of a stepper card (like Ready's)
  lv_obj_t *w = flex(b, LV_FLEX_FLOW_ROW, 0);
  lv_obj_center(w);
  static const lv_point_t h[] = {{3, 10}, {17, 10}};
  static const lv_point_t v[] = {{10, 3}, {10, 17}};
  lv_obj_t *ic = icon_box(w, 20, 20);
  line(ic, h, 2, 2, COL_TEXT);
  if (plus) line(ic, v, 2, 2, COL_TEXT);
  lv_obj_add_event_cb(b, on_disp_step, LV_EVENT_CLICKED, (void *)(intptr_t)k);
  return b;
}

// ---- building the menu and its pages --------------------------------------------------------------------------------
void build_menu(lv_obj_t *scr) {
  g_menu = box(scr, 0, 0, 800, 480, COL_BG, 0);
  lv_obj_set_clickable(g_menu, true);
  g_menu_title = label(g_menu, "Menu", F_TITLE, COL_TEXT);
  lv_obj_set_pos(g_menu_title, 24, 32);
  g_menu_back = box(g_menu, 24, 24, 52, 52, COL_CARD, RAD_CTRL);
  tag(g_menu_back, "action.back");
  make_button(g_menu_back, COL_CARD_PR);
  static const lv_point_t lt[] = {{12, 4}, {7, 10}, {12, 16}};
  lv_obj_t *bw = icon_box(g_menu_back, 20, 20);
  lv_obj_center(bw);
  line(bw, lt, 3, 2, COL_TEXT);
  lv_obj_add_event_cb(g_menu_back, on_menu_back, LV_EVENT_CLICKED, NULL);
  lv_obj_t *cl = box(g_menu, 724, 24, 52, 52, COL_CARD, RAD_CTRL);
  tag(cl, "action.close");
  make_button(cl, COL_CARD_PR);
  lv_obj_t *cw = flex(cl, LV_FLEX_FLOW_ROW, 0);
  lv_obj_center(cw);
  icon_close(cw, COL_TEXT);
  lv_obj_add_event_cb(cl, on_menu_close, LV_EVENT_CLICKED, NULL);

  // main list: one full-width column, a blue line icon per row; page index per row, -1 = no page
  g_menu_main = box(g_menu, 24, 96, 752, 360, COL_BG, 0);   // lists start at 96
  tag(g_menu_main, "screen.menu");
  // R[] also has the web-UI-only rows (0, 3), not shown; its indexes are g_mrow_val's
  static const struct { const char *name; int page; bool grey; } R[7] = {
    {"Edit profile", -1, true}, {"Scale", MP_SCALE, false}, {"Display", MP_DISPLAY, false},
    {"Maintenance", -1, true}, {"Wi-Fi", MP_WIFI, false}, {"About", MP_ABOUT, false},
    {"Shot history", MP_HISTORY, false}};
  // only the rows that open something; the web-UI-only items are one line at the bottom
  static const int ORDER[5] = {6, 1, 2, 4, 5};      // Shot history, Scale, Display, Wi-Fi, About
  static const int ICON[5] = {IC_HISTORY, IC_SCALE, IC_DISPLAY, IC_WIFI, IC_ABOUT};
  lv_obj_t *col = menu_column(g_menu_main, 0);
  for (int j = 0; j < 5; j++) {
    const int i = ORDER[j];
    lv_obj_t *r = menu_row(col, R[i].name, &g_mrow_val[i], R[i].page >= 0, R[i].grey);
    lv_obj_set_size(r, 752, 64);
    lv_obj_t *ic = line_icon(r, ICON[j], 24, COL_BLUE);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(lv_obj_get_child(r, 0), LV_ALIGN_LEFT_MID, 40, 0);   // the name, after the icon
    if (R[i].page >= 0) lv_obj_add_event_cb(r, on_menu_row, LV_EVENT_CLICKED, (void *)(intptr_t)R[i].page);
    if (R[i].page == MP_WIFI) tag(r, "action.wifi_settings");
  }
  lv_obj_align(label(g_menu_main, "Profiles and maintenance are in the machine's web UI.", F_TEXT, COL_MUTED),
               LV_ALIGN_BOTTOM_LEFT, 0, -4);

  // pages
  // About: the screen's own 3 rows, then the machine's versions and its update notice
  static const char *const INFO[5][7] = {
    {NULL}, {"Type", "Status", "Weight", "Tare"}, {NULL},
    {"Build", "Running for", "Free memory", "Machine firmware", "Machine screen", "Machine web UI", "Machine update"},
    {"Network", "Signal", "Access point", "Screen", "Machine"}};
  build_wifi(g_menu);                              // the Wi-Fi list and password / address pages
  build_hist(g_menu);                              // shot history
  for (int k = 0; k < 5; k++) {
    if (k == MP_WIFI) continue;
    g_menu_page[k] = box(g_menu, 24, 96, 752, 360, COL_BG, 0);
    static const char *const PAGE_TAG[5] = {"screen.display", "screen.scale", NULL, "screen.about", "screen.wifi_details"};
    tag(g_menu_page[k], PAGE_TAG[k]);
    lv_obj_t *c = flex(g_menu_page[k], LV_FLEX_FLOW_COLUMN, 0);
    hairline(c, LV_BORDER_SIDE_TOP);
    if (k == MP_DISPLAY) {                        // Display: two steppers
      static const char *const D[2] = {"Brightness", "Dim after"};
      for (int i = 0; i < 2; i++) {
        lv_obj_t *r = box(c, 0, 0, 752, 80, COL_BG, 0);
        hairline(r, LV_BORDER_SIDE_BOTTOM);
        lv_obj_align(label(r, D[i], F_ROW, COL_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
        // one stepper card, as wide as the toggles below (2 x 116 + 2 + 2 x 3)
        lv_obj_t *st = flex(r, LV_FLEX_FLOW_ROW, 0);
        lv_obj_set_flex_align(st, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_size(st, 240, 52);
        lv_obj_set_style_bg_color(st, COL_CARD, 0);
        lv_obj_set_style_bg_opa(st, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(st, RAD_CTRL, 0);
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, 0, 0);
        small_btn(st, false, i * 2);
        g_disp_val[i] = label(st, "", F_ROW, COL_TEXT);
        lv_obj_set_width(g_disp_val[i], 240 - 2 * 52);
        lv_obj_set_style_text_align(g_disp_val[i], LV_TEXT_ALIGN_CENTER, 0);
        small_btn(st, true, i * 2 + 1);
      }
      disp_toggle_row(c, 0, "Idle clock", "Still", "Bouncing");
      disp_toggle_row(c, 1, "Splash at start", "On", "Off");     // the boot splash (ui_splash)
      lv_obj_t *n = label(c, "Dim after: minutes without a touch before the dim clock shows. "
                             "Saved on this screen.", F_TEXT, COL_MUTED);
      lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
      lv_obj_set_width(n, 752);
      lv_obj_set_style_pad_top(n, 16, 0);
      continue;
    }
    if (k == MP_ABOUT) {                          // About, first row: what this is (not an official product)
      lv_obj_t *r = box(c, 0, 0, 752, 44, COL_BG, 0);
      hairline(r, LV_BORDER_SIDE_BOTTOM);
      lv_obj_align(logo(r, 32), LV_ALIGN_LEFT_MID, -3, 0);   // 32 px: at 28 the 2 px wisps nearly touch
      lv_obj_align(label(r, "Brewdeck", F_TEXT, COL_TEXT), LV_ALIGN_LEFT_MID, 36, 0);
      lv_obj_t *un = label(r, "Unofficial Wi-Fi screen for Gaggiuino machines", F_TEXT, COL_MUTED);
      lv_obj_align(un, LV_ALIGN_RIGHT_MID, 0, 0);
      tag(un, "info.unofficial");
    }
    for (int i = 0; i < 7 && INFO[k][i]; i++) {
      // Wi-Fi details: room for Forget below; About: the Brewdeck row + 7 rows of 44 px in the 360 px page
      lv_obj_t *r = box(c, 0, 0, 752, k == MP_WIFI_DETAILS ? 52 : k == MP_ABOUT ? 44 : 60, COL_BG, 0);
      hairline(r, LV_BORDER_SIDE_BOTTOM);
      lv_obj_align(label(r, INFO[k][i], F_TEXT, COL_MUTED), LV_ALIGN_LEFT_MID, 0, 0);
      g_info[k][i] = label(r, "", F_TEXT, COL_TEXT);
      if (k == MP_ABOUT && i == 6) tag(g_info[k][i], "info.update");   // the machine's update notice, kept
      lv_obj_align(g_info[k][i], LV_ALIGN_RIGHT_MID, 0, 0);
      if (k == MP_WIFI_DETAILS && i == 4) {         // Machine row: change the machine's address
        lv_obj_align(g_info[k][i], LV_ALIGN_RIGHT_MID, -28, 0);
        lv_obj_t *ch = icon_box(r, 16, 16);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, 0, 0);
        icon_chevron_right(ch, COL_MUTED);
        make_button(r, COL_CARD);
        lv_obj_add_event_cb(r, on_addr_row, LV_EVENT_CLICKED, NULL);
        tag(r, "action.machine_address");
      }
    }
    if (k == MP_WIFI_DETAILS) {                     // Forget this network (like a phone), 2-tap confirm
      g_fg_btn = box(g_menu_page[MP_WIFI_DETAILS], 0, 360 - 56, 260, 56, COL_CARD, RAD_CTRL);
      make_button(g_fg_btn, COL_CARD_PR);
      lv_obj_add_event_cb(g_fg_btn, on_forget, LV_EVENT_CLICKED, NULL);
      tag(g_fg_btn, "action.forget");
      g_fg_lbl = label(g_fg_btn, "Forget this network", F_TEXT_B, COL_TEXT);
      lv_obj_center(g_fg_lbl);
    }
  }
  lv_obj_set_hidden(g_menu, true);
  lv_timer_create(menu_timer, 1000, NULL);
}
