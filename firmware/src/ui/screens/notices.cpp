// Machine notifications (banner), the pressure-release glow and the notices list under the bell.
#include "../ui_int.h"

// A bell button left of Profiles keeps the machine's recent notices (newest first, time of the last arrival), a blue
// dot while one arrived without being shown (asleep); a tap opens the list under it, a tap anywhere else closes it.
// The pressure release ("Dropping beats" / "Releasing pressure") is NOT kept here (it lights the screen up when it
// happens: glow + banner). The update notice is kept with the screen's own text. Read-only, sends nothing.
static const int NB_Y = 38;   // the banner's y off Ready (= the status pills' y)
#define NTC_MAX 10   // the list scrolls
struct Notice { char t[64]; uint8_t type; char when[8]; bool unread; };
// colours per notice type (BD_NOTICE_*: info, success, warn, error): banner background, dot, banner text
static const struct { uint32_t bg, dot, tx; } NTC_COL[4] = {
    {0x1A1A1D, 0x7CC4FF, 0xF2F2F3}, {0x10291A, 0x4ADE80, 0x7EE2A8},
    {0x33200F, 0xFFA94D, 0xFFC089}, {0x3A1416, 0xE5484D, 0xFF9EA1}};

// ---- the banner -----------------------------------------------------------------------------------------------------
// A machine notice for ~5 s: in the status pill's place on Ready, centred elsewhere.
// Pill like the status pills (dot, no icon), colour by type; on the top layer (over every screen, never takes taps).
// Wakes the screen (screen off / idle clock) so "Releasing pressure!" is seen. The core leaves out "Fill the water
// tank!" (the machine repeats it ~1.4/s; the water pill shows it already). A repeat of the shown text keeps it up.
static lv_obj_t *g_nb, *g_nb_dot, *g_nb_lbl;
static int g_nb_pill_max = 392;   // Ready: the banner in the pill's place ends before the header's buttons
static uint32_t g_nb_ms = 0;
static bool g_nb_lit = false;                     // backlight raised for a banner over the idle clock
// On Ready the message takes the status pill's place for NB_SHOW_MS (the pill is hidden under it; up to the header's
// buttons, "..." when longer), then the pill comes back; no slide. Elsewhere: centred, sliding in.
static bool g_nb_inpill = false;
// The machine's update notice comes every 2 min: shown once per connection (again when the release changes), never
// wakes the idle clock / screen off and does not count as a touch; Menu > About keeps it ("Machine update").
char g_upd_rel[32] = "";
bool g_upd_known = false;   // the machine reported an update (release name in g_upd_rel, "" if not given)
bool g_upd_shown = false;
static bool nb_on_ready(void);
static void ntc_close_if_covered(void);
static void nb_pill_restore(void) {
  if (!g_nb_inpill) return;
  g_nb_inpill = false;
  lv_obj_set_hidden(g_pill, false);
  lv_obj_set_hidden(g_prof_lbl, false);
}
static const uint32_t NB_SHOW_MS = 5000;
static void nb_anim_y(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, (int32_t)v); }
static void nb_slide(bool in) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, g_nb);
  lv_anim_set_exec_cb(&a, nb_anim_y);
  lv_anim_set_values(&a, in ? -48 : NB_Y, in ? NB_Y : -48);
  lv_anim_set_duration(&a, 220);
  lv_anim_set_path_cb(&a, in ? lv_anim_path_ease_out : lv_anim_path_ease_in);
  if (!in) lv_anim_set_completed_cb(&a, [](lv_anim_t *x) { lv_obj_set_hidden((lv_obj_t *)x->var, true); });
  lv_anim_start(&a);
}
static int g_nb_k;
static char g_nb_txt[96];
static void nb_place(int k, const char *msg, bool shown);
static void on_nb_timer(lv_timer_t *t) {
  (void)t;
  // in the status pill but another screen opened on top (menu, picker, ...): it would sit over that screen's title
  ntc_close_if_covered();
  const bool left = g_nb_inpill && !nb_on_ready();
  if (g_nb_ms && !g_nb_inpill && !lv_obj_is_hidden(g_nb) && nb_on_ready()) nb_place(g_nb_k, g_nb_txt, true);
  if (g_nb_ms && (lv_tick_elaps(g_nb_ms) > NB_SHOW_MS || left)) {
    g_nb_ms = 0;
    if (g_nb_inpill) { lv_obj_set_hidden(g_nb, true); nb_pill_restore(); }
    else nb_slide(false);
    // shown over the idle clock at full backlight: back to the clock's dim level
    if (g_nb_lit && !lv_obj_is_hidden(g_idle)) bl_set(g_idle_bright < g_bright ? g_idle_bright : g_bright);
    g_nb_lit = false;
  }
}
// Pressure-release warning: water comes ~3 s after the machine's countdown starts ("Dropping beats in: 3 / 2 / 1", ~1 s
// apart, then "Releasing pressure!"), so a soft blue glow fades in from the screen edges (FR_D px deep) on each beat
// (full, fading to 25 % in 0.7 s), holds at 60 % while the pressure is released (until the banner goes), then fades
// out. Its opacity falls off like an inset shadow (~26 % at the edge, 17 % at 20 px, 9 % at 40 px) plus a 2 px line at
// 50 %, in four edge strips (only the edges are redrawn; LVGL's own shadow fades out within ~20 px and gets slow when
// wider). Taps pass through. The core recognises the machine's text: a Gaggiuino update that rewords it leaves only
// the banner.
static lv_obj_t *g_fr[4];
static int g_fr_mode = 0;                          // 0 off, 1 countdown, 2 releasing
static int g_fr_opa = 0;                           // 0-255
static uint32_t g_fr_ms = 0;                       // last beat / release message
static const int FR_D = 48;                        // glow depth (px)
// Drawn per pixel into the finished strip: a faint blue over near-black has only a few RGB565 levels, so the fade is
// mixed in 8.8 fixed point and an ordered 4x4 (Bayer) threshold of one 565 step is added before rounding (dithering).
// Speed: slow glow frames let the panel show half-drawn frames while fading. The mixed pixel depends only on (source
// pixel, alpha, threshold), so results are reused: along a row (top / bottom strips: same alpha and background over
// long runs, the threshold repeats every 4 px) and down a column (side strips: alpha depends only on x, the threshold
// repeats every 4 rows); the 16-bit pixel is read / written in one go.
static const uint8_t FR_BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static inline uint16_t fr_mix(uint32_t s, int32_t a, int32_t th) {   // RGB565 s, alpha 0..256, threshold 0..15
  const int32_t r0 = ((s >> 8) & 0xF8) | (s >> 13), g0 = ((s >> 3) & 0xFC) | ((s >> 9) & 3),
                b0 = ((s << 3) & 0xF8) | ((s >> 2) & 7);
  const int32_t r = LV_MIN(((r0 << 8) + (0x7C - r0) * a + th * 128) >> 8, 255);   // COL_BLUE 7CC4FF
  const int32_t g = LV_MIN(((g0 << 8) + (0xC4 - g0) * a + th * 64) >> 8, 255);
  const int32_t b = LV_MIN(((b0 << 8) + (0xFF - b0) * a + th * 128) >> 8, 255);
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// LVGL 9 draws later (draw tasks), so the glow is applied to each finished strip just before it goes to the panel
// (ui_flush_filter, called by the LVGL adapter, bd_lvgl9.h), i.e. over everything. Four invisible edge objects on the
// top layer mark what must be redrawn while it fades.
void ui_flush_filter(int x1, int y1, int x2, int y2, uint16_t *buf, int stride) {
  if (g_fr_opa <= 0 || !g_fr[0] || lv_obj_is_hidden(g_fr[0])) return;
  // alpha per distance from an edge, 0..256: 0.28 at the edge, between linear and squared, plus the 2 px edge line
  // at 50 % (blended over it)
  static uint32_t A[FR_D];
  static int a_for = -1;
  if (a_for != g_fr_opa) {
    a_for = g_fr_opa;
    for (int d = 0; d < FR_D; d++) {
      const float t = FR_D - d - 0.5f;
      float al = g_fr_opa / 255.0f * (72.0f / 255.0f) * t * (t + FR_D) / (2.0f * FR_D * FR_D);
      if (d < 2) al = 1.0f - (1.0f - al) * (1.0f - g_fr_opa / 255.0f * 0.5f);
      A[d] = (uint32_t)(al * 256.0f + 0.5f);   // 0..256
    }
  }
  // column cache for the side strips (x < FR_D or x >= 800 - FR_D): key = pixel << 9 | alpha, results per y & 3
  static uint32_t ck[2 * FR_D];
  static uint16_t co[2 * FR_D][4];
  static uint8_t cm[2 * FR_D];
  for (int y = y1; y <= y2; y++) {
    const int dy = LV_MIN(y, 479 - y);
    const uint32_t ay = dy < FR_D ? A[dy] : 0;
    const uint8_t *thr = FR_BAYER[y & 3];
    const int j = y & 3;
    uint32_t rk = 0xFFFFFFFF;   // row cache: key, results per x & 3
    uint16_t ro[4];
    uint32_t rm = 0;
    // the rows near the top / bottom edge across, the others only within FR_D of the sides
    const int ranges[2][2] = {{x1, ay ? x2 : LV_MIN(x2, FR_D - 1)}, {ay ? x2 + 1 : LV_MAX(x1, 800 - FR_D), x2}};
    for (int ri = 0; ri < 2; ri++)
    for (int x = ranges[ri][0]; x <= ranges[ri][1]; x++) {
      uint16_t *px = buf + (size_t)(y - y1) * stride + (x - x1);
      const int dx = LV_MIN(x, 799 - x);
      const uint32_t ax = dx < FR_D ? A[dx] : 0;
      const uint32_t al = ax + ay - ((ax * ay) >> 8);   // both edges near a corner, like CSS
      if (!al) continue;
      const uint32_t s = *px, key = s << 9 | al;
      if (!ay) {   // side strip (al > 0 means dx < FR_D)
        const int c = x < FR_D ? x : x - (800 - 2 * FR_D);
        if (ck[c] != key) { ck[c] = key; cm[c] = 0; }
        if (!(cm[c] >> j & 1)) { co[c][j] = fr_mix(s, al, thr[x & 3]); cm[c] |= 1 << j; }
        *px = co[c][j];
      } else {
        const int i = x & 3;
        if (key != rk) { rk = key; rm = 0; }
        if (!(rm >> i & 1)) { ro[i] = fr_mix(s, al, thr[i]); rm |= 1 << i; }
        *px = ro[i];
      }
    }
  }
}
// benchmark: the pressure-release glow held at 60 % (on) / off, without its animation
void ui_bench_glow(bool on) {
  lv_anim_delete(g_fr, NULL);
  g_fr_opa = on ? 153 : 0;
  for (lv_obj_t *f : g_fr) { if (on) lv_obj_set_hidden(f, false); else lv_obj_set_hidden(f, true); }
}
static void fr_opa_cb(void *o, int32_t v) {
  (void)o;
  g_fr_opa = (int)v;
  for (lv_obj_t *f : g_fr) lv_obj_invalidate(f);
}
static void fr_fade(int from, int to, uint32_t ms) {
  lv_anim_delete(g_fr, fr_opa_cb);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, g_fr);
  lv_anim_set_exec_cb(&a, fr_opa_cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  if (to == 0) lv_anim_set_completed_cb(&a, [](lv_anim_t *) { for (lv_obj_t *f : g_fr) lv_obj_set_hidden(f, true); });
  lv_anim_start(&a);
}
static void fr_event(int mode) {                   // 1 = a countdown beat, 2 = releasing
  for (lv_obj_t *f : g_fr) lv_obj_set_hidden(f, false);
  g_fr_mode = mode;
  g_fr_ms = stamp_nz(lv_tick_get());
  if (mode == 1) fr_fade(255, 64, 700);
  else fr_fade(g_fr_opa > 153 ? g_fr_opa : 255, 153, 300);
}
static void on_fr_timer(lv_timer_t *t) {
  (void)t;
  if (!g_fr_mode) return;
  // countdown without a release after it: gone 1.5 s after the last beat; release: lit while the banner is up
  const uint32_t keep = g_fr_mode == 1 ? 1500 : 5000;
  if (lv_tick_elaps(g_fr_ms) > keep) { fr_fade(g_fr_opa, 0, 500); g_fr_mode = 0; }
}
void build_frame(void) {
  const int st[4][4] = {{0, 0, 800, FR_D}, {0, 480 - FR_D, 800, FR_D}, {0, FR_D, FR_D, 480 - 2 * FR_D},
                        {800 - FR_D, FR_D, FR_D, 480 - 2 * FR_D}};
  for (int i = 0; i < 4; i++) {
    g_fr[i] = box(lv_layer_top(), st[i][0], st[i][1], st[i][2], st[i][3], COL_BLUE, 0);
    if (i == 0) tag(g_fr[i], "warn.release");     // the four edges show together
    lv_obj_set_style_bg_opa(g_fr[i], LV_OPA_TRANSP, 0);
    lv_obj_set_hidden(g_fr[i], true);
  }
  lv_timer_create(on_fr_timer, 100, NULL);
}
// ---- machine notices list -------------------------------------------------------------------------------------------
static Notice g_ntc[NTC_MAX];
static int g_ntc_n = 0;
static bool g_ntc_unread = false;
static lv_obj_t *g_ntc_btn, *g_ntc_badge, *g_ntc_back, *g_ntc_card, *g_ntc_row[NTC_MAX], *g_ntc_dot[NTC_MAX],
    *g_ntc_txt[NTC_MAX], *g_ntc_when[NTC_MAX];
static void ntc_close(void) {
  if (g_ntc_back) lv_obj_set_hidden(g_ntc_back, true);
  if (g_ntc_btn) set_bg(g_ntc_btn, COL_CARD);
}
static void ntc_refresh(void) {
  if (!g_ntc_btn) return;
  g_ntc_unread = false;
  for (int i = 0; i < g_ntc_n; i++) g_ntc_unread |= g_ntc[i].unread;
  set_hidden(g_ntc_btn, g_ntc_n == 0);
  // the name (and a notice banner in the pill's place) ends 16 px before the leftmost header button
  const int32_t bx = g_ntc_n ? lv_obj_get_x(g_ntc_btn) : g_hdr_btn_x;
  lv_obj_set_width(g_hdr_left, bx - 16 - 24);
  g_nb_pill_max = bx - 16 - 24;
  set_hidden(g_ntc_badge, !g_ntc_unread);
  for (int i = 0; i < NTC_MAX; i++) {
    set_hidden(g_ntc_row[i], i >= g_ntc_n);
    if (i >= g_ntc_n) continue;
    set_bg(g_ntc_dot[i], lv_color_hex(NTC_COL[g_ntc[i].type & 3].dot));
    set_text(g_ntc_txt[i], g_ntc[i].t);
    set_text(g_ntc_when[i], g_ntc[i].when);
    lv_obj_set_style_border_width(g_ntc_row[i], i == g_ntc_n - 1 ? 0 : 1, 0);
  }
}
// seen: its banner shows now (the screen lights up for it), so it is not marked unread: the blue dot is only for
// notices nobody saw (the update notice while the screen was asleep)
static void ntc_add(int type, const char *t, bool seen) {
  char when[8] = "";
  if (g_clock_h >= 0) snprintf(when, sizeof when, "%02d:%02d", g_clock_h, g_clock_m);
  int j = 0;
  while (j < g_ntc_n && strcmp(g_ntc[j].t, t)) j++;
  // a text already listed (the update notice every 2 min) moves to the top with the new time, not unread again
  if (j < g_ntc_n) {
    const Notice n = g_ntc[j];
    for (int i = j; i > 0; i--) g_ntc[i] = g_ntc[i - 1];
    g_ntc[0] = n;
    snprintf(g_ntc[0].when, sizeof g_ntc[0].when, "%s", when);
    if (seen) g_ntc[0].unread = false;             // its banner shows this time
  } else {
    if (g_ntc_n < NTC_MAX) g_ntc_n++;
    for (int i = g_ntc_n - 1; i > 0; i--) g_ntc[i] = g_ntc[i - 1];
    snprintf(g_ntc[0].t, sizeof g_ntc[0].t, "%s", t);
    snprintf(g_ntc[0].when, sizeof g_ntc[0].when, "%s", when);
    g_ntc[0].type = (uint8_t)(type >= 0 && type <= 3 ? type : 0);
    g_ntc[0].unread = !seen;
  }
  ntc_refresh();
}
static void on_ntc_btn(lv_event_t *e) {
  (void)e;
  if (!lv_obj_is_hidden(g_ntc_back)) { ntc_close(); return; }
  for (int i = 0; i < g_ntc_n; i++) g_ntc[i].unread = false;
  ntc_refresh();
  set_bg(g_ntc_btn, lv_color_hex(0x26262B));
  lv_obj_scroll_to_y(g_ntc_card, 0, LV_ANIM_OFF);   // newest on top
  lv_obj_set_hidden(g_ntc_back, false);
}
void ntc_build_btn(lv_obj_t *scr, int32_t x) {
  g_ntc_btn = box(scr, x, 24, 52, 52, COL_CARD, RAD_CTRL);
  tag(g_ntc_btn, "action.notices");
  make_button(g_ntc_btn, COL_CARD_PR);
  lv_obj_add_event_cb(g_ntc_btn, on_ntc_btn, LV_EVENT_CLICKED, NULL);
  lv_obj_center(line_icon(g_ntc_btn, IC_BELL, 24, COL_PALE_TX));
  g_ntc_badge = box(g_ntc_btn, 30, 11, 11, 11, COL_BLUE, 6);   // unread: blue dot, ringed in the button's colour
  lv_obj_set_style_border_width(g_ntc_badge, 2, 0);
  lv_obj_set_style_border_color(g_ntc_badge, COL_CARD, 0);
  lv_obj_set_hidden(g_ntc_btn, true);
  // the list: on the top layer, over a 50 % black backdrop (so the card clearly sits on top of Ready instead of
  // cutting through it) that closes it on a tap anywhere else
  g_ntc_back = box(lv_layer_top(), 0, 0, 800, 480, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_ntc_back, LV_OPA_50, 0);
  lv_obj_set_clickable(g_ntc_back, true);
  lv_obj_add_event_cb(g_ntc_back, [](lv_event_t *) { ntc_close(); }, LV_EVENT_CLICKED, NULL);
  g_ntc_card = flex(g_ntc_back, LV_FLEX_FLOW_COLUMN, 0);
  tag(g_ntc_card, "screen.notices");
  lv_obj_set_pos(g_ntc_card, 800 - 24 - 440, 84);   // under the bell, right-aligned with the header's buttons
  lv_obj_set_width(g_ntc_card, 440);
  lv_obj_set_style_pad_hor(g_ntc_card, 20, 0);
  lv_obj_set_style_pad_top(g_ntc_card, 8, 0);
  lv_obj_set_style_pad_bottom(g_ntc_card, 12, 0);
  lv_obj_set_style_radius(g_ntc_card, RAD_CARD, 0);
  lv_obj_set_style_bg_color(g_ntc_card, COL_CARD, 0);
  lv_obj_set_style_bg_opa(g_ntc_card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(g_ntc_card, 1, 0);
  lv_obj_set_style_border_color(g_ntc_card, COL_BORDER, 0);
  lv_obj_set_clickable(g_ntc_card, true);   // taps on the card do not close it
  // at most down to the 24 px margin above the bottom edge; scrolls, the cut-off row hints it
  lv_obj_set_style_max_height(g_ntc_card, 480 - 92 - 16, 0);
  lv_obj_set_scrollable(g_ntc_card, true);
  lv_obj_set_scroll_dir(g_ntc_card, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(g_ntc_card, LV_SCROLLBAR_MODE_OFF);
  lv_obj_t *hd = box(g_ntc_card, 0, 0, 400, 36, COL_CARD, 0);
  lv_obj_align(label(hd, "From the machine", F_SMALL, COL_MUTED), LV_ALIGN_LEFT_MID, 0, 0);
  for (int i = 0; i < NTC_MAX; i++) {
    lv_obj_t *r = g_ntc_row[i] = box(g_ntc_card, 0, 0, 400, 56, COL_CARD, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x222226), 0);
    g_ntc_dot[i] = box(r, 0, 23, 10, 10, COL_BLUE, 5);
    g_ntc_txt[i] = label(r, "", F_TEXT, COL_TEXT);
    one_line(g_ntc_txt[i], 312);
    lv_obj_align(g_ntc_txt[i], LV_ALIGN_LEFT_MID, 22, 0);
    g_ntc_when[i] = label(r, "", F_TEXT, COL_MUTED);
    lv_obj_align(g_ntc_when[i], LV_ALIGN_RIGHT_MID, 0, 0);
  }
  lv_obj_set_hidden(g_ntc_back, true);
  ntc_refresh();                                   // sizes the header's name for no bell
}

static bool nb_on_ready(void) {                   // Ready visible (nothing on top of it)
  lv_obj_t *const over[] = {g_brew, g_rv, g_off, g_menu, g_editor, g_pick, g_st, g_idle};
  for (lv_obj_t *o : over) if (o && !lv_obj_is_hidden(o)) return false;
  return true;
}
static void ntc_close_if_covered(void) {          // the list belongs to Ready: closed when a screen opens on top
  if (g_ntc_back && !lv_obj_is_hidden(g_ntc_back) && !nb_on_ready()) ntc_close();
}
// style and place the banner: in the status pill's place while Ready is visible, centred (sliding in) elsewhere.
// Also called by on_nb_timer when Ready comes back while a centred banner is up, so a notice that arrived during
// the offline screen does not stay centred over the header.
static void nb_place(int k, const char *msg, bool shown) {
  if (msg != g_nb_txt) snprintf(g_nb_txt, sizeof g_nb_txt, "%s", msg);
  g_nb_k = k;
  msg = g_nb_txt;
  const bool ready = nb_on_ready();
  // in the status pill, info is blue-tinted like the busy pills (card grey would read as Standby)
  const lv_color_t bg = ready && k == 0 ? COL_BLUE_BG : lv_color_hex(NTC_COL[k].bg);
  const lv_color_t tx = lv_color_hex(ready && k == 0 ? 0xCFE8FF : NTC_COL[k].tx), dot = lv_color_hex(NTC_COL[k].dot);
  set_bg(g_nb, bg);
  lv_obj_set_style_border_color(g_nb, lv_color_mix(dot, bg, 70), 0);
  lv_obj_set_style_border_width(g_nb, ready ? 0 : 1, 0);
  set_bg(g_nb_dot, dot);
  set_tc(g_nb_lbl, tx);
  set_text(g_nb_lbl, msg);
  // width: up to the header's buttons on Ready ("..." past them), the text's own width elsewhere
  lv_point_t sz;
  lv_text_get_size(&sz, msg, F_TEXT_B, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  const int32_t room = (ready ? g_nb_pill_max : 760) - 2 * 16 - 10 - 8;
  if (sz.x > room) one_line(g_nb_lbl, room);
  else {
    lv_label_set_long_mode(g_nb_lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(g_nb_lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  }
  if (ready) {                                     // in the status pill's place, no slide
    lv_anim_delete(g_nb, nb_anim_y);
    lv_obj_align(g_nb, LV_ALIGN_TOP_LEFT, 24, NB_Y - 8);   // Ready: 24 px margin
    lv_obj_set_hidden(g_nb, false);
    lv_obj_set_hidden(g_pill, true);
    lv_obj_set_hidden(g_prof_lbl, true);   // the banner takes the header's left side
    g_nb_inpill = true;
  } else {                                         // other screens: centred, slides in from above
    const bool was_pill = g_nb_inpill;
    nb_pill_restore();
    lv_obj_align(g_nb, LV_ALIGN_TOP_MID, 0, NB_Y);
    if (!shown || was_pill) {
      lv_anim_delete(g_nb, nb_anim_y);
      lv_obj_set_hidden(g_nb, false);
      nb_slide(true);
    }
  }
}
// the banner for a notice (all kinds): wakes the screen and shows it in the pill's place on Ready, centred elsewhere
// fr: pressure-release glow (1 a countdown beat, 2 releasing, 0 none)
static void notify_show(int type, const char *msg, int fr) {
  // standby: stay on the dim clock instead of jumping to Ready; the banner (and the glow) show over it at full
  // backlight, back to dim when the banner goes. Screen off: lights up on the clock. Dimming Off (no clock): screen
  // off wakes to the screen below.
  if (g_blank_on) {
    g_blank_on = g_blank_offline = false;
    lv_obj_set_hidden(g_blank, true);
    if (g_idle_min > 0) { g_idle_force = true; on_idle_timer(NULL); }   // shows the clock now (no Ready in between)
    bd_log("ui: screen on (notification)");
  }
  bl_set(g_bright);
  if (!lv_obj_is_hidden(g_idle)) g_nb_lit = true;
  if (fr) fr_event(fr);
  const int k = type >= 0 && type <= 3 ? type : 0;
  const bool shown = g_nb_ms != 0;
  g_nb_ms = stamp_nz(lv_tick_get());
  nb_place(k, msg, shown);
}
// an ordinary machine notice (the core has taken out the pressure release, the update notice and the water-tank flood)
void ui_notify(int type, const char *msg) {
  if (!msg || !msg[0]) return;
  ntc_add(type, msg, true);                       // always shown: it wakes the screen
  lv_display_trigger_activity(NULL);
  notify_show(type, msg, 0);
}
// "Dropping beats in: N" (count >= 0) / "Releasing pressure!" (count < 0): glow + banner, not kept in the list
void ui_pressure_release(int count) {
  char t[32];
  if (count >= 0) snprintf(t, sizeof t, "Dropping beats in: %d", count);
  else snprintf(t, sizeof t, "Releasing pressure!");
  lv_display_trigger_activity(NULL);
  notify_show(0, t, count >= 0 ? 1 : 2);
}
// the machine's update notice (repeats every 2 min): banner once per connection and only while awake (else with the
// next one), with the screen's own text (the screen cannot install it); kept in the list and in Menu > About
void ui_update_notice(int type, const char *release) {
  if (!release) release = "";
  if (!g_upd_known || strcmp(g_upd_rel, release)) {
    g_upd_known = true;
    snprintf(g_upd_rel, sizeof g_upd_rel, "%s", release);
    g_upd_shown = false;
  }
  const bool asleep = g_blank_on || !lv_obj_is_hidden(g_idle);   // asleep: banner with the next one
  {
    char t[64];
    if (g_upd_rel[0]) snprintf(t, sizeof t, "Update %s available \xC2\xB7 see web UI", g_upd_rel);
    else snprintf(t, sizeof t, "Update available \xC2\xB7 see web UI");
    ntc_add(type, t, !g_upd_shown && !asleep);
  }
  if (g_upd_shown || asleep) return;
  g_upd_shown = true;
  notify_show(type, "Update available \xC2\xB7 see web UI", 0);
}

// the banner (top layer), hidden until a notice comes
void build_notif(void) {
  g_nb = flex(lv_layer_top(), LV_FLEX_FLOW_ROW, 8);
  tag(g_nb, "info.notice");
  lv_obj_set_height(g_nb, 40);
  lv_obj_set_style_pad_hor(g_nb, 16, 0);
  lv_obj_set_style_radius(g_nb, RAD_PILL, 0);
  lv_obj_set_style_bg_opa(g_nb, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(g_nb, 1, 0);
  g_nb_dot = box(g_nb, 0, 0, 10, 10, COL_WARN, 5);
  g_nb_lbl = label(g_nb, "", F_TEXT_B, COL_WARN_TX);
  lv_obj_align(g_nb, LV_ALIGN_TOP_MID, 0, NB_Y);
  lv_obj_set_hidden(g_nb, true);
  lv_timer_create(on_nb_timer, 250, NULL);
}
