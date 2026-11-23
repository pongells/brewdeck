// The offline screen: joining Wi-Fi / looking for the machine, what is wrong and the likely fix.
#include "../ui_int.h"

enum { OFF_NO_CFG = 0, OFF_JOINING, OFF_WIFI_FAIL, OFF_LOOKING, OFF_NOT_FOUND };
enum { OFF_ACT_NONE = 0, OFF_ACT_WIFI, OFF_ACT_RETRY, OFF_ACT_ADDRESS };
// icon tile: Wi-Fi arcs (Wi-Fi states) or a cup (machine states); trying: arcs light up in turn / steam rises. A badge
// on the tile's corner: orange "!" (Wi-Fi failed), orange "?" (machine not found), gold "+" (no Wi-Fi set up).
enum { OI_WIFI_TRY = 0, OI_WIFI_FAIL, OI_WIFI_NONE, OI_CUP_TRY, OI_CUP_FAIL };

// The machine cannot be reached (or no Wi-Fi yet): a full screen saying so, instead of values that cannot be used.
// Shown after OFF_DELAY_MS offline (short dropouts, e.g. at the start of a shot, do not flash it), at once while
// booting; never over a shot in progress or the shot review (they work from the data already here). Hidden as soon as
// the machine answers.
static const uint32_t OFF_DELAY_MS = 5000;
lv_obj_t *g_off;
static lv_obj_t *g_off_title, *g_off_body, *g_off_detail, *g_off_icon;
int g_off_link = BD_LINK_CONNECTING;
uint32_t g_off_since;          // lv_tick when the link was last lost (OFF_DELAY_MS before the screen shows)
uint32_t g_off_phase;          // lv_tick when the current state began (joining Wi-Fi / looking for the machine)
bool g_off_ever_online = false;
bool g_off_active = false;     // set by the first ui_set_live (the render test never sets it)
char g_off_sip[20] = "", g_off_mach[40] = "";

// States (say clearly what is wrong, stop "trying" after OFF_GIVE_UP_MS; the core keeps retrying quietly):
//   no Wi-Fi set up / joining Wi-Fi / not connected to Wi-Fi
//   looking for the machine (after Wi-Fi is up) / machine not found (name or IP can be changed)
static const uint32_t OFF_GIVE_UP_MS = 20000;
static lv_obj_t *g_off_btn[2], *g_off_btn_l[2];
static int g_off_btn_act[2];                   // what each button does: OFF_ACT_*
static int g_off_why = BD_WHY_NONE;               // bd_why, from the core
static char g_off_why_ip[16] = "";
void ui_set_offline_reason(int why, const char *ip) {
  g_off_why = why;
  snprintf(g_off_why_ip, sizeof g_off_why_ip, "%s", ip);
}

static int off_state(void) {
  const bool late = lv_tick_elaps(g_off_phase) >= OFF_GIVE_UP_MS;
  if (g_off_link == BD_LINK_CONNECTING) return !g_wifi_target[0] ? OFF_NO_CFG : late ? OFF_WIFI_FAIL : OFF_JOINING;
  return late ? OFF_NOT_FOUND : OFF_LOOKING;
}
// two buttons: [0] = the main action (gold, like Use / Join), [1] = the other way out (card)
static void off_buttons(int a0, const char *t0, int a1, const char *t1) {
  const int a[2] = {a0, a1};
  const char *t[2] = {t0, t1};
  for (int i = 0; i < 2; i++) {
    g_off_btn_act[i] = a[i];
    set_hidden(g_off_btn[i], a[i] == OFF_ACT_NONE);
    if (a[i] != OFF_ACT_NONE) set_text(g_off_btn_l[i], t[i]);
    static const char *const TAG[4] = {NULL, "action.wifi_settings", "action.retry", "action.machine_address"};
    tag(g_off_btn[i], TAG[a[i]]);                  // the tag follows the button's action
  }
}

// The icon: one family with the logo (its card tile, 112 px, radius 26; drawings on the logo's 96-unit grid) and a
// badge on the tile's corner saying what is wrong (orange "?" / "!") or what to do (gold "+"); the title stays white.
// Cup: a pre-rendered picture (LVGL draws no ellipses; tools/gen_cup.py), the steam = the logo's three wisps, drawn
// live so they can rise and fade in turn. Wi-Fi: three arcs lighting up one after another while joining.
#include "cup_art.h"
static const int OFF_TILE = 112, OFF_TILE_X = 344, OFF_TILE_Y = 72, OFF_ANIM_MS = 50;
static lv_obj_t *g_off_badge, *g_off_badge_dot, *g_off_badge_l;
static int g_off_icon_kind = -1;
static lv_image_dsc_t g_cup_img[2];

static float ease(float u) { return u * u * (3 - 2 * u); }   // ease-in-out between two keyframes
// a wisp of the logo (an S standing on y0, h tall) on the 96 grid, k px per unit, faded by `opa` into the tile's
// colour (solid lines with round joins: drawn half-transparent, the overlapping joins would show as beads). Only the
// part above the cup's rim (grid y 36) is drawn, so the steam rises out of the cup.
static void off_wisp(lv_layer_t *ctx, int ox, int oy, float k, float x, float y0, float h, lv_color_t c, lv_opa_t opa) {
  if (opa < 4) return;
  const float q = h / 4, top_of_cup = 36;
  const float seg[2][8] = {{x, y0, x - 4.5f, y0 - q * 1.1f, x - 4.5f, y0 - q * 1.6f, x, y0 - q * 2},
                           {x, y0 - q * 2, x + 4.5f, y0 - q * 2.4f, x + 4.5f, y0 - q * 3.1f, x, y0 - h}};
  lv_draw_line_dsc_t l;
  lv_draw_line_dsc_init(&l);
  l.color = lv_color_mix(c, COL_CARD, opa);
  l.width = (int32_t)(3.6f * k + 0.5f);
  l.round_start = l.round_end = 1;
  const int N = 8;
  float pyg = y0;
  lv_point_t prev = {(int32_t)(ox + x * k + 0.5f), (int32_t)(oy + y0 * k + 0.5f)};
  for (int s = 0; s < 2; s++)
    for (int i = 1; i <= N; i++) {
      const float u = (float)i / N, v = 1 - u, b0 = v * v * v, b1 = 3 * v * v * u, b2 = 3 * v * u * u, b3 = u * u * u;
      const float px = b0 * seg[s][0] + b1 * seg[s][2] + b2 * seg[s][4] + b3 * seg[s][6];
      const float py = b0 * seg[s][1] + b1 * seg[s][3] + b2 * seg[s][5] + b3 * seg[s][7];
      const lv_point_t p = {(int32_t)(ox + px * k + 0.5f), (int32_t)(oy + py * k + 0.5f)};
      if (pyg <= top_of_cup && py <= top_of_cup) dl(ctx, &l, &prev, &p);
      prev = p;
      pyg = py;
    }
}
static void off_icon_draw(lv_event_t *e) {
  lv_layer_t *ctx = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const int ox = a.x1, oy = a.y1;
  const float k = OFF_TILE / 96.0f;
  const int kind = g_off_icon_kind;
  const uint32_t t = lv_tick_get();
  if (kind <= OI_WIFI_NONE) {
    const lv_point_t c = {(int32_t)(ox + 48 * k + 0.5f), (int32_t)(oy + 70 * k + 0.5f)};
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = kind == OI_WIFI_TRY ? COL_BLUE : COL_BORDER;
    const int dr = (int)(5.5f * k + 0.5f);
    const lv_area_t da = {(int32_t)(c.x - dr), (int32_t)(c.y - dr), (int32_t)(c.x + dr), (int32_t)(c.y + dr)};
    lv_draw_rect(ctx, &dot, &da);
    for (int i = 0; i < 3; i++) {
      lv_draw_arc_dsc_t d;
      lv_draw_arc_dsc_init(&d);
      d.width = (int32_t)(6 * k + 0.5f);
      d.rounded = 1;
      d.color = COL_BORDER;
      if (kind == OI_WIFI_TRY) {                    // 1.8 s cycle, each arc 0.3 s after the one inside it
        const float u = (float)((t + 1800 - i * 300) % 1800) / 1800;
        const float on = u < 0.15f   ? 0
                         : u < 0.30f ? (u - 0.15f) / 0.15f
                         : u < 0.70f ? 1
                         : u < 0.85f ? (0.85f - u) / 0.15f
                                     : 0;
        d.color = lv_color_mix(COL_BLUE, COL_BORDER, (uint8_t)(on * 255));
      }
      darc(ctx, &d, &c, (int)((15 + i * 15) * k + 0.5f), 225, 315);
    }
    return;
  }
  const bool full = kind == OI_CUP_TRY;
  lv_draw_image_dsc_t im;
  lv_draw_image_dsc_init(&im);
  im.src = &g_cup_img[full ? 0 : 1];
  const lv_area_t ia = {(int32_t)(ox + CUP_X), (int32_t)(oy + CUP_Y), (int32_t)(ox + CUP_X + CUP_W - 1),
                        (int32_t)(oy + CUP_Y + CUP_H - 1)};
  lv_draw_image(ctx, &im, &ia);
  if (!full) return;
  static const float X[3] = {37, 48, 59};
  const lv_color_t C[3] = {COL_PRESS, COL_FLOW, COL_RED};
  for (int i = 0; i < 3; i++) {                     // 2.4 s cycle, each wisp 0.5 s after the one before
    const float u = (float)((t + 2400 * 2 - i * 500) % 2400) / 2400;
    const float op = u < 0.30f ? ease(u / 0.30f) : u < 0.75f ? 1 : 1 - ease((u - 0.75f) / 0.25f);
    const float dy = u < 0.75f ? 8 - 12 * ease(u / 0.75f) : -4 - 6 * ease((u - 0.75f) / 0.25f);
    off_wisp(ctx, ox, oy, k, X[i], 34 + dy, 23, C[i], (lv_opa_t)(op * 255));
  }
}
static void off_icon(int kind) {
  if (kind == g_off_icon_kind) return;
  g_off_icon_kind = kind;
  lv_obj_invalidate(g_off_icon);
  const char *sym = kind == OI_CUP_FAIL ? "?" : kind == OI_WIFI_FAIL ? "!" : kind == OI_WIFI_NONE ? "+" : NULL;
  set_hidden(g_off_badge, !sym);
  if (sym) {
    lv_obj_set_style_bg_color(g_off_badge_dot, kind == OI_WIFI_NONE ? COL_GOLD : COL_WARN, 0);
    set_text(g_off_badge_l, sym);
  }
}
static void on_off_anim(lv_timer_t *t) {           // the moving parts: only while trying, on screen
  (void)t;
  if (lv_obj_is_hidden(g_off) || g_blank_on) return;
  if (g_off_icon_kind == OI_WIFI_TRY || g_off_icon_kind == OI_CUP_TRY) lv_obj_invalidate(g_off_icon);
}

static void off_text(void) {
  const int st = off_state();
  char b[220];
  const uint32_t s = lv_tick_elaps(g_off_phase) / 1000;
  char tm[16];
  snprintf(tm, sizeof tm, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  const char *net = g_wifi_ssid[0] ? g_wifi_ssid : g_wifi_target;
  switch (st) {
    case OFF_NO_CFG:
      off_icon(OI_WIFI_NONE);
      set_text(g_off_title, "No Wi-Fi set up");
      set_text(g_off_body, "Choose the network this screen should use.");
      off_buttons(OFF_ACT_WIFI, "Choose network", OFF_ACT_NONE, "");
      set_text(g_off_detail, "");
      break;
    case OFF_JOINING:
      off_icon(OI_WIFI_TRY);
      set_text(g_off_title, "Joining Wi-Fi");
      snprintf(b, sizeof b, "Connecting to %s...", g_wifi_target);
      set_text(g_off_body, b);
      off_buttons(OFF_ACT_NONE, "", OFF_ACT_WIFI, "Wi-Fi settings");
      set_text(g_off_detail, tm);
      break;
    case OFF_WIFI_FAIL:
      off_icon(OI_WIFI_FAIL);
      set_text(g_off_title, "Not connected to Wi-Fi");
      if (g_off_why == BD_WHY_WIFI_AUTH) {
        snprintf(b, sizeof b, "%s did not accept the password. Choose the network again to enter it.", g_wifi_target);
        off_buttons(OFF_ACT_WIFI, "Wi-Fi settings", OFF_ACT_RETRY, "Try again");
      } else {
        if (g_off_why == BD_WHY_WIFI_NO_AP)
          snprintf(b, sizeof b, "%s is not in range. Is the router on?", g_wifi_target);
        else snprintf(b, sizeof b, "Could not join %s.", g_wifi_target);
        off_buttons(OFF_ACT_RETRY, "Try again", OFF_ACT_WIFI, "Wi-Fi settings");
      }
      set_text(g_off_body, b);
      snprintf(b, sizeof b, "Not connected for %s  \xC2\xB7  still trying", tm);
      set_text(g_off_detail, b);
      break;
    case OFF_LOOKING:
      off_icon(OI_CUP_TRY);
      set_text(g_off_title, "Looking for your machine");
      snprintf(b, sizeof b, "Asking for %s on %s...", g_mach_host, net);
      set_text(g_off_body, b);
      off_buttons(OFF_ACT_NONE, "", OFF_ACT_NONE, "");
      snprintf(b, sizeof b, "Screen %s  \xC2\xB7  %s", g_off_sip[0] ? g_off_sip : "-", tm);
      set_text(g_off_detail, b);
      break;
    default:
      off_icon(OI_CUP_FAIL);
      set_text(g_off_title, "Machine not found");
      if (g_off_why == BD_WHY_NO_REPLY && g_off_why_ip[0] && strcmp(g_off_why_ip, g_mach_host) != 0)
        snprintf(b, sizeof b, "%s is at %s but does not answer. Is it switched on?", g_mach_host, g_off_why_ip);
      else if (g_off_why == BD_WHY_NO_REPLY)
        snprintf(b, sizeof b, "Nothing answers at %s. Is the machine switched on?", g_mach_host);
      else
        snprintf(b, sizeof b, "Nothing on %s answers to %s. Is the machine switched on? If it uses another "
                              "name or address, set it here.", net, g_mach_host);
      set_text(g_off_body, b);
      off_buttons(OFF_ACT_ADDRESS, "Machine address", OFF_ACT_WIFI, "Wi-Fi settings");
      snprintf(b, sizeof b, "Screen %s on %s  \xC2\xB7  not found for %s  \xC2\xB7  still trying",
               g_off_sip[0] ? g_off_sip : "-", net[0] ? net : "Wi-Fi", tm);
      set_text(g_off_detail, b);
      break;
  }
}

static void on_off_btn(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  switch (g_off_btn_act[i]) {
    case OFF_ACT_WIFI: open_wifi_menu(); g_menu_entry = g_menu_cur; break;
    case OFF_ACT_ADDRESS: open_address_page(); g_menu_entry = g_menu_cur; break;
    case OFF_ACT_RETRY:
      bd_wifi_retry();
      g_off_phase = lv_tick_get();               // back to "Joining" for another OFF_GIVE_UP_MS
      off_text();
      break;
  }
}
void on_off_timer(lv_timer_t *t) {
  (void)t;
  const bool want = g_off_active && g_off_link != BD_LINK_ONLINE && (g_br_state == BR_HIDDEN) && !g_in_wifi_pages &&
                    (!g_off_ever_online || lv_tick_elaps(g_off_since) >= OFF_DELAY_MS);
  if (want && lv_obj_is_hidden(g_off)) {
    picker_close();
    editor_close();
    lv_obj_set_hidden(g_off, false);
    lv_obj_move_to_index(g_off, -1);
  } else if (!want && !lv_obj_is_hidden(g_off)) {
    lv_obj_set_hidden(g_off, true);
  }
  if (lv_obj_is_hidden(g_off)) return;
  off_text();
}

// Fixed positions in every state (nothing moves when buttons come or go): tile 72..184, title 211, text 257 (their
// baselines match the design), buttons 328, details line 430.
void build_offline(lv_obj_t *scr) {
  g_off = box(scr, 0, 0, 800, 480, COL_BG, 0);
  tag(g_off, "screen.offline");
  lv_obj_set_clickable(g_off, true);    // swallow taps: only the buttons react
  for (int i = 0; i < 2; i++) {
    lv_image_dsc_t &d = g_cup_img[i];
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_RGB565;
    d.header.w = CUP_W;
    d.header.h = CUP_H;
    d.header.stride = CUP_W * 2;
    d.data_size = CUP_W * CUP_H * 2;
    d.data = (const uint8_t *)(i == 0 ? CUP_FULL : CUP_EMPTY);
  }
  g_off_icon = box(g_off, OFF_TILE_X, OFF_TILE_Y, OFF_TILE, OFF_TILE, COL_CARD, 26);
  lv_obj_add_event_cb(g_off_icon, off_icon_draw, LV_EVENT_DRAW_MAIN, NULL);
  // the badge: a 3 px ring of background around a dot
  g_off_badge = box(g_off, OFF_TILE_X + OFF_TILE - 28, OFF_TILE_Y - 8, 36, 36, COL_BG, 18);
  g_off_badge_dot = box(g_off_badge, 3, 3, 30, 30, COL_WARN, 15);
  g_off_badge_l = label(g_off_badge_dot, "?", F_TEXT_B, COL_BG);
  lv_obj_center(g_off_badge_l);
  g_off_title = label(g_off, "Joining Wi-Fi", F_TITLE, COL_TEXT);
  tag(g_off_title, "value.offline_reason");
  lv_obj_set_width(g_off_title, 800);
  lv_obj_set_style_text_align(g_off_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(g_off_title, 0, 211);        // F_TITLE baseline at 239
  g_off_body = label(g_off, "", F_TEXT, COL_MUTED);
  lv_label_set_long_mode(g_off_body, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(g_off_body, 560);
  lv_obj_set_style_text_align(g_off_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(g_off_body, 2, 0);   // 22 px lines
  lv_obj_set_pos(g_off_body, 120, 257);
  lv_obj_t *row = flex(g_off, LV_FLEX_FLOW_ROW, 12);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 328);
  for (int i = 0; i < 2; i++) {
    g_off_btn[i] = box(row, 0, 0, 200, 52, i == 0 ? COL_GOLD : COL_CARD, RAD_CTRL);
    make_button(g_off_btn[i], i == 0 ? lv_color_mix(COL_GOLD, COL_BG, 200) : COL_CARD_PR);
    g_off_btn_l[i] = label(g_off_btn[i], "", F_TEXT_B, i == 0 ? COL_BG : COL_TEXT);
    lv_obj_center(g_off_btn_l[i]);
    lv_obj_add_event_cb(g_off_btn[i], on_off_btn, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  g_off_detail = label(g_off, "", F_TEXT, COL_OUTLINE);
  lv_obj_set_width(g_off_detail, 800);
  lv_obj_set_style_text_align(g_off_detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(g_off_detail, 0, 430);
  off_icon(OI_WIFI_TRY);
  g_off_since = g_off_phase = lv_tick_get();
  lv_timer_create(on_off_timer, 150, NULL);
  lv_timer_create(on_off_anim, OFF_ANIM_MS, NULL);
}

void ui_set_addr(const char *screen_ip, const char *machine) {
  snprintf(g_off_sip, sizeof g_off_sip, "%s", screen_ip);
  snprintf(g_off_mach, sizeof g_off_mach, "%s", machine);
}
