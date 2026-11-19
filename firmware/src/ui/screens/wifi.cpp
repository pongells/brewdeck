// Wi-Fi: network list, details, password and machine address keyboard.
#include "../ui_int.h"

// The list (MP_WIFI): networks from a scan (2.4 GHz only), the connected one on top in green; tap -> the password
// page or, for an open network, join at once; tap the connected one -> details. The password page has its own keyboard
// (no LVGL keyboard widget: the keys match the design system). The password lives only here and in the join request;
// never logged. The machine address page uses the same keyboard.
#define NET_ROWS 8                                 // rows on the list page (the rest of the scan is cut)
#define KEYS 32
static const uint32_t DOUBLE_TAP_MS = 400;          // shift tapped twice within this: caps lock
static const uint32_t FORGET_ARM_MS = 3000;         // "Tap again to forget" waits this long
enum { K_SHIFT = 19, K_BS = 27, K_LAYER = 28, K_SPACE = 29, K_DOT = 30, K_JOIN = 31 };

// ---- Wi-Fi join -----------------------------------------------------------------------------------------------------
char g_wifi_ssid[33] = "", g_wifi_ap[20] = "";   // network the screen is on, "" = none
int g_wifi_rssi = 0, g_wifi_ch = 0;
char g_wifi_target[33] = "";                     // network the screen tries to join ("" = none set up)
char g_mach_host[64] = "gaggiuino.local";        // configured machine address
char g_wj_ssid[33] = "";                         // network being joined (password page title)
static bd_wifi_net g_nets[BD_MAX_NETS];
static int g_nets_n = 0;
static bool g_nets_scanning = false;
int g_wj_state = BD_JOIN_IDLE;
char g_pw[65] = "";
bool g_pw_show = true, g_shift = false, g_caps = false;
static bool g_kb_addr = false;                      // keyboard page used for the machine address
int g_layer = 0;                             // 0 letters, 1 digits/symbols, 2 more symbols
static uint32_t g_shift_ms = 0;
static lv_obj_t *g_wl_note, *g_wl_scan;
static EmptyState g_wl_empty;
static lv_obj_t *g_wl_row[NET_ROWS], *g_wl_name[NET_ROWS], *g_wl_sub[NET_ROWS], *g_wl_bar[NET_ROWS][4];
static lv_obj_t *g_wl_lock[NET_ROWS], *g_wl_chev[NET_ROWS], *g_wl_conn[NET_ROWS];
static int g_wl_idx[NET_ROWS];                      // row -> index in g_nets, -1 = the connected network (not in scan)
static lv_obj_t *g_pw_txt, *g_pw_cur, *g_pw_hint, *g_pw_join, *g_pw_join_l, *g_pw_eye, *g_pw_field_l;
static lv_obj_t *g_key[KEYS], *g_key_l[KEYS];
// layout: row 0 10 keys, row 1 9, row 2 shift + 7 + backspace, row 3 layer + space + '.' + Join
static const char *const KL[3][3] = {
  {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
  {"1234567890", "@#$%&*-+=", "!?/_\"':"},
  {"1234567890", "()[]{}<>~", ",;\\|^`."}};

void ui_set_wifi(const char *ssid, int rssi, int channel, const char *ap) {
  const bool changed = strcmp(g_wifi_ssid, ssid) != 0;
  snprintf(g_wifi_ssid, sizeof g_wifi_ssid, "%s", ssid);
  snprintf(g_wifi_ap, sizeof g_wifi_ap, "%s", ap);
  g_wifi_rssi = rssi;
  g_wifi_ch = channel;
  if (changed) wifi_refresh();
}
void ui_set_wifi_target(const char *ssid) { snprintf(g_wifi_target, sizeof g_wifi_target, "%s", ssid); }
void ui_set_machine_host(const char *host) { snprintf(g_mach_host, sizeof g_mach_host, "%s", host); }
void ui_set_wifi_scan(const bd_wifi_net *n, int count, bool scanning) {
  g_nets_n = count < 0 ? 0 : count > BD_MAX_NETS ? BD_MAX_NETS : count;
  memcpy(g_nets, n, sizeof(bd_wifi_net) * g_nets_n);
  g_nets_scanning = scanning;
  wifi_refresh();
}

static void set_bars(int row, int rssi, bool cur) {
  const int n = rssi >= -60 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
  for (int i = 0; i < 4; i++) set_bg(g_wl_bar[row][i], i < n ? (cur ? COL_GREEN : COL_TEXT) : lv_color_hex(0x2E2E33));
}
void wifi_refresh(void) {
  if (!g_wl_note) return;
  set_text(g_wl_note, g_nets_scanning ? "Scanning..." : "2.4 GHz only");
  int r = 0;
  if (g_wifi_ssid[0]) {                             // connected network first, also when the scan missed it
    int ci = -1;
    for (int i = 0; i < g_nets_n; i++) if (!strcmp(g_nets[i].ssid, g_wifi_ssid)) ci = i;
    g_wl_idx[r++] = ci < 0 ? -1 : ci;
  }
  for (int i = 0; i < g_nets_n && r < NET_ROWS; i++)
    if (strcmp(g_nets[i].ssid, g_wifi_ssid) != 0) g_wl_idx[r++] = i;
  char b[64];
  for (int k = 0; k < NET_ROWS; k++) {
    if (k >= r) { set_hidden(g_wl_row[k], true); continue; }
    set_hidden(g_wl_row[k], false);
    const bool cur = k == 0 && g_wifi_ssid[0];
    const bd_wifi_net *n = g_wl_idx[k] >= 0 ? &g_nets[g_wl_idx[k]] : NULL;
    set_text(g_wl_name[k], cur ? g_wifi_ssid : n->ssid);
    set_bars(k, cur ? g_wifi_rssi : n->rssi, cur);
    if (cur) snprintf(b, sizeof b, "%d dBm  \xC2\xB7  channel %d  \xC2\xB7  %s", g_wifi_rssi, g_wifi_ch, g_off_sip);
    set_text(g_wl_sub[k], cur ? b : "");
    set_hidden(g_wl_conn[k], !cur);
    set_hidden(g_wl_chev[k], cur);
    set_hidden(g_wl_lock[k], cur || !n || !n->locked);
  }
  set_hidden(g_wl_empty.box, r > 0);
  if (g_nets_scanning) empty_set(g_wl_empty, "Looking for networks", "2.4 GHz networks only.");
  else empty_set(g_wl_empty, "No networks found", "Tap scan, top right, to look again.");
}

static void pw_refresh(void) {
  char b[80];
  const size_t n = strlen(g_pw);
  set_text(g_pw_field_l, g_kb_addr ? "Address" : "Password");
  set_hidden(g_pw_eye, g_kb_addr);
  set_text(g_pw_join_l, g_kb_addr ? "Use" : "Join");
  if (g_pw_show || g_kb_addr) snprintf(b, sizeof b, "%s", g_pw);
  else { size_t i = 0; for (; i < n && i < sizeof b - 1; i++) b[i] = '*'; b[i] = 0; }
  set_text(g_pw_txt, b);
  const bool can = g_kb_addr ? n >= 1 : n >= 8 && g_wj_state != BD_JOIN_BUSY;
  set_bg(g_pw_join, can ? COL_GOLD : COL_CARD);
  lv_obj_set_style_text_color(g_pw_join_l, can ? COL_BG : COL_OUTLINE, 0);
  char jb[64];
  snprintf(jb, sizeof jb, "Joining %s...", g_wj_ssid);
  if (g_kb_addr) {
    set_text(g_pw_hint, "A name (gaggiuino.local) or an IP address (192.168.1.50). Saved on this screen.");
    lv_obj_set_style_text_color(g_pw_hint, COL_MUTED, 0);
  } else
  set_text(g_pw_hint, g_wj_state == BD_JOIN_BUSY ? jb :
                      g_wj_state == BD_JOIN_FAILED ? "Could not join. Wrong password? Check it and try again." :
                      "Saved on this screen. The machine has its own Wi-Fi setting.");
  if (!g_kb_addr) lv_obj_set_style_text_color(g_pw_hint, g_wj_state == BD_JOIN_FAILED ? COL_WARN : COL_MUTED, 0);
  // key labels for the layer / shift state
  for (int r = 0, k = 0; r < 3; r++) {
    const char *row = KL[g_layer][r];
    if (r == 2) k++;                                // shift key
    for (const char *c = row; *c; c++, k++) {
      char t[2] = {*c, 0};
      if (g_layer == 0 && (g_shift || g_caps)) t[0] = (char)(t[0] - 'a' + 'A');
      set_text(g_key_l[k], t);
    }
    if (r == 2) k++;                                // backspace
  }
  set_text(g_key_l[K_LAYER], g_layer ? "abc" : "123 #");
  set_text(g_key_l[K_SHIFT], g_layer == 0 ? "" : g_layer == 1 ? "#+=" : "123");   // shift key text on symbol layers
  lv_obj_t *arrow = (lv_obj_t *)lv_obj_get_user_data(g_key[K_SHIFT]);
  if (arrow) set_hidden(arrow, g_layer != 0);
  set_bg(g_key[K_SHIFT], g_layer == 0 && g_caps ? COL_MUTED : (g_layer == 0 && g_shift ? COL_CARD_PR : COL_CARD));
}

static void wifi_join_now(const char *ssid, const char *pass) {
  snprintf(g_wj_ssid, sizeof g_wj_ssid, "%s", ssid);
  g_wj_state = BD_JOIN_BUSY;
  bd_wifi_join(ssid, pass);
}
void ui_set_wifi_join(int state) {
  g_wj_state = state;
  if (state == BD_JOIN_OK) {
    memset(g_pw, 0, sizeof g_pw);
    // opened from the offline screen ("Choose network"): done, close the menu (then "Looking for your machine" and
    // Ready follow by themselves); from Menu > Wi-Fi: back to the list
    if (g_menu_entry == MP_WIFI && (g_menu_cur == MP_PASSWORD || g_menu_cur == MP_WIFI)) { menu_close(); return; }
    if (g_menu_cur == MP_PASSWORD) menu_show_page(MP_WIFI);
    bd_wifi_scan();
  }
  if (g_menu_cur == MP_PASSWORD) pw_refresh();
  wifi_refresh();
}

static void on_net_row(lv_event_t *e) {
  const int k = (int)(intptr_t)lv_event_get_user_data(e);
  if (k == 0 && g_wifi_ssid[0]) { menu_show_page(MP_WIFI_DETAILS); return; }
  const bd_wifi_net *n = &g_nets[g_wl_idx[k]];
  snprintf(g_wj_ssid, sizeof g_wj_ssid, "%s", n->ssid);
  if (!n->locked) { wifi_join_now(n->ssid, ""); return; }
  memset(g_pw, 0, sizeof g_pw);
  g_wj_state = BD_JOIN_IDLE;
  g_layer = 0; g_shift = g_caps = false; g_pw_show = true;
  menu_show_page(MP_PASSWORD);
}
static void on_scan(lv_event_t *e) { (void)e; g_nets_scanning = true; wifi_refresh(); bd_wifi_scan(); }
void wifi_page_shown(int k) {
  set_hidden(g_wl_note, k != MP_WIFI);
  set_hidden(g_wl_scan, k != MP_WIFI);
  if (k == MP_WIFI) {
    wifi_refresh();
    if (!g_nets_scanning) { g_nets_scanning = true; bd_wifi_scan(); }
    wifi_refresh();
  }
  if (k == MP_PASSWORD || k == MP_ADDRESS) { g_kb_addr = k == MP_ADDRESS; pw_refresh(); }
}
void open_address_page(void) {
  on_menu_open(NULL);
  snprintf(g_pw, sizeof g_pw, "%s", g_mach_host);
  g_layer = 0; g_shift = g_caps = false;
  menu_show_page(MP_ADDRESS);
}
void on_addr_row(lv_event_t *e) { (void)e; open_address_page(); }
// "Forget this network" (details page): the first tap arms it (orange, "Tap again to forget") for FORGET_ARM_MS, the
// second forgets
lv_obj_t *g_fg_btn, *g_fg_lbl;
static uint32_t g_fg_armed = 0;                     // lv_tick when armed, 0 = not armed
static void forget_paint(void) {
  const bool a = g_fg_armed != 0;
  set_bg(g_fg_btn, a ? COL_WARN_BG : COL_CARD);
  set_tc(g_fg_lbl, a ? COL_WARN : COL_TEXT);
  set_text(g_fg_lbl, a ? "Tap again to forget" : "Forget this network");
}
void on_forget(lv_event_t *e) {
  (void)e;
  if (!g_fg_armed) { g_fg_armed = stamp_nz(lv_tick_get()); forget_paint(); return; }
  g_fg_armed = 0;
  forget_paint();
  bd_wifi_forget();
  g_wifi_target[0] = 0;                             // until the core reports it: no network chosen
  menu_show_page(MP_WIFI);                          // the list: tap a network to join it again
}
void forget_refresh(void) {
  if (!g_fg_btn) return;
  if (g_fg_armed && lv_tick_elaps(g_fg_armed) > FORGET_ARM_MS) { g_fg_armed = 0; forget_paint(); }
  set_hidden(g_fg_btn, !g_wifi_target[0]);
}
static void on_eye(lv_event_t *e) { (void)e; g_pw_show = !g_pw_show; pw_refresh(); }
static void on_join(lv_event_t *e) {
  (void)e;
  if (g_kb_addr) {                                  // machine address: save and look for it at once
    if (!g_pw[0]) return;
    snprintf(g_mach_host, sizeof g_mach_host, "%s", g_pw);
    bd_set_machine_address(g_pw);
    g_off_phase = lv_tick_get();                    // "Looking for your machine" again, at the new address
    menu_close();
    return;
  }
  if (strlen(g_pw) < 8 || g_wj_state == BD_JOIN_BUSY) return;
  wifi_join_now(g_wj_ssid, g_pw);
  pw_refresh();
}
static void on_kb_key(lv_event_t *e) {
  const int k = (int)(intptr_t)lv_event_get_user_data(e);
  size_t n = strlen(g_pw);
  if (g_wj_state == BD_JOIN_BUSY && !g_kb_addr) return;
  if (g_wj_state == BD_JOIN_FAILED) g_wj_state = BD_JOIN_IDLE;
  if (k == K_SHIFT) {                               // shift (letters) / other symbol layer
    if (g_layer) g_layer = g_layer == 1 ? 2 : 1;
    else {
      const uint32_t now = lv_tick_get();
      if (g_caps) { g_caps = false; g_shift = false; }
      else if (g_shift && now - g_shift_ms < DOUBLE_TAP_MS) { g_caps = true; g_shift = false; }
      else g_shift = !g_shift;
      g_shift_ms = now;
    }
  } else if (k == K_BS) {
    if (n) g_pw[n - 1] = 0;
  } else if (k == K_LAYER) { g_layer = g_layer ? 0 : 1; }
  else if (k == K_SPACE) { if (n + 1 < sizeof g_pw) { g_pw[n] = ' '; g_pw[n + 1] = 0; } }
  else if (k == K_DOT) { if (n + 1 < sizeof g_pw) { g_pw[n] = '.'; g_pw[n + 1] = 0; } }
  else {
    const char *t = lv_label_get_text(g_key_l[k]);
    if (t[0] && !t[1] && n + 1 < sizeof g_pw) { g_pw[n] = t[0]; g_pw[n + 1] = 0; }
    if (g_shift) g_shift = false;
  }
  pw_refresh();
}

static lv_obj_t *kb_key(lv_obj_t *row, int w, int k) {
  lv_obj_t *b = box(row, 0, 0, w, 50, COL_CARD, RAD_CTRL);
  make_button(b, COL_CARD_PR);
  g_key[k] = b;
  g_key_l[k] = label(b, "", F_ROW, COL_TEXT);
  lv_obj_center(g_key_l[k]);
  lv_obj_add_event_cb(b, k == K_JOIN ? on_join : on_kb_key, LV_EVENT_CLICKED, (void *)(intptr_t)k);
  return b;
}

void build_wifi(lv_obj_t *m) {
  // header extras on the list page: "2.4 GHz only" + scan button (left of Close)
  g_wl_scan = box(m, 656, 24, 52, 52, COL_CARD, RAD_CTRL);
  make_button(g_wl_scan, COL_CARD_PR);
  {
    static const lv_point_t arc[] = {{17, 6},  {13, 4},  {8, 5},   {5, 8},   {4, 12}, {5, 16},
                                     {8, 19},  {12, 20}, {16, 19}, {19, 16}, {20, 12}};
    static const lv_point_t head[] = {{13, 3}, {18, 6}, {14, 10}};
    lv_obj_t *ic = icon_box(g_wl_scan, 24, 24);
    lv_obj_center(ic);
    line(ic, arc, 11, 2, COL_TEXT);
    line(ic, head, 3, 2, COL_TEXT);
  }
  lv_obj_add_event_cb(g_wl_scan, on_scan, LV_EVENT_CLICKED, NULL);
  g_wl_note = label(m, "2.4 GHz only", F_TEXT, COL_MUTED);
  lv_obj_align(g_wl_note, LV_ALIGN_TOP_RIGHT, -164, 42);

  lv_obj_t *pg = g_menu_page[MP_WIFI] = box(m, 24, 96, 752, 360, COL_BG, 0);
  lv_obj_t *c = flex(pg, LV_FLEX_FLOW_COLUMN, 0);
  hairline(c, LV_BORDER_SIDE_TOP);
  for (int k = 0; k < NET_ROWS; k++) {
    lv_obj_t *r = g_wl_row[k] = box(c, 0, 0, 752, 60, COL_BG, 0);
    hairline(r, LV_BORDER_SIDE_BOTTOM);
    make_button(r, COL_CARD);
    lv_obj_add_event_cb(r, on_net_row, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    lv_obj_t *bars = icon_box(r, 22, 18);
    lv_obj_align(bars, LV_ALIGN_LEFT_MID, 0, 0);
    for (int i = 0; i < 4; i++) g_wl_bar[k][i] = box(bars, i * 6, 13 - i * 4, 4, 5 + i * 4, COL_TEXT, 1);
    lv_obj_t *t = flex(r, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 38, 0);
    g_wl_name[k] = label(t, "", F_ROW, COL_TEXT);
    g_wl_sub[k] = label(t, "", F_TEXT, COL_MUTED);
    lv_obj_t *rt = flex(r, LV_FLEX_FLOW_ROW, 12);
    lv_obj_align(rt, LV_ALIGN_RIGHT_MID, 0, 0);
    g_wl_lock[k] = icon_box(rt, 16, 16);
    {
      static const lv_point_t sh[] = {{5, 7}, {5, 5}, {6, 3}, {8, 2}, {10, 3}, {11, 5}, {11, 7}};
      lv_obj_t *bd = box(g_wl_lock[k], 3, 7, 10, 8, COL_BG, 2);
      lv_obj_set_style_border_width(bd, 2, 0);
      lv_obj_set_style_border_color(bd, COL_MUTED, 0);
      line(g_wl_lock[k], sh, 7, 2, COL_MUTED);
    }
    g_wl_conn[k] = flex(rt, LV_FLEX_FLOW_ROW, 10);
    label(g_wl_conn[k], "Connected", F_TEXT, COL_GREEN);
    icon_check(g_wl_conn[k], COL_GREEN);
    g_wl_chev[k] = icon_box(rt, 16, 16);
    icon_chevron_right(g_wl_chev[k], COL_MUTED);
    set_hidden(r, true);
  }
  g_wl_empty = empty_state(pg, IC_WIFI);

  // password page (and the machine address page)
  pg = g_menu_page[MP_PASSWORD] = g_menu_page[MP_ADDRESS] = box(m, 24, 90, 752, 366, COL_BG, 0);
  lv_obj_t *f = box(pg, 0, 0, 752, 56, COL_WELL, RAD_CTRL);
  lv_obj_set_style_border_width(f, 1, 0);
  lv_obj_set_style_border_color(f, COL_BORDER, 0);
  lv_obj_t *fl = g_pw_field_l = label(f, "Password", F_TEXT, COL_MUTED);
  lv_obj_align(fl, LV_ALIGN_LEFT_MID, 18, 0);
  lv_obj_t *fr = flex(f, LV_FLEX_FLOW_ROW, 2);
  lv_obj_align(fr, LV_ALIGN_LEFT_MID, 116, 0);
  g_pw_txt = label(fr, "", F_ROW, COL_TEXT);
  g_pw_cur = box(fr, 0, 0, 2, 24, COL_GOLD, 0);
  g_pw_eye = box(f, 696, 8, 40, 40, COL_WELL, 10);
  make_button(g_pw_eye, COL_CARD);
  {
    static const lv_point_t top[] = {{2, 11}, {6, 6}, {11, 4}, {16, 6}, {20, 11}};
    static const lv_point_t bot[] = {{2, 11}, {6, 16}, {11, 18}, {16, 16}, {20, 11}};
    lv_obj_t *ic = icon_box(g_pw_eye, 22, 22);
    lv_obj_center(ic);
    line(ic, top, 5, 2, COL_MUTED);
    line(ic, bot, 5, 2, COL_MUTED);
    box(ic, 8, 8, 6, 6, COL_MUTED, 3);           // pupil
  }
  lv_obj_add_event_cb(g_pw_eye, on_eye, LV_EVENT_CLICKED, NULL);
  g_pw_hint = label(pg, "", F_TEXT, COL_MUTED);
  lv_obj_set_pos(g_pw_hint, 0, 70);
  lv_obj_t *kb = flex(pg, LV_FLEX_FLOW_COLUMN, 8);
  lv_obj_set_flex_align(kb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_width(kb, 752);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  int k = 0;
  for (int r = 0; r < 4; r++) {
    lv_obj_t *row = flex(kb, LV_FLEX_FLOW_ROW, 6);
    if (r == 0) for (int i = 0; i < 10; i++) kb_key(row, 68, k++);
    if (r == 1) for (int i = 0; i < 9; i++) kb_key(row, 68, k++);
    if (r == 2) {
      lv_obj_t *sh = kb_key(row, 104, k++);        // shift (K_SHIFT)
      static const lv_point_t ar[] = {{10, 2}, {3, 10}, {7, 10}, {7, 18}, {13, 18}, {13, 10}, {17, 10}, {10, 2}};
      lv_obj_t *ic = icon_box(sh, 20, 20);
      lv_obj_align(ic, LV_ALIGN_CENTER, 0, 0);
      line(ic, ar, 8, 2, COL_TEXT);
      lv_obj_set_event_bubble(ic, true);
      lv_obj_set_user_data(sh, ic);
      for (int i = 0; i < 7; i++) kb_key(row, 68, k++);
      lv_obj_t *bs = kb_key(row, 104, k++);        // backspace
      static const lv_point_t bk[] = {{8, 3}, {22, 3}, {22, 17}, {8, 17}, {2, 10}, {8, 3}};
      static const lv_point_t x1[] = {{11, 7}, {17, 13}};
      static const lv_point_t x2[] = {{17, 7}, {11, 13}};
      lv_obj_t *bi = icon_box(bs, 24, 20);
      lv_obj_align(bi, LV_ALIGN_CENTER, 0, 0);
      line(bi, bk, 6, 2, COL_TEXT);
      line(bi, x1, 2, 2, COL_TEXT);
      line(bi, x2, 2, 2, COL_TEXT);
    }
    if (r == 3) {
      kb_key(row, 104, k++);                       // layer
      kb_key(row, 386, k++);                       // space
      kb_key(row, 68, k++);                        // '.'
      lv_obj_t *j = kb_key(row, 160, k++);         // Join
      g_pw_join = j;
      g_pw_join_l = g_key_l[k - 1];
    }
  }
  set_text(g_key_l[K_SPACE], "space");
  lv_obj_set_style_text_font(g_key_l[K_SPACE], F_TEXT, 0);
  lv_obj_set_style_text_color(g_key_l[K_SPACE], COL_MUTED, 0);
  set_text(g_key_l[K_DOT], ".");
  set_text(g_pw_join_l, "Join");
  lv_obj_set_style_text_font(g_pw_join_l, F_ROW_B, 0);
  lv_obj_set_style_text_font(g_key_l[K_LAYER], F_TEXT, 0);
  lv_obj_set_style_text_font(g_key_l[K_SHIFT], F_TEXT, 0);
}
