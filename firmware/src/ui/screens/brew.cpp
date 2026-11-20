// The brewing screen (live chart and values) and the shot review (whole shot, scrubber, target / previous shot).
#include "../ui_int.h"

// x = ox + t10 * kx; y = base - v10 * k (clamped to the box)
typedef struct { int ox, base; float kx, ky, kt, kw; } ChartMap;
enum { BS_P, BS_F, BS_W, BS_T, BS_TP, BS_TF };
// polyline pen: solid segments, or a 4 on / 5 off dash pattern that runs on across points (as in the design)
typedef struct { lv_layer_t *ctx; lv_draw_line_dsc_t d; bool dashed; float phase; int px, py; } Pen;

// ---- brewing screen and shot review ---------------------------------------------------------------------------------
// Brewing opens on the first point of a shot (~7/s) and shows "Done" when the core reports the shot's end
// (BD_EV_SHOT_START / SHOT_END, ui_bridge.cpp). REVIEW_AFTER_MS later (or on a tap) the shot review opens: the whole shot, a
// scrubber (drag on the chart) and a "Target | Previous" toggle for the dashed line (the profile's targets, or the
// previous stored shot of the same profile, loaded from the machine). Close, or RV_CLOSE_MS without a touch, goes back
// to Ready (or to the list, for a shot opened from the shot history).
// Charts: solid = this shot, dashed = what it is compared with (the targets the machine sent with each point, or the
// previous shot). Drawn by custom-draw objects that dash sloped lines themselves (a pen with its own dash phase, so
// the dashes run on across segments); while brewing only the strip at "now" is invalidated. Design opacities are
// pre-mixed with the background (overlapping round caps of translucent segments would show as dots).
static const int BR_X = 24, BR_Y = 96, BR_W = 752, BR_H = 288;    // brewing chart box: full width
static const int CH_BASE = 261;                                     // brewing + review: baseline, 261 px / 12.5 bar
// the review uses the brewing chart's box, baseline and scales, so the lines stay put when it opens after a shot
static const int RV_X = BR_X, RV_Y = BR_Y, RV_W = BR_W, RV_H = BR_H;
static const int RV_PW = BR_W;                                      // plot width
static const int RV_BASE = CH_BASE;
bd_shot_point *g_br;                          // BD_SHOT_MAX points (11.5 KB) in PSRAM: internal RAM is for Wi-Fi
int g_br_n = 0;
static int g_br_step = 1, g_br_skip = 0, g_br_tmax = 30, g_br_lastx = -1;
int g_br_state = BR_HIDDEN;
int ui_test_brew_shown(void) { return g_br_state == BR_HIDDEN ? 0 : g_br_state == BR_BREWING ? 1 : 2; }
bool g_live_brew = false;
static bool g_br_new = false;                    // a shot started: its first point opens a fresh brewing screen
static bool g_br_prev_asked = false;             // the previous shot was asked for this shot (once per shot)
static uint32_t g_br_done_ms = 0;
static bd_shot_point g_br_cur;
lv_obj_t *g_brew;
static lv_obj_t *g_br_chart, *g_br_pill, *g_br_dot, *g_br_pill_lbl, *g_br_prof, *g_br_clock;
static lv_obj_t *g_br_w, *g_br_target, *g_br_p, *g_br_f, *g_br_temp;
// review
static bd_shot *g_rv_prev;                     // previous shot (PSRAM)
int g_rv_prev_state = RV_PREV_NONE;       // the previous shot: RV_PREV_* (= bd_load)
int g_rv_mode = 0, g_rv_i = 0;
static int g_rv_tmax = 25;
static uint32_t g_rv_touch_ms = 0;
static const uint32_t RV_CLOSE_MS = 60000;     // the review closes after this long untouched
static const uint32_t REVIEW_AFTER_MS = 3000;   // the review opens this long after Done
lv_obj_t *g_rv, *g_rv_chart;
static lv_obj_t *g_rv_res_w, *g_rv_res_t, *g_rv_close_t, *g_rv_tog[2], *g_rv_tog_l[2];
static lv_obj_t *g_rv_v[4][2];
// "vs Profile target": the profile's planned rest after the shot stopped (estimated, preview_plan), drawn dimmer
static bd_shot_point g_rv_plan[240];
static int g_rv_plan_n = 0;
// review of a STORED shot (menu > Shot history): the same screen, fed with the shot the core loaded; its own profile
// (targets, planned rest) instead of the active one; Close goes back to the list
static bool g_rv_hist = false;
static bd_profile_shape *g_rv_shape;                 // that shot's stored profile (PSRAM)
static int g_rv_tw10 = 0, g_rv_tset = -1;         // its stop weight (x10) and set temperature

static float target_g(void) { return g_weight10 > 0 ? g_weight10 / 10.0f : 21.0f; }
// pressure / flow: 0..12.5 at the top of the box (the machine can reach ~12 bar)
static ChartMap br_map(void) {   // 752x288: plot 0..752, baseline 261, 261 px / 12.5 bar
  ChartMap m = {0, CH_BASE, 75.2f / g_br_tmax, 2.088f, 0.245f, 17.96f / target_g()};
  return m;
}
static ChartMap rv_map(void) {   // X = t*RV_PW/axis; y as br_map, weight scaled to the reviewed shot's stop weight
  ChartMap m = br_map();
  m.kx = RV_PW / 10.0f / g_rv_tmax;
  if (g_rv_hist && g_rv_tw10 > 0) m.kw = 17.96f / (g_rv_tw10 / 10.0f);
  return m;
}
static int cm_x(const ChartMap *m, int t10) { return m->ox + (int)(t10 * m->kx + 0.5f); }

static int sp_val(const bd_shot_point *s, int k) {
  switch (k) { case BS_P: return s->p; case BS_F: return s->f; case BS_W: return s->w; case BS_T: return s->temp;
               case BS_TP: return s->tp; default: return s->tf; }
}
static int cm_y(const ChartMap *m, int k, int v) {
  const float y = m->base - v * (k == BS_W ? m->kw : (k == BS_T ? m->kt : m->ky));
  return y < 0 ? 0 : (y > m->base + 4 ? m->base + 4 : (int)(y + 0.5f));
}

static void pen_init(Pen *pn, lv_layer_t *ctx, lv_color_t col, int width, bool dashed) {
  pn->ctx = ctx;
  lv_draw_line_dsc_init(&pn->d);
  pn->d.color = col;
  pn->d.width = width;
  pn->d.round_start = pn->d.round_end = !dashed;
  pn->dashed = dashed;
  pn->phase = 0;
  pn->px = -1;
}
static void pen_to(Pen *pn, int x, int y) {
  if (pn->px < 0 || (x == pn->px && y == pn->py)) { if (pn->px < 0) { pn->px = x; pn->py = y; } return; }
  const lv_area_t *clip = &pn->ctx->_clip_area;
  const bool vis = !(x < clip->x1 - 8 && pn->px < clip->x1 - 8) && !(x > clip->x2 + 8 && pn->px > clip->x2 + 8);
  const int px = pn->px, py = pn->py;
  if (!pn->dashed) {
    if (vis) { lv_point_t a = {(int32_t)px, (int32_t)py}, b = {(int32_t)x, (int32_t)y}; dl(pn->ctx, &pn->d, &a, &b); }
  } else {
    const float dx = x - px, dy = y - py, len = sqrtf(dx * dx + dy * dy);
    float s = 0;
    for (int guard = 0; s < len && guard < 4096; guard++) {
      const float in = pn->phase < 4 ? 4 - pn->phase : 9 - pn->phase;
      // a step that rounds to nothing (phase a hair below a boundary) would loop forever: jump to the boundary
      if (in < 0.01f) { pn->phase = pn->phase < 4 ? 4 : 0; continue; }
      const float e = s + in < len ? s + in : len;
      if (pn->phase < 4 && vis) {
        lv_point_t a = {(int32_t)(px + dx * s / len + 0.5f), (int32_t)(py + dy * s / len + 0.5f)};
        lv_point_t b = {(int32_t)(px + dx * e / len + 0.5f), (int32_t)(py + dy * e / len + 0.5f)};
        dl(pn->ctx, &pn->d, &a, &b);
      }
      pn->phase += e - s;
      if (pn->phase >= 9) pn->phase -= 9;
      s = e;
    }
  }
  pn->px = x;
  pn->py = y;
}

// one value of a shot as a line. Weight: the scale reports less often than the snapshots come, so short runs of the
// same reading (<= 3 points, ~0.4 s) are drawn as a ramp to the next reading instead of a stair step.
static void ch_series(lv_layer_t *ctx, int ox, int oy, const ChartMap *m, const bd_shot_point *pts, int n, int k,
                      lv_color_t col, int width, bool dashed) {
  Pen pn;
  pen_init(&pn, ctx, col, width, dashed);
  for (int i = 0; i < n;) {
    int e = i;
    if (k == BS_W) while (e + 1 < n && pts[e + 1].w == pts[i].w) e++;
    pen_to(&pn, ox + cm_x(m, pts[i].t), oy + cm_y(m, k, sp_val(&pts[i], k)));
    if (e != i && (e - i + 1 > 3 || e == n - 1))
      pen_to(&pn, ox + cm_x(m, pts[e].t), oy + cm_y(m, k, sp_val(&pts[e], k)));
    i = e + 1;
  }
}

static void ch_dot(lv_layer_t *ctx, int x, int y, int r, lv_color_t col) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  d.bg_color = col;
  d.bg_opa = LV_OPA_COVER;
  lv_area_t a = {(int32_t)(x - r), (int32_t)(y - r), (int32_t)(x + r), (int32_t)(y + r)};
  lv_draw_rect(ctx, &d, &a);
}
// the scrub time above the scrubber: F_TEXT_B, centred on it, kept inside the chart (ox..ox+w)
static const int RV_AT_H = 22, RV_AT_HW = 30;      // label height, half width (fits "1:05.0")
static int rv_at_cx(int x, int ox, int w) {
  return x < ox + RV_AT_HW ? ox + RV_AT_HW : x > ox + w - RV_AT_HW ? ox + w - RV_AT_HW : x;
}

// the four live lines (temperature, weight, flow, pressure on top)
static void ch_live(lv_layer_t *ctx, int ox, int oy, const ChartMap *m, const bd_shot_point *pts, int n) {
  // design: 1.25 px at opacity 0.5. 1 px lines look grey when flat and red when sloped (anti-aliasing), so 2 px dimmer
  ch_series(ctx, ox, oy, m, pts, n, BS_T, lv_color_mix(COL_RED, COL_BG, 96), 2, false);
  ch_series(ctx, ox, oy, m, pts, n, BS_W, lv_color_mix(COL_TEXT, COL_BG, 191), 2, false);   // 0.75
  ch_series(ctx, ox, oy, m, pts, n, BS_F, COL_FLOW, 2, false);
  ch_series(ctx, ox, oy, m, pts, n, BS_P, COL_PRESS, 3, false);
}
static void ch_dots(lv_layer_t *ctx, int ox, int oy, const ChartMap *m, const bd_shot_point *s) {
  const int x = ox + cm_x(m, s->t);
  ch_dot(ctx, x, oy + cm_y(m, BS_P, s->p), 5, COL_PRESS);
  ch_dot(ctx, x, oy + cm_y(m, BS_F, s->f), 4, COL_FLOW);    // design radii 5 / 4.5 / 4 / 3.5
  ch_dot(ctx, x, oy + cm_y(m, BS_W, s->w), 4, COL_TEXT);
  ch_dot(ctx, x, oy + cm_y(m, BS_T, s->temp), 3, COL_RED);
}

// brewing + review: grid at 3 / 6 / 9 (bar or ml/s) with the numbers at the right end, baseline, time every 10 s
static void ch_axes(lv_layer_t *ctx, int ox, int oy, const ChartMap *m, int w, int span_s) {
  for (int v = 3; v <= 9; v += 3) {                // 20.88 px per bar / ml/s (m.ky per 0.1)
    const int y = oy + CH_BASE - (int)(v * 10 * m->ky + 0.5f);
    ch_hline(ctx, ox, ox + w, y, COL_GRID);
    char t[4];
    snprintf(t, sizeof t, "%d", v);
    ch_text_c(ctx, ox + w, y - 17, t, LV_TEXT_ALIGN_RIGHT, COL_AXIS);
  }
  ch_hline(ctx, ox, ox + w, oy + CH_BASE, COL_LINE);
  axis_time(ctx, ox, w, oy + CH_BASE + 4, span_s, 10);
}

static void br_draw(lv_event_t *e) {
  lv_layer_t *ctx = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const int ox = a.x1, oy = a.y1;
  const ChartMap m = br_map();
  ch_axes(ctx, ox, oy, &m, BR_W, g_br_tmax);
  if (!g_br_n) return;
  ch_series(ctx, ox, oy, &m, g_br, g_br_n, BS_TF, lv_color_mix(COL_FLOW, COL_BG, 153), 2, true);   // design 0.6
  ch_series(ctx, ox, oy, &m, g_br, g_br_n, BS_TP, lv_color_mix(COL_PRESS, COL_BG, 140), 2, true);   // 0.55
  ch_live(ctx, ox, oy, &m, g_br, g_br_n);
  ch_dots(ctx, ox, oy, &m, &g_br[g_br_n - 1]);
}


static void rv_draw(lv_event_t *e) {
  lv_layer_t *ctx = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const int ox = a.x1, oy = a.y1;
  const ChartMap m = rv_map();
  // grid and axes as on the brewing screen
  ch_axes(ctx, ox, oy, &m, RV_PW, g_rv_tmax);
  if (!g_br_n) return;
  // dashed: the profile's targets during this shot, or the previous shot
  if (g_rv_mode == 1 && g_rv_prev_state == RV_PREV_READY && g_rv_prev->n) {
    ch_series(ctx, ox, oy, &m, g_rv_prev->pt, g_rv_prev->n, BS_W, lv_color_mix(COL_TEXT, COL_BG, 128), 2, true);
    ch_series(ctx, ox, oy, &m, g_rv_prev->pt, g_rv_prev->n, BS_F, lv_color_mix(COL_FLOW, COL_BG, 153), 2, true);
    ch_series(ctx, ox, oy, &m, g_rv_prev->pt, g_rv_prev->n, BS_P, lv_color_mix(COL_PRESS, COL_BG, 140), 2, true);
  } else {
    ch_series(ctx, ox, oy, &m, g_br, g_br_n, BS_TF, lv_color_mix(COL_FLOW, COL_BG, 153), 2, true);
    ch_series(ctx, ox, oy, &m, g_br, g_br_n, BS_TP, lv_color_mix(COL_PRESS, COL_BG, 140), 2, true);
    if (g_rv_plan_n > 1) {                          // planned rest of the profile (estimated): dimmer
      ch_series(ctx, ox, oy, &m, g_rv_plan, g_rv_plan_n, BS_TF, lv_color_mix(COL_FLOW, COL_BG, 120), 2, true);
      ch_series(ctx, ox, oy, &m, g_rv_plan, g_rv_plan_n, BS_TP, lv_color_mix(COL_PRESS, COL_BG, 110), 2, true);
    }
  }
  ch_live(ctx, ox, oy, &m, g_br, g_br_n);
  // scrubber
  const bd_shot_point *s = &g_br[g_rv_i];
  const int x = ox + cm_x(&m, s->t);
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = COL_BORDER;
  d.width = 2;
  lv_point_t p1 = {(int32_t)x, (int32_t)(oy + RV_AT_H + 4)}, p2 = {(int32_t)x, (int32_t)(oy + RV_BASE)};
  dl(ctx, &d, &p1, &p2);
  {                                                // the time at the scrubber, on top of its line
    char b[16];
    snprintf(b, sizeof b, "%u:%02u.%u", (unsigned)(s->t / 600), (unsigned)(s->t / 10 % 60), (unsigned)(s->t % 10));
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = F_TEXT_B;
    ld.color = COL_TEXT;
    ld.align = LV_TEXT_ALIGN_CENTER;
    const int cx = rv_at_cx(x, ox, RV_PW);
    lv_area_t la = {(int32_t)(cx - RV_AT_HW), (int32_t)oy, (int32_t)(cx + RV_AT_HW - 1), (int32_t)(oy + RV_AT_H)};
    dlabel(ctx, &ld, &la, b);
  }
  ch_dots(ctx, ox, oy, &m, s);
  ch_dot(ctx, x, oy + RV_BASE, 9, COL_TEXT);   // handle
}

static void br_pill(bool done) {
  set_bg(g_br_pill, done ? COL_PILL_BG : COL_BREW_BG);   // done: the green status pill, as on the review
  set_bg(g_br_dot, done ? COL_GREEN : COL_GOLD);
  set_tc(g_br_pill_lbl, done ? COL_PILL_TX : COL_BREW_TX);
  set_text(g_br_pill_lbl, done ? "Done" : "Brewing");
}

static void br_target(void) {
  // the stop weight after the weight: "/ 21.0" (the unit follows); nothing without a stop weight
  char b[24];
  if (g_weight10 > 0) snprintf(b, sizeof b, "/ %d.%d", g_weight10 / 10, g_weight10 % 10);
  else b[0] = 0;
  set_text(g_br_target, b);
}

static void rv_hide(void) { lv_obj_set_hidden(g_rv, true); }

static void br_start(void) {
  idle_hide();                                       // same frame as the brewing screen: one full redraw, not two
  g_br_n = 0;
  g_br_step = 1;
  g_br_skip = 0;
  g_br_tmax = 30;
  g_br_lastx = -1;
  g_br_new = false;
  g_br_prev_asked = false;
  g_br_state = BR_BREWING;
  g_rv_hist = false;
  g_rv_prev_state = RV_PREV_NONE;
  memset(&g_br_cur, 0, sizeof g_br_cur);
  picker_close();
  editor_close();
  rv_hide();
  char b[48];
  display_name(g_cur_profile, b, sizeof b, NULL);
  set_text(g_br_prof, b);
  br_pill(false);
  br_target();
  lv_obj_set_hidden(g_brew, false);
  lv_obj_move_to_index(g_brew, -1);
  ready_cover();
  lv_obj_invalidate(g_br_chart);
}

static void br_done(void) {
  g_br_state = BR_DONE;
  g_br_done_ms = lv_tick_get();
  br_pill(true);
  // look up the previous shot of this profile now, so it is there when the review opens; once per shot: after a
  // late point the newest stored shot is this one, and a second lookup would compare the shot with itself
  if (g_br_prev_asked) return;
  g_br_prev_asked = true;
  if (g_cur_profile[0] && bd_load_previous_shot(g_cur_profile, (int)g_br_cur.t) == BD_OK)
    g_rv_prev_state = RV_PREV_LOADING;
}

void br_hide(void) {
  g_br_state = BR_HIDDEN;
  g_rv_hist = false;
  lv_obj_set_hidden(g_brew, true);
  rv_hide();
  ready_cover();
}

// ---- shot review ----------------------------------------------------------------------------------------------------
static void fmt10(unsigned v, char *b, size_t n) { snprintf(b, n, "%u.%u", v / 10, v % 10); }
// values row: values at the scrubber, this shot (solid) and the dashed line (previous shot or the targets)
void rv_values(void) {
  char b[40];
  const bd_shot_point *s = &g_br[g_rv_i];
  const unsigned v[4] = {s->p, s->f, s->w, s->temp};
  for (int i = 0; i < 4; i++) { fmt10(v[i], b, sizeof b); set_text(g_rv_v[i][0], b); }
  const bool prev = g_rv_prev_state == RV_PREV_READY && g_rv_prev->n;
  if (g_rv_mode == 1 && prev) {
    const bd_shot *pt = g_rv_prev;
    int best = 0;
    for (int i = 1; i < pt->n; i++)
      if (abs((int)pt->pt[i].t - (int)s->t) < abs((int)pt->pt[best].t - (int)s->t)) best = i;
    const bd_shot_point *q = &pt->pt[best];
    const bool past = s->t > pt->pt[pt->n - 1].t + 5;     // the previous shot had ended by then
    const unsigned w[4] = {q->p, q->f, q->w, q->temp};
    for (int i = 0; i < 4; i++) {
      if (past) snprintf(b, sizeof b, "-");
      else fmt10(w[i], b, sizeof b);
      set_text(g_rv_v[i][1], b);
    }
  } else {                                        // targets as sent during the shot, the stop weight, the set temp
    fmt10(s->tp, b, sizeof b); set_text(g_rv_v[0][1], b);
    fmt10(s->tf, b, sizeof b); set_text(g_rv_v[1][1], b);
    const unsigned tw = g_rv_hist ? (unsigned)g_rv_tw10 : (unsigned)(g_weight10 > 0 ? g_weight10 : 0);   // 0: "-"
    const int tset = g_rv_hist ? g_rv_tset : g_tset;
    if (tw) fmt10(tw, b, sizeof b); else snprintf(b, sizeof b, "-");
    set_text(g_rv_v[2][1], b);
    if (tset > 0) snprintf(b, sizeof b, "%d.0", tset); else snprintf(b, sizeof b, "-");
    set_text(g_rv_v[3][1], b);
  }
}

static void rv_plan(void) {   // the rest of the active profile from the phase (and moment) where the shot stopped
  g_rv_plan_n = 0;
  const bd_profile_shape *sh = g_rv_hist ? (g_rv_shape && g_rv_shape->n ? g_rv_shape : NULL)
                                      : (g_shape_loaded ? &g_last_shape : NULL);
  if (!sh || g_br_n < 2) return;
  const bd_shot_point *e = &g_br[g_br_n - 1];
  int k = g_br_n - 1;
  while (k > 0 && g_br[k - 1].ph == e->ph) k--;     // first point of the last phase
  g_rv_plan_n = preview_plan(sh, e->ph, (e->t - g_br[k].t) / 10.0f, e->t, e->tp, e->tf, e->w / 10.0f, g_rv_plan,
                             (int)(sizeof g_rv_plan / sizeof g_rv_plan[0]));
}

void rv_axis(void) {   // 25 s, or longer in steps of 5 s so this shot (and the previous one / the plan) fit
  int end = g_br_n ? g_br[g_br_n - 1].t : 0;
  if (g_rv_mode == 0 && g_rv_plan_n && g_rv_plan[g_rv_plan_n - 1].t > end) end = g_rv_plan[g_rv_plan_n - 1].t;
  if (end > 1190) end = 1190;                       // at most 2 min on the axis
  if (g_rv_mode == 1 && g_rv_prev_state == RV_PREV_READY && g_rv_prev->n && g_rv_prev->pt[g_rv_prev->n - 1].t > end)
    end = g_rv_prev->pt[g_rv_prev->n - 1].t;
  g_rv_tmax = 25;
  while (end > (g_rv_tmax - 1) * 10) g_rv_tmax += 5;
}

void rv_toggle_style(void) {
  // per RV_PREV_*, short: the toggle is narrow
  static const char *prev_txt[] = {"Previous", "Previous", "Previous", "No previous", "Previous n/a"};
  set_text(g_rv_tog_l[1], prev_txt[g_rv_prev_state < 5 ? g_rv_prev_state : 4]);
  for (int i = 0; i < 2; i++) {
    const bool on = g_rv_mode == i;
    const bool avail = i == 0 || g_rv_prev_state == RV_PREV_READY;
    lv_obj_set_style_bg_opa(g_rv_tog[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    set_tc(g_rv_tog_l[i], on ? COL_TEXT : (avail ? COL_MUTED : COL_OUTLINE));
  }
}

static void rv_open(void) {
  if (g_br_n < 3) { br_hide(); return; }         // nothing worth reviewing
  g_br_state = BR_REVIEW;
  g_rv_mode = 0;
  g_rv_i = g_br_n - 1;
  g_rv_touch_ms = lv_tick_get();
  char b[48];
  snprintf(b, sizeof b, "%u.%u g", (unsigned)(g_br_cur.w / 10), (unsigned)(g_br_cur.w % 10));
  set_text(g_rv_res_w, b);
  snprintf(b, sizeof b, "%u.%u s", (unsigned)(g_br_cur.t / 10), (unsigned)(g_br_cur.t % 10));
  set_text(g_rv_res_t, b);
  rv_plan();
  rv_axis();
  rv_values();
  rv_toggle_style();
  lv_obj_set_hidden(g_brew, true);
  lv_obj_set_hidden(g_rv, false);
  lv_obj_move_to_index(g_rv, -1);
  ready_cover();
  lv_obj_invalidate(g_rv_chart);
}

static void on_rv_close(lv_event_t *e) { (void)e; br_hide(); }

static void on_rv_toggle(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  g_rv_touch_ms = lv_tick_get();
  if (i == 1 && g_rv_prev_state != RV_PREV_READY) return;
  g_rv_mode = i;
  rv_axis();
  rv_toggle_style();
  rv_values();
  lv_obj_invalidate(g_rv_chart);
}

static void on_rv_scrub(lv_event_t *e) {
  lv_indev_t *in = lv_indev_active();
  if (!in || !g_br_n) return;
  lv_point_t p;
  lv_indev_get_point(in, &p);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const ChartMap m = rv_map();
  const int t10 = (int)((p.x - a.x1 - m.ox) / m.kx);
  int best = 0;
  for (int i = 1; i < g_br_n; i++)
    if (abs((int)g_br[i].t - t10) < abs((int)g_br[best].t - t10)) best = i;
  g_rv_touch_ms = lv_tick_get();
  if (best == g_rv_i) return;
  // redraw only the strips under the old and the new scrubber (line, dots up to r 5, handle r 9: +-12 px), not the
  // whole chart (~148k px, too slow per step on the device); the series clip to the strip (pen_to)
  const int xo = a.x1 + cm_x(&m, g_br[g_rv_i].t), xn = a.x1 + cm_x(&m, g_br[best].t);
  g_rv_i = best;
  rv_values();
  for (int k = 0; k < 2; k++) {
    const int x = k ? xn : xo;
    lv_area_t s = {(int32_t)(x - 12), a.y1, (int32_t)(x + 12), a.y2};
    lv_obj_invalidate_area(g_rv_chart, &s);   // clipped to the chart; LVGL joins overlapping strips
    const int cx = rv_at_cx(x, a.x1, RV_PW);   // and the time label above it
    lv_area_t t = {(int32_t)(cx - RV_AT_HW), a.y1, (int32_t)(cx + RV_AT_HW), (int32_t)(a.y1 + RV_AT_H)};
    lv_obj_invalidate_area(g_rv_chart, &t);
  }
}

void ui_set_prev(int state, const bd_shot *t) {
  if (g_rv_hist) return;                          // a late answer for the last shot while a stored one is shown
  if (!g_rv_prev) return;
  if (state == RV_PREV_READY && t) memcpy(g_rv_prev, t, sizeof *t);
  if (g_br_state == BR_HIDDEN) return;                    // a late answer after the review was closed
  g_rv_prev_state = state;
  if (g_br_state == BR_REVIEW) {
    rv_toggle_style();
    rv_values();
    if (g_rv_mode == 1) { rv_axis(); lv_obj_invalidate(g_rv_chart); }
  }
}

// opens the review on a stored shot (shot history); the menu stays below it, so Close shows the list again
void rv_open_stored(const bd_shot *s, const bd_profile_shape *sh, const bd_shot *prev, int prev_state) {
  const int n = s->n < BD_SHOT_MAX ? s->n : BD_SHOT_MAX;
  if (!g_br || n < 3) return;
  memcpy(g_br, s->pt, sizeof(bd_shot_point) * n);
  g_br_n = n;
  memset(&g_br_cur, 0, sizeof g_br_cur);
  g_br_cur.w = s->pt[n - 1].w;
  g_br_cur.t = s->dur10 > 0 ? (uint16_t)s->dur10 : s->pt[n - 1].t;
  *g_rv_shape = *sh;
  g_rv_tw10 = sh->stop_weight > 0 ? (int)(sh->stop_weight * 10 + 0.5f) : 0;
  g_rv_tset = sh->brew_temp > 0 ? (int)(sh->brew_temp + 0.5f) : -1;
  if (prev_state == RV_PREV_READY) memcpy(g_rv_prev, prev, sizeof *prev);
  g_rv_prev_state = prev_state;
  g_rv_hist = true;
  picker_close();
  editor_close();
  rv_open();
}

static void on_brew_tap(lv_event_t *e) { (void)e; if (g_br_state == BR_DONE) rv_open(); }

static void on_brew_timer(lv_timer_t *t) {
  (void)t;
  if (g_br_state == BR_DONE && lv_tick_elaps(g_br_done_ms) > REVIEW_AFTER_MS) {
    rv_open();
  } else if (g_br_state == BR_REVIEW) {
    const uint32_t el = lv_tick_elaps(g_rv_touch_ms);
    if (el >= RV_CLOSE_MS) { br_hide(); return; }
    const unsigned left = (unsigned)((RV_CLOSE_MS - el + 999) / 1000);
    char b[8];
    snprintf(b, sizeof b, "%u:%02u", left / 60, left % 60);
    set_text(g_rv_close_t, b);
  }
}

void ui_shot_start(void) { if (g_br) g_br_new = true; }
void ui_shot_end(void) { if (g_br_state == BR_BREWING) br_done(); }

void ui_shot(const bd_shot_point *s) {
  if (!g_br) return;                               // no PSRAM: no brewing screen
  if (g_br_new) br_start();
  else if (g_br_state == BR_HIDDEN || g_br_state == BR_REVIEW) return;   // a point with no shot start: a stray resend
  else if (g_br_state == BR_DONE) {                // a late point of the same shot: it goes on
    g_br_state = BR_BREWING;
    br_pill(false);
  }
  g_br_cur = *s;
  // keep the point (thinned 2:1 when full)
  if (++g_br_skip >= g_br_step) {
    g_br_skip = 0;
    if (g_br_n == BD_SHOT_MAX) {
      for (int i = 0; i < BD_SHOT_MAX / 2; i++) g_br[i] = g_br[2 * i];
      g_br_n = BD_SHOT_MAX / 2;
      g_br_step *= 2;
    }
    g_br[g_br_n++] = *s;
  }
  // time axis: 30 s, grows by 15 s when the shot gets near the end of it. Each step squeezes and redraws the whole
  // chart in one frame (~100-140 ms), so few, larger steps: most shots fit in 30 or 45 s
  if ((int)s->t > (g_br_tmax - 1) * 10) {
    while ((int)s->t > (g_br_tmax - 1) * 10) g_br_tmax += 15;
    g_br_lastx = -1;
  }
  const ChartMap m = br_map();
  const int x = cm_x(&m, s->t);
  if (g_br_lastx < 0) lv_obj_invalidate(g_br_chart);
  else {
    lv_area_t a;
    lv_obj_get_coords(g_br_chart, &a);
    a.x2 = a.x1 + x + 8;
    a.x1 = a.x1 + (g_br_lastx < x ? g_br_lastx : x) - 8;
    lv_obj_invalidate_area(g_br_chart, &a);
  }
  g_br_lastx = x;
  // numbers
  char b[24];
  snprintf(b, sizeof b, "%u:%02u", (unsigned)(s->t / 600), (unsigned)(s->t / 10 % 60));
  set_text(g_br_clock, b);
  snprintf(b, sizeof b, "%u.%u", (unsigned)(s->w / 10), (unsigned)(s->w % 10));
  set_text(g_br_w, b);
  snprintf(b, sizeof b, "%u.%u", (unsigned)(s->p / 10), (unsigned)(s->p % 10));
  set_text(g_br_p, b);
  snprintf(b, sizeof b, "%u.%u", (unsigned)(s->f / 10), (unsigned)(s->f % 10));
  set_text(g_br_f, b);
  snprintf(b, sizeof b, "%u.%u", (unsigned)(s->temp / 10), (unsigned)(s->temp % 10));
  set_text(g_br_temp, b);
}

void build_brew(lv_obj_t *scr) {
  // the shot being drawn (~11.5 KB)
  g_br = (bd_shot_point *)bd_alloc(sizeof(bd_shot_point) * BD_SHOT_MAX, BD_MEM_PSRAM);
  g_rv_prev = (bd_shot *)bd_alloc(sizeof(bd_shot), BD_MEM_PSRAM);
  g_rv_shape = (bd_profile_shape *)bd_alloc(sizeof(bd_profile_shape), BD_MEM_PSRAM);
  if (g_br && g_rv_prev && g_rv_shape) {
    memset(g_rv_prev, 0, sizeof *g_rv_prev);
    memset(g_rv_shape, 0, sizeof *g_rv_shape);
  } else {                                         // the screen is built but never shown (ui_shot, rv_open_stored)
    bd_log("ui: no PSRAM for the brewing screen: shots are not shown");
    bd_free(g_br); bd_free(g_rv_prev); bd_free(g_rv_shape);
    g_br = NULL; g_rv_prev = NULL; g_rv_shape = NULL;
  }
  g_brew = box(scr, 0, 0, 800, 480, COL_BG, 0);
  lv_obj_set_clickable(g_brew, true);
  lv_obj_add_event_cb(g_brew, on_brew_tap, LV_EVENT_CLICKED, NULL);
  // header: pill + profile name, shot clock on the right
  lv_obj_t *hl = flex(g_brew, LV_FLEX_FLOW_ROW, 14);
  lv_obj_set_flex_align(hl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_pos(hl, 24, 30);
  g_br_pill = pill_row(hl);
  lv_obj_set_style_pad_hor(g_br_pill, 16, 0);
  lv_obj_set_style_bg_color(g_br_pill, COL_BREW_BG, 0);
  g_br_dot = box(g_br_pill, 0, 0, 10, 10, COL_GOLD, 5);
  g_br_pill_lbl = label(g_br_pill, "Brewing", F_TEXT_B, COL_BREW_TX);
  g_br_prof = label(hl, "", F_TEXT, COL_MUTED);
  g_br_clock = label(g_brew, "0:00", F_VALUE, COL_TEXT);
  lv_obj_align(g_br_clock, LV_ALIGN_TOP_RIGHT, -24, 22);
  // values: one row under the full-width chart, left-aligned: weight (white), pressure, flow, temperature; each a dot
  // in its colour, the number and its unit
  lv_obj_t *vr = flex(g_brew, LV_FLEX_FLOW_ROW, 0);
  lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_pos(vr, 24, VALS_Y);
  lv_obj_set_height(vr, 52);
  g_br_w = strip_value(vr, COL_TEXT, "g");
  g_br_p = strip_value(vr, COL_PRESS, "bar");
  g_br_f = strip_value(vr, COL_FLOW, "ml/s");
  g_br_temp = strip_value(vr, COL_RED, "\xC2\xB0" "C");
  lv_obj_t *cols[4] = {g_br_w, g_br_p, g_br_f, g_br_temp};
  for (int i = 0; i < 4; i++) lv_obj_set_width(lv_obj_get_parent(cols[i]), BR_W / 4);   // four equal columns
  // the stop weight after the weight: "1.3 / 21.0 g"
  lv_obj_t *wi = lv_obj_get_parent(g_br_w);
  g_br_target = label(wi, "/ 21.0", F_ROW, COL_FAINT);
  lv_obj_set_style_pad_bottom(g_br_target, base_pad(F_VALUE, F_ROW), 0);
  lv_obj_move_to_index(g_br_target, 2);
  // chart
  g_br_chart = box(g_brew, BR_X, BR_Y, BR_W, BR_H, COL_BG, 0);
  lv_obj_set_style_bg_opa(g_br_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_clickable(g_br_chart, false);
  lv_obj_add_event_cb(g_br_chart, br_draw, LV_EVENT_DRAW_MAIN, NULL);
  lv_obj_set_hidden(g_brew, true);

  // shot review
  g_rv = box(scr, 0, 0, 800, 480, COL_BG, 0);
  // header: Done pill (a dot like the Ready pill: status pills have a dot, never an icon), "21.8 g in 7.3 s";
  // Close with its countdown on the right
  lv_obj_t *rh = flex(g_rv, LV_FLEX_FLOW_ROW, 16);
  lv_obj_set_flex_align(rh, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_pos(rh, 24, 24);
  lv_obj_set_height(rh, 56);
  lv_obj_t *rp = pill_row(rh);
  lv_obj_set_style_pad_hor(rp, 16, 0);
  lv_obj_set_style_bg_color(rp, COL_PILL_BG, 0);
  box(rp, 0, 0, 10, 10, COL_GREEN, 5);
  label(rp, "Done", F_TEXT_B, COL_PILL_TX);
  lv_obj_t *rc = flex(rh, LV_FLEX_FLOW_ROW, 14);    // one line: "19.6 g in 23.0 s"
  lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_t *rr = flex(rc, LV_FLEX_FLOW_ROW, 8);
  g_rv_res_w = label(rr, "0.0 g", F_TITLE, COL_TEXT);
  label(rr, "in", F_TITLE, COL_MUTED);
  g_rv_res_t = label(rr, "0.0 s", F_TITLE, COL_TEXT);
  lv_obj_t *cl = flex(g_rv, LV_FLEX_FLOW_ROW, 8);
  lv_obj_set_flex_align(cl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_height(cl, 52);
  lv_obj_set_style_pad_hor(cl, 22, 0);
  lv_obj_set_style_radius(cl, RAD_CTRL, 0);
  lv_obj_set_style_bg_opa(cl, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(cl, COL_CARD, 0);
  lv_obj_set_style_bg_color(cl, COL_CARD_PR, LV_STATE_PRESSED);
  lv_obj_align(cl, LV_ALIGN_TOP_RIGHT, -24, 26);
  lv_obj_set_clickable(cl, true);
  lv_obj_add_event_cb(cl, on_rv_close, LV_EVENT_CLICKED, NULL);
  label(cl, "Close", F_TEXT_B, COL_TEXT);
  g_rv_close_t = label(cl, "1:00", F_TEXT, COL_MUTED);
  // chart (drag to scrub)
  g_rv_chart = box(g_rv, RV_X, RV_Y, RV_W, RV_H, COL_BG, 0);
  lv_obj_set_style_bg_opa(g_rv_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_clickable(g_rv_chart, true);
  lv_obj_add_event_cb(g_rv_chart, rv_draw, LV_EVENT_DRAW_MAIN, NULL);
  lv_obj_add_event_cb(g_rv_chart, on_rv_scrub, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(g_rv_chart, on_rv_scrub, LV_EVENT_PRESSING, NULL);
  // what the dashed line compares with: a short two-way toggle in the header, left of Close
  lv_obj_update_layout(cl);
  lv_obj_t *tg = flex(g_rv, LV_FLEX_FLOW_ROW, 2);
  lv_obj_set_style_pad_all(tg, 3, 0);
  lv_obj_set_style_radius(tg, RAD_CTRL, 0);
  lv_obj_set_style_bg_opa(tg, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(tg, COL_WELL, 0);
  static const char *tog[] = {"Target", "Previous"};
  for (int i = 0; i < 2; i++) {
    g_rv_tog[i] = flex(tg, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_align(g_rv_tog[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_height(g_rv_tog[i], 38);
    lv_obj_set_style_pad_hor(g_rv_tog[i], 12, 0);
    lv_obj_set_style_radius(g_rv_tog[i], RAD_INNER, 0);
    lv_obj_set_style_bg_color(g_rv_tog[i], COL_PK_BTN, 0);
    lv_obj_set_clickable(g_rv_tog[i], true);
    lv_obj_add_event_cb(g_rv_tog[i], on_rv_toggle, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    g_rv_tog_l[i] = label(g_rv_tog[i], tog[i], F_TEXT_B, COL_MUTED);
  }
  lv_obj_align(tg, LV_ALIGN_TOP_RIGHT, -(24 + lv_obj_get_width(cl) + 12), 30);
  // values at the scrubber: one row under the chart exactly like brewing (four equal columns): value / dashed
  // value + unit. The time is drawn on the chart, above the scrubber (rv_draw).
  lv_obj_t *rvr = flex(g_rv, LV_FLEX_FLOW_ROW, 0);
  lv_obj_set_flex_align(rvr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_pos(rvr, 24, VALS_Y);
  lv_obj_set_height(rvr, 52);
  static const lv_color_t dots[4] = {COL_PRESS, COL_FLOW, COL_TEXT, COL_RED};
  static const char *const units[4] = {"bar", "ml/s", "g", "\xC2\xB0" "C"};
  static const int order[4] = {2, 0, 1, 3};        // shown like brewing: weight, pressure, flow, temperature
  for (int j = 0; j < 4; j++) {
    const int i = order[j];
    lv_obj_t *it = flex(rvr, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_flex_align(it, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(it, RV_W / 4);
    digit_dot(it, F_VALUE, dots[i], 8);
    g_rv_v[i][0] = label(it, "-", F_VALUE, COL_TEXT);
    lv_obj_t *sl = label(it, "/", F_ROW, COL_FAINT);
    lv_obj_set_style_pad_bottom(sl, base_pad(F_VALUE, F_ROW), 0);
    g_rv_v[i][1] = label(it, "-", F_ROW, COL_FAINT);
    lv_obj_set_style_pad_bottom(g_rv_v[i][1], base_pad(F_VALUE, F_ROW), 0);
    lv_obj_t *u = label(it, units[i], F_ROW, COL_MUTED);
    lv_obj_set_style_pad_bottom(u, base_pad(F_VALUE, F_ROW), 0);
  }
  lv_obj_set_hidden(g_rv, true);
  lv_timer_create(on_brew_timer, 250, NULL);
}
