// The steam screen: boiler temperature and pressure chart while the machine is in steam mode.
#include "../ui_int.h"

#define ST_PTS 240                                  // 1 sample/s: the last 4 minutes

// ---- steam screen ---------------------------------------------------------------------------------------------------
// Layout like the brewing screen: header = Steam pill (+ an orange "Refill N%" pill below 20 %) and the steam clock;
// the brewing chart's full-width box: temperature red with the dashed set point (labelled), pressure blue, since steam
// started; one values row under it (temperature / set point, pressure).
static const int ST_CX = 24, ST_CY = 96, ST_CW = 752, ST_CH = 288;    // chart box = the brewing chart's (BR_*)
lv_obj_t *g_st;
static lv_obj_t *g_st_temp, *g_st_setl, *g_st_chart, *g_st_clock, *g_st_p;
static lv_obj_t *g_st_tank, *g_st_tank_pct, *g_st_wt;
static int16_t g_st_s[ST_PTS];                      // temperature x10
static int16_t g_st_ps[ST_PTS];                     // pressure x100
static int g_st_n = 0, g_st_set_deg = 0;
static uint32_t g_st_last = 0, g_st_t0 = 0;
bool g_st_on = false;
static void st_draw(lv_event_t *e) {
  lv_layer_t *ctx = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  // plot area like br_draw: the full box width, baseline at 261 (box coordinates)
  const int ox = a.x1, oy = a.y1, x0 = ox, x1 = ox + ST_CW, base = oy + 261, top = oy + 22;
  lv_draw_line_dsc_t l;
  lv_draw_line_dsc_init(&l);
  l.color = COL_LINE;
  l.width = 1;
  int lo = g_st_n ? g_st_s[0] : 0, hi = lo;
  for (int i = 0; i < g_st_n; i++) { if (g_st_s[i] < lo) lo = g_st_s[i]; if (g_st_s[i] > hi) hi = g_st_s[i]; }
  if (g_st_set_deg > 0) {
    if (g_st_set_deg * 10 > hi) hi = g_st_set_deg * 10;
    if (g_st_set_deg * 10 < lo) lo = g_st_set_deg * 10;
  }
  lo -= 50;                                         // 5 deg of room below, 8 above (the set point sits near the top)
  hi += 80;
  // the set point's label (right end, 22 px above its line): a grid label next to it is left out
  const int set_y = g_st_set_deg > 0 && g_st_n ? top + (hi - g_st_set_deg * 10) * (base - top) / (hi - lo) : -100;
  // pressure scale: 0..3 bar, grows to the next whole bar above the highest reading (up to 13: the machine can
  // reach ~12 bar)
  int pmax = 300;
  for (int i = 0; i < g_st_n; i++) if (g_st_ps[i] > pmax) pmax = g_st_ps[i];
  pmax = (pmax + 99) / 100 * 100;
  if (pmax > 1300) pmax = 1300;
  const int pstep = pmax <= 400 ? 1 : 3;            // grid every bar up to 4 bar, then at 3 / 6 / 9 / 12
  for (int b = pstep; b * 100 < pmax; b += pstep) {
    const int y = base - b * 100 * (base - top) / pmax;
    ch_hline(ctx, x0, x1, y, COL_GRID);
    if (abs(y - set_y) < 26) continue;
    char t[8];
    snprintf(t, sizeof t, "%d bar", b);
    ch_text_c(ctx, x1, y - 17, t, LV_TEXT_ALIGN_RIGHT, COL_AXIS);
  }
  lv_point_t p1 = {(int32_t)x0, (int32_t)base}, p2 = {(int32_t)x1, (int32_t)base};
  dl(ctx, &l, &p1, &p2);
  const int span = g_st_n - 1 > 60 ? g_st_n - 1 : 60;   // time axis: at least 1 min, grows to 4 min
  axis_time(ctx, x0, x1 - x0, base + 4, span, span <= 120 ? 30 : 60);
  if (!g_st_n) return;
#define ST_X(i) (x0 + 2 + (i) * (x1 - x0 - 4) / span)
#define ST_Y(v) (top + (hi - (v)) * (base - top) / (hi - lo))
#define ST_PY(p) (base - ((p) < 0 ? 0 : (p) > pmax ? pmax : (p)) * (base - top) / pmax)   // pressure x100, 0..pmax
  if (g_st_set_deg > 0) {                           // set point: dashed, labelled once at its right end
    l.color = COL_RED;
    l.opa = LV_OPA_60;
    l.width = 2;
    l.dash_width = 4;
    l.dash_gap = 5;
    p1 = {(int32_t)x0, (int32_t)ST_Y(g_st_set_deg * 10)};
    p2 = {(int32_t)x1, (int32_t)ST_Y(g_st_set_deg * 10)};
    dl(ctx, &l, &p1, &p2);
  }
  for (int k = 0; k < 2; k++) {                     // 0 pressure (blue, under), 1 temperature (red)
    lv_draw_line_dsc_init(&l);
    // pressure: thinner and at ~45 % (mixed with the background): the secondary value here; its live dot stays full
    l.color = k ? COL_RED : lv_color_mix(COL_PRESS, COL_BG, 115);
    l.width = k ? 3 : 2;
    l.round_start = l.round_end = 1;
    for (int i = 1; i < g_st_n; i++) {
      p1 = {(int32_t)ST_X(i - 1), (int32_t)(k ? ST_Y(g_st_s[i - 1]) : ST_PY(g_st_ps[i - 1]))};
      p2 = {(int32_t)ST_X(i), (int32_t)(k ? ST_Y(g_st_s[i]) : ST_PY(g_st_ps[i]))};
      dl(ctx, &l, &p1, &p2);
    }
  }
  if (g_st_set_deg > 0) {                           // the set point's label over the lines (see the backing)
    char t[12];
    snprintf(t, sizeof t, "%d\xC2\xB0", g_st_set_deg);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = F_TEXT;
    ld.color = COL_RED;
    ld.align = LV_TEXT_ALIGN_RIGHT;
    int ly = ST_Y(g_st_set_deg * 10) - 22;
    if (ly < oy) ly = oy;
    // right end of its line, or the left end when a line runs through the right spot (at ~12 bar the pressure and
    // its live dot sit right there); either way on a dark backing, so it stays readable
    lv_point_t tws;
    lv_text_get_size(&tws, t, ld.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t tw = tws.x;
    bool busy_r = false, busy_l = false;
    for (int i = 0; i < g_st_n; i++) {
      const int x = ST_X(i);
      for (int k = 0; k < 2; k++) {
        const int y = k ? ST_Y(g_st_s[i]) : ST_PY(g_st_ps[i]);
        if (y < ly - 4 || y > ly + 22) continue;
        if (x >= x1 - tw - 12) busy_r = true;
        if (x <= x0 + tw + 12) busy_l = true;
      }
    }
    const bool left = busy_r && !busy_l;
    const int bx1 = left ? x0 : x1 - tw - 6, bx2 = left ? x0 + tw + 6 : x1;
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = COL_BG;
    bg.radius = 4;
    const lv_area_t ba = {(int32_t)bx1, (int32_t)(ly - 1), (int32_t)bx2, (int32_t)(ly + 18)};
    lv_draw_rect(ctx, &bg, &ba);
    ld.align = left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT;
    const lv_area_t la = {(int32_t)(left ? x0 + 3 : x1 - 80), (int32_t)ly, (int32_t)(left ? x0 + 80 : x1),
                          (int32_t)(ly + 18)};
    dlabel(ctx, &ld, &la, t);
  }
  for (int k = 0; k < 2; k++) {                     // the live values' dots on top of everything
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = k ? COL_RED : COL_PRESS;
    const int x = ST_X(g_st_n - 1), y = k ? ST_Y(g_st_s[g_st_n - 1]) : ST_PY(g_st_ps[g_st_n - 1]), r = k ? 4 : 3;
    const lv_area_t da = {(int32_t)(x - r), (int32_t)(y - r), (int32_t)(x + r), (int32_t)(y + r)};
    lv_draw_rect(ctx, &dot, &da);
  }
#undef ST_X
#undef ST_Y
#undef ST_PY
}

// called from ui_set_live with every update while online
void st_update(const bd_machine *d, bool on) {
  if (on != g_st_on) {
    g_st_on = on;
    set_hidden(g_st, !on);                          // stays in its build order: menu, brewing, offline, idle above
    ready_cover();
    if (on) {
      g_st_n = 0;                                   // a new steam session: chart and clock from now
      g_st_last = 0;
      g_st_t0 = stamp_nz(lv_tick_get());
    }
  }
  if (!on) return;
  char b[48];
  g_st_set_deg = d->steam_set;
  snprintf(b, sizeof b, d->temperature >= 99.95f ? "%.0f" : "%.1f", d->temperature);
  set_text(g_st_temp, b);
  const uint32_t s = lv_tick_elaps(g_st_t0) / 1000;
  snprintf(b, sizeof b, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  set_text(g_st_clock, b);
  snprintf(b, sizeof b, "%.1f", d->pressure < 0 ? 0.0f : d->pressure);
  set_text(g_st_p, b);
  const bool known = g_st_set_deg > 0;
  if (known) snprintf(b, sizeof b, "/ %d", g_st_set_deg); else b[0] = 0;
  set_text(g_st_setl, b);
  set_hidden(g_st_setl, !known);
  const int lvl = d->water_level < 0 ? 0 : d->water_level > 100 ? 100 : d->water_level;
  int fh = 16 * lvl / 100;
  if (fh < 2) fh = 2;
  set_h(g_st_tank, fh);
  set_y(g_st_tank, 16 - fh);
  set_hidden(g_st_wt, lvl >= 20);
  snprintf(b, sizeof b, "Refill %d%%", lvl);
  set_text(g_st_tank_pct, b);
  if (!g_st_last || lv_tick_elaps(g_st_last) >= 1000) {   // one chart point per second
    // on a 1 s grid (+= 1000, not "now"): this runs every ~150 ms, and "now" would drift ~7 % slow behind the steam
    // clock; after a long gap it starts again
    g_st_last = g_st_last && lv_tick_elaps(g_st_last) < 3000 ? stamp_nz(g_st_last + 1000) : stamp_nz(lv_tick_get());
    if (g_st_n == ST_PTS) {
      memmove(g_st_s, g_st_s + 1, sizeof g_st_s[0] * (ST_PTS - 1));
      memmove(g_st_ps, g_st_ps + 1, sizeof g_st_ps[0] * (ST_PTS - 1));
      g_st_n--;
    }
    g_st_s[g_st_n] = (int16_t)(d->temperature * 10.0f + 0.5f);
    g_st_ps[g_st_n++] = (int16_t)((d->pressure < 0 ? 0 : d->pressure) * 100.0f + 0.5f);
    lv_obj_invalidate(g_st_chart);
  }
}

void build_steam(lv_obj_t *scr) {
  g_st = box(scr, 0, 0, 800, 480, COL_BG, 0);
  lv_obj_set_clickable(g_st, true);     // nothing behind it can be used
  // header: state pill + water, steam clock on the right (like the shot clock)
  lv_obj_t *hl = flex(g_st, LV_FLEX_FLOW_ROW, 14);
  lv_obj_set_pos(hl, 24, 24);
  lv_obj_set_height(hl, 52);
  lv_obj_set_flex_align(hl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *pill = pill_row(hl);
  lv_obj_set_style_pad_left(pill, 16, 0);
  lv_obj_set_style_pad_right(pill, 16, 0);
  lv_obj_set_style_bg_color(pill, COL_BLUE_BG, 0);
  box(pill, 0, 0, 10, 10, COL_BLUE, 5);
  label(pill, "Steam", F_TEXT_B, COL_BLUE);
  // water (the level adds nothing here): only an orange "Refill N%" pill below 20 %, like the Ready footer's rule
  // (a pill only while something needs attention); hidden otherwise
  lv_obj_t *wt = g_st_wt = pill_row(hl);
  lv_obj_set_style_pad_left(wt, 10, 0);
  lv_obj_set_style_pad_right(wt, 12, 0);
  lv_obj_set_style_bg_color(wt, COL_WARN_BG, 0);
  g_st_tank = icon_tank(wt, 60, COL_WARN);
  g_st_tank_pct = label(wt, "", F_TEXT_B, COL_WARN_TX);
  lv_obj_set_hidden(wt, true);
  // the clock top right, like brewing
  g_st_clock = label(g_st, "0:00", F_VALUE, COL_TEXT);
  lv_obj_align(g_st_clock, LV_ALIGN_TOP_RIGHT, -24, 22);
  // values: one row under the full-width chart, like brewing: boiler temperature (red) / set point, pressure (blue)
  lv_obj_t *vr = flex(g_st, LV_FLEX_FLOW_ROW, 0);
  lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_pos(vr, 24, VALS_Y);
  lv_obj_set_height(vr, 52);
  g_st_temp = strip_value(vr, COL_RED, "\xC2\xB0" "C");
  g_st_p = strip_value(vr, COL_PRESS, "bar");
  lv_obj_set_width(lv_obj_get_parent(g_st_temp), ST_CW / 4);   // the brewing screen's columns
  lv_obj_set_width(lv_obj_get_parent(g_st_p), ST_CW / 4);
  g_st_setl = label(lv_obj_get_parent(g_st_temp), "", F_ROW, COL_FAINT);   // "/ 155": the set point
  lv_obj_set_style_pad_bottom(g_st_setl, base_pad(F_VALUE, F_ROW), 0);
  lv_obj_move_to_index(g_st_setl, 2);
  // chart: the brewing chart's box
  g_st_chart = box(g_st, ST_CX, ST_CY, ST_CW, ST_CH, COL_BG, 0);
  lv_obj_set_style_bg_opa(g_st_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_clickable(g_st_chart, false);
  lv_obj_add_event_cb(g_st_chart, st_draw, LV_EVENT_DRAW_MAIN, NULL);
  lv_obj_set_hidden(g_st, true);
}
