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
static lv_obj_t *g_mrow_val[8];                    // value text of each main row
static lv_obj_t *g_info[MENU_PAGES][7];            // status page values: [page][row]
static lv_obj_t *g_disp_val[2];                    // Display page: brightness, idle time
// Display page toggles: [0] idle clock Still / Bouncing, [1] splash at start On / Off
static lv_obj_t *g_disp_tog[2][2], *g_disp_tog_l[2][2];
static void (*g_settings_cb)(int bright, int idle_min, bool bounce, bool splash) = NULL;
bd_machine g_menu_live;
int g_menu_link = BD_LINK_CONNECTING;
static char g_sys_txt[3][64] = {"", "", ""};   // build (version + date), uptime (+ restart reason), memory
static char g_sys_ver[40] = "";                  // the firmware version, "" when not known
// Brewdeck's own releases (ui_set_release): the About page's version row gets an Install button while one is out
static int g_rel_state = BD_RELEASE_UNKNOWN;
static char g_rel_ver[16] = "", g_rel_err[48] = "";
static bool g_rel_installing = false;
static lv_obj_t *g_rel_btn, *g_rel_lbl;
static uint32_t g_rel_armed = 0;                 // lv_tick when Install was tapped once, 0 = not armed
static uint32_t g_rel_note_at = 0;               // lv_tick of a refusal shown in the row ("machine busy")
static const uint32_t REL_ARM_MS = 3000, REL_NOTE_MS = 4000;
static void menu_refresh(void);
static void on_rel_install(lv_event_t *e) {
  (void)e;
  if (!g_rel_armed) {                            // first tap: "Tap again" for 3 s (like Forget this network)
    g_rel_armed = stamp_nz(lv_tick_get());
    menu_refresh();
    return;
  }
  g_rel_armed = 0;
  const bd_result r = bd_release_install();      // OK: the update page takes over (BD_EV_UPDATE)
  if (r == BD_ERR_BUSY) {
    snprintf(g_rel_err, sizeof g_rel_err, "Not while the machine is busy");
    g_rel_note_at = stamp_nz(lv_tick_get());
  }
  bd_log("ui: install %s: %s", g_rel_ver, r == BD_OK ? "started" : r == BD_ERR_BUSY ? "machine busy" : "not available");
  menu_refresh();
}
// Interface pages (menu row "Interface"): the interfaces of the newest release (bd_ui_catalog), each with a picture of
// its Ready screen (bd_ui_preview, copied here: the core's copy lasts one tick); a row opens its details, Install is
// only there (tap, then "Tap again" within 3 s)
static const int IF_ROWS = BD_MAX_UIS, PV_W = 320, PV_H = 192, TH_W = 160, TH_H = 96;
static lv_obj_t *g_if_row[IF_ROWS], *g_if_title[IF_ROWS], *g_if_state[IF_ROWS], *g_if_about[IF_ROWS];
static lv_obj_t *g_if_thumb[IF_ROWS], *g_if_empty;
static uint16_t *g_if_px[IF_ROWS], *g_if_th[IF_ROWS];   // our copies: 320 x 192 and the 160 x 96 thumbnail
static lv_image_dsc_t g_if_dsc[IF_ROWS], g_if_tdsc[IF_ROWS];
static char g_if_ver[16];                        // the release the copies belong to
static int g_if_sel = -1;                        // the entry the details page shows
static lv_obj_t *g_ifd_img, *g_ifd_about, *g_ifd_size, *g_ifd_state, *g_ifd_btn, *g_ifd_btn_lbl;
static uint32_t g_ifd_armed = 0;                 // lv_tick of the first Install tap, 0 = not armed
static char g_ifd_title[24];

static void dsc_init(lv_image_dsc_t *d, const uint16_t *px, int w, int h) {
  memset(d, 0, sizeof *d);
  d->header.magic = LV_IMAGE_HEADER_MAGIC;
  d->header.cf = LV_COLOR_FORMAT_RGB565;
  d->header.w = w;
  d->header.h = h;
  d->header.stride = w * 2;
  d->data = (const uint8_t *)px;
  d->data_size = (uint32_t)(w * h * 2);
}
// takes the core's picture of entry k once it is there: a copy, and a thumbnail of 2 x 2 averages
static void if_take_preview(int k, const char *name) {
  if (g_if_px[k]) return;
  const bd_picture *p = bd_ui_preview(name);    // asking starts the download
  if (!p || p->w != PV_W || p->h != PV_H) return;
  g_if_px[k] = (uint16_t *)bd_alloc(PV_W * PV_H * 2, BD_MEM_PSRAM);
  g_if_th[k] = (uint16_t *)bd_alloc(TH_W * TH_H * 2, BD_MEM_PSRAM);
  if (!g_if_px[k] || !g_if_th[k]) {
    bd_free(g_if_px[k]);
    bd_free(g_if_th[k]);
    g_if_px[k] = g_if_th[k] = NULL;
    return;
  }
  memcpy(g_if_px[k], p->px, PV_W * PV_H * 2);
  for (int y = 0; y < TH_H; y++)
    for (int x = 0; x < TH_W; x++) {
      const uint16_t *s = g_if_px[k] + 2 * y * PV_W + 2 * x;
      const uint16_t q[4] = {s[0], s[1], s[PV_W], s[PV_W + 1]};
      int r = 0, g = 0, b = 0;
      for (int i = 0; i < 4; i++) { r += q[i] >> 11; g += (q[i] >> 5) & 63; b += q[i] & 31; }
      g_if_th[k][y * TH_W + x] = (uint16_t)((r / 4) << 11 | (g / 4) << 5 | (b / 4));
    }
  dsc_init(&g_if_dsc[k], g_if_px[k], PV_W, PV_H);
  dsc_init(&g_if_tdsc[k], g_if_th[k], TH_W, TH_H);
  lv_image_set_src(g_if_thumb[k], &g_if_tdsc[k]);
  if (g_if_sel == k) lv_image_set_src(g_ifd_img, &g_if_dsc[k]);
}
static void if_forget_previews(void) {           // another release: other pictures
  for (int k = 0; k < IF_ROWS; k++) {
    lv_image_set_src(g_if_thumb[k], NULL);
    bd_free(g_if_px[k]);
    bd_free(g_if_th[k]);
    g_if_px[k] = g_if_th[k] = NULL;
  }
  lv_image_set_src(g_ifd_img, NULL);
}
static void on_if_row(lv_event_t *e) {
  g_if_sel = (int)(intptr_t)lv_event_get_user_data(e);
  g_ifd_armed = 0;
  const bd_catalog *c = bd_ui_catalog();
  if (g_if_sel >= c->n) return;
  snprintf(g_ifd_title, sizeof g_ifd_title, "%s", c->ui[g_if_sel].title);
  lv_image_set_src(g_ifd_img, g_if_px[g_if_sel] ? &g_if_dsc[g_if_sel] : NULL);
  menu_show_page(MP_IFACE_DETAIL);
}
static void on_ifd_install(lv_event_t *e) {
  (void)e;
  const bd_catalog *c = bd_ui_catalog();
  if (g_if_sel < 0 || g_if_sel >= c->n) return;
  if (!g_ifd_armed) {                            // first tap: "Tap again" for 3 s
    g_ifd_armed = stamp_nz(lv_tick_get());
    menu_refresh();
    return;
  }
  g_ifd_armed = 0;
  const bd_result r = bd_ui_install(c->ui[g_if_sel].name);   // OK: the update page takes over (BD_EV_UPDATE)
  if (r == BD_ERR_BUSY) {
    snprintf(g_rel_err, sizeof g_rel_err, "Not while the machine is busy");
    g_rel_note_at = stamp_nz(lv_tick_get());
  }
  bd_log("ui: switch to interface \"%s\": %s", c->ui[g_if_sel].name,
         r == BD_OK ? "started" : r == BD_ERR_BUSY ? "machine busy" : "not available");
  menu_refresh();
}
static void if_refresh(void) {
  if (!g_if_empty) return;
  const bd_catalog *c = bd_ui_catalog();
  if (strcmp(g_if_ver, c->version)) {
    if_forget_previews();
    snprintf(g_if_ver, sizeof g_if_ver, "%s", c->version);
  }
  const bool shown = g_menu_cur == MP_INTERFACE || g_menu_cur == MP_IFACE_DETAIL;
  for (int k = 0; k < IF_ROWS; k++) {
    const bool on = k < c->n;
    set_hidden(g_if_row[k], !on);
    if (!on) continue;
    const bd_ui_entry *u = &c->ui[k];
    set_text(g_if_title[k], u->title);
    set_text(g_if_about[k], u->about);
    set_text(g_if_state[k], u->current ? "Running" : "");
    if (shown && u->has_preview) if_take_preview(k, u->name);   // only while shown: the download is on demand
  }
  const char *empty = c->n ? NULL
                    : g_rel_state == BD_RELEASE_CHECKING ? "Looking for the newest release..."
                    : g_rel_state == BD_RELEASE_FAILED ? "Could not reach the release list. The screen tries again later."
                    : "The list comes with the newest release: the screen looks for it at start and once a day.";
  set_hidden(g_if_empty, !empty);
  if (empty) set_text(g_if_empty, empty);
  // details
  if (g_if_sel < 0 || g_if_sel >= c->n) return;
  if (g_ifd_armed && lv_tick_elaps(g_ifd_armed) > REL_ARM_MS) g_ifd_armed = 0;
  if (g_rel_note_at && lv_tick_elaps(g_rel_note_at) > REL_NOTE_MS) { g_rel_note_at = 0; g_rel_err[0] = 0; }
  const bd_ui_entry *u = &c->ui[g_if_sel];
  char b[64];
  set_text(g_ifd_about, u->about[0] ? u->about : "");
  snprintf(b, sizeof b, "%.1f MB \xC2\xB7 %s", u->size / 1048576.0, c->version);
  set_text(g_ifd_size, b);
  const bool can = !u->current && !g_rel_installing;
  set_hidden(g_ifd_btn, !can);
  set_text(g_ifd_state, u->current ? "Running now" : g_rel_err[0] ? g_rel_err : "");
  set_tc(g_ifd_state, u->current ? COL_GREEN : COL_WARN);
  set_bg(g_ifd_btn, g_ifd_armed ? COL_WARN_BG : COL_CARD);
  set_tc(g_ifd_btn_lbl, g_ifd_armed ? COL_WARN : COL_TEXT);
  set_text(g_ifd_btn_lbl, g_ifd_armed ? "Tap again to install" : "Install");
}
static void build_interface_page(void) {
  // the list: one 112 px row per interface (thumbnail, title, one line), the note at the bottom
  lv_obj_t *pg = g_menu_page[MP_INTERFACE] = box(g_menu, 24, 96, 752, 360, COL_BG, 0);
  tag(pg, "screen.interface");
  lv_obj_t *c = flex(pg, LV_FLEX_FLOW_COLUMN, 0);
  lv_obj_set_size(c, 752, 304);
  lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);    // more interfaces than fit: the list scrolls
  hairline(c, LV_BORDER_SIDE_TOP);
  static const char *const ROW_TAG[2] = {"row.interface.0", "row.interface.1"};
  for (int k = 0; k < IF_ROWS; k++) {
    lv_obj_t *r = g_if_row[k] = box(c, 0, 0, 752, 112, COL_BG, 0);
    hairline(r, LV_BORDER_SIDE_BOTTOM);
    make_button(r, COL_CARD);
    lv_obj_add_event_cb(r, on_if_row, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    if (k < 2) tag(r, ROW_TAG[k]);
    lv_obj_t *frame = box(r, 0, 8, TH_W, TH_H, COL_CARD, RAD_INNER);   // grey until the picture is there
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_border_width(frame, 1, 0);   // a dark interface on the dark page still has an edge
    lv_obj_set_style_border_color(frame, COL_BORDER, 0);
    lv_obj_set_style_border_post(frame, true, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    g_if_thumb[k] = lv_image_create(frame);
    lv_obj_set_pos(g_if_thumb[k], 0, 0);
    g_if_title[k] = label(r, "", F_ROW_B, COL_TEXT);
    lv_obj_set_pos(g_if_title[k], TH_W + 20, 24);
    g_if_about[k] = label(r, "", F_TEXT, COL_MUTED);
    lv_label_set_long_mode(g_if_about[k], LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(g_if_about[k], 752 - TH_W - 20 - 120);
    lv_obj_set_pos(g_if_about[k], TH_W + 20, 54);
    g_if_state[k] = label(r, "", F_TEXT, COL_GREEN);
    lv_obj_align(g_if_state[k], LV_ALIGN_RIGHT_MID, -28, 0);
    lv_obj_t *ch = icon_box(r, 16, 16);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, 0, 0);
    icon_chevron_right(ch, COL_MUTED);
    lv_obj_set_hidden(r, true);
  }
  g_if_empty = label(c, "", F_TEXT, COL_MUTED);
  lv_obj_set_style_pad_top(g_if_empty, 16, 0);
  lv_obj_t *n = label(pg, "Tap one to see it. To go back after installing another, hold the screen for 5 s while it "
                          "starts.", F_TEXT, COL_MUTED);
  lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(n, 752);
  lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 0, -4);

  // the details: the picture at full size, the description, size and release, Install (or "Running now")
  lv_obj_t *d = g_menu_page[MP_IFACE_DETAIL] = box(g_menu, 24, 96, 752, 360, COL_BG, 0);
  tag(d, "screen.interface_detail");
  lv_obj_t *frame = box(d, 0, 0, PV_W, PV_H, COL_CARD, RAD_CARD);
  lv_obj_set_style_clip_corner(frame, true, 0);
  lv_obj_set_style_border_width(frame, 1, 0);
  lv_obj_set_style_border_color(frame, COL_BORDER, 0);
  lv_obj_set_style_border_post(frame, true, 0);
  g_ifd_img = lv_image_create(frame);
  lv_obj_set_pos(g_ifd_img, 0, 0);
  g_ifd_about = label(d, "", F_TEXT, COL_TEXT);
  lv_label_set_long_mode(g_ifd_about, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(g_ifd_about, 752 - PV_W - 24);
  lv_obj_set_pos(g_ifd_about, PV_W + 24, 0);
  g_ifd_size = label(d, "", F_TEXT, COL_MUTED);
  lv_obj_set_pos(g_ifd_size, PV_W + 24, 120);
  g_ifd_state = label(d, "", F_TEXT, COL_GREEN);
  lv_obj_set_pos(g_ifd_state, PV_W + 24, 152);
  g_ifd_btn = box(d, PV_W + 24, PV_H - 52, 260, 52, COL_CARD, RAD_CTRL);
  make_button(g_ifd_btn, COL_CARD_PR);
  lv_obj_add_event_cb(g_ifd_btn, on_ifd_install, LV_EVENT_CLICKED, NULL);
  tag(g_ifd_btn, "action.interface_install");
  g_ifd_btn_lbl = label(g_ifd_btn, "Install", F_TEXT_B, COL_TEXT);
  lv_obj_center(g_ifd_btn_lbl);
  lv_obj_t *n2 = label(d, "Installing it restarts the screen with this interface; your Wi-Fi and machine stay set, and "
                          "it keeps its own settings. To go back, hold the screen for 5 s while it starts.", F_TEXT,
                       COL_MUTED);
  lv_label_set_long_mode(n2, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(n2, 752);
  lv_obj_align(n2, LV_ALIGN_BOTTOM_LEFT, 0, -4);
}

void ui_set_release(int state, const char *version, bool installing, const char *error) {
  g_rel_state = state;
  snprintf(g_rel_ver, sizeof g_rel_ver, "%s", version ? version : "");
  g_rel_installing = installing;
  if (!g_rel_note_at) snprintf(g_rel_err, sizeof g_rel_err, "%s", error ? error : "");
  if (state == BD_RELEASE_AVAILABLE) ntc_release(g_rel_ver);
  menu_refresh();
}
static const int IDLE_OPTS[] = {0, 1, 2, 5, 10, 20, 30, 60};
static const char *const PAGE_NAMES[MENU_PAGES] = {"Display", "Scale", "Wi-Fi", "About", "", "", "Machine address",
                                                     "Shot history",
                                                     "Interface", ""};

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
    const bool rel = g_rel_state == BD_RELEASE_AVAILABLE;
    set_tc(g_mrow_val[5], rel ? COL_BLUE : COL_MUTED);
    if (rel) snprintf(b, sizeof b, "Update available");
    else if (g_upd_known) snprintf(b, sizeof b, "Machine update available");
    else if (g_sys_ver[0]) snprintf(b, sizeof b, "%s", g_sys_ver);
    else if (sscanf(bd, "%d %3s", &day, mon) == 2 || sscanf(bd, "%3s %d", mon, &day) == 2)
      snprintf(b, sizeof b, "Built %d %s", day, mon);
    else snprintf(b, sizeof b, "Screen firmware");
    set_text(g_mrow_val[5], b);
  }
  {                                               // Interface: the running one's title
    const bd_catalog *c = bd_ui_catalog();
    const char *t = "Brewdeck";
    for (int k = 0; k < c->n; k++) if (c->ui[k].current) t = c->ui[k].title;
    set_text(g_mrow_val[7], t);
  }
  if_refresh();
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
  if (g_rel_btn) {                                // Brewdeck version: + the newest release and Install while one is out
    if (g_rel_armed && lv_tick_elaps(g_rel_armed) > REL_ARM_MS) g_rel_armed = 0;
    if (g_rel_note_at && lv_tick_elaps(g_rel_note_at) > REL_NOTE_MS) { g_rel_note_at = 0; g_rel_err[0] = 0; }
    const bool avail = g_rel_state == BD_RELEASE_AVAILABLE && !g_rel_installing;
    set_hidden(g_rel_btn, !avail);
    lv_obj_align(g_info[MP_ABOUT][0], LV_ALIGN_RIGHT_MID, avail ? -(132 + 12) : 0, 0);
    if (avail || g_rel_installing) {
      if (g_rel_err[0]) snprintf(b, sizeof b, "%s", g_rel_err);
      else if (g_rel_installing) snprintf(b, sizeof b, "Installing %s...", g_rel_ver);
      else snprintf(b, sizeof b, "%s available", g_rel_ver);
      set_text(g_info[MP_ABOUT][0], b);
      set_tc(g_info[MP_ABOUT][0], g_rel_err[0] ? COL_WARN : COL_BLUE);
    } else {
      set_tc(g_info[MP_ABOUT][0], COL_TEXT);
    }
    set_bg(g_rel_btn, g_rel_armed ? COL_WARN_BG : COL_CARD);
    set_tc(g_rel_lbl, g_rel_armed ? COL_WARN : COL_TEXT);
    set_text(g_rel_lbl, g_rel_armed ? "Tap again" : "Install");
  }
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
    set_text(g_menu_title, k == MP_IFACE_DETAIL ? g_ifd_title
                           : k == MP_WIFI_DETAILS ? (g_wifi_ssid[0] ? g_wifi_ssid : "Wi-Fi")
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
                 : g_menu_cur == MP_ADDRESS ? MP_WIFI_DETAILS : g_menu_cur == MP_IFACE_DETAIL ? MP_INTERFACE : MP_LIST);
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
  static const struct { const char *name; int page; bool grey; } R[8] = {
    {"Edit profile", -1, true}, {"Scale", MP_SCALE, false}, {"Display", MP_DISPLAY, false},
    {"Maintenance", -1, true}, {"Wi-Fi", MP_WIFI, false}, {"About", MP_ABOUT, false},
    {"Shot history", MP_HISTORY, false}, {"Interface", MP_INTERFACE, false}};
  // only the rows that open something; the web-UI-only items are one line at the bottom
  static const int ORDER[6] = {6, 1, 2, 4, 7, 5};   // Shot history, Scale, Display, Wi-Fi, Interface, About
  static const int ICON[6] = {IC_HISTORY, IC_SCALE, IC_DISPLAY, IC_WIFI, IC_INTERFACE, IC_ABOUT};
  lv_obj_t *col = menu_column(g_menu_main, 0);
  for (int j = 0; j < 6; j++) {
    const int i = ORDER[j];
    lv_obj_t *r = menu_row(col, R[i].name, &g_mrow_val[i], R[i].page >= 0, R[i].grey);
    lv_obj_set_size(r, 752, 56);                  // six rows above the note (were 64 px for five)
    lv_obj_t *ic = line_icon(r, ICON[j], 24, COL_BLUE);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(lv_obj_get_child(r, 0), LV_ALIGN_LEFT_MID, 40, 0);   // the name, after the icon
    if (R[i].page >= 0) lv_obj_add_event_cb(r, on_menu_row, LV_EVENT_CLICKED, (void *)(intptr_t)R[i].page);
    if (R[i].page == MP_WIFI) tag(r, "action.wifi_settings");
    if (R[i].page == MP_INTERFACE) tag(r, "action.interface");
  }
  lv_obj_align(label(g_menu_main, "Profiles and maintenance are in the machine's web UI.", F_TEXT, COL_MUTED),
               LV_ALIGN_BOTTOM_LEFT, 0, -4);

  // pages
  // About: the screen's own 3 rows, then the machine's versions and its update notice
  static const char *const INFO[5][7] = {
    {NULL}, {"Type", "Status", "Weight", "Tare"}, {NULL},
    {"Brewdeck version", "Running for", "Free memory", "Machine firmware", "Machine screen", "Machine web UI",
     "Machine update"},
    {"Network", "Signal", "Access point", "Screen", "Machine"}};
  build_wifi(g_menu);                              // the Wi-Fi list and password / address pages
  build_hist(g_menu);                              // shot history
  build_interface_page();                          // interfaces of the newest release
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
      if (k == MP_ABOUT && i == 0) {                // Install a newer Brewdeck release (hidden until one is out)
        tag(g_info[k][i], "info.release");
        g_rel_btn = box(r, 0, 0, 132, 36, COL_CARD, RAD_CTRL);
        lv_obj_align(g_rel_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        make_button(g_rel_btn, COL_CARD_PR);
        lv_obj_add_event_cb(g_rel_btn, on_rel_install, LV_EVENT_CLICKED, NULL);
        lv_obj_set_ext_click_area(g_rel_btn, 6);    // 48 px touch height (X6) from the 36 px button
        tag(g_rel_btn, "action.release_install");
        g_rel_lbl = label(g_rel_btn, "Install", F_TEXT_B, COL_TEXT);
        lv_obj_center(g_rel_lbl);
        lv_obj_set_hidden(g_rel_btn, true);
      }
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
