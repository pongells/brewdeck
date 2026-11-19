// Shared helpers: drawing calls, object helpers (box, label, flex, hairline), the logo, line icons, empty states, the
// time-axis labels / values row / pill row the charts share, the date format, and the state several screens use.
#include "../ui_int.h"
#include <time.h>

// ---- LVGL 9 drawing: descriptors carry the geometry and the work is done later (draw tasks), so a label's text is
// copied (text_local) and must not point into a stack buffer otherwise
void dl(lv_layer_t *L, lv_draw_line_dsc_t *d, const lv_point_t *a, const lv_point_t *b) {
  d->p1.x = a->x; d->p1.y = a->y; d->p2.x = b->x; d->p2.y = b->y;
  lv_draw_line(L, d);
}
void darc(lv_layer_t *L, lv_draw_arc_dsc_t *d, const lv_point_t *c, int r, int a0, int a1) {
  d->center = *c; d->radius = (uint16_t)r; d->start_angle = a0; d->end_angle = a1;
  lv_draw_arc(L, d);
}
void dlabel(lv_layer_t *L, lv_draw_label_dsc_t *d, const lv_area_t *a, const char *t) {
  d->text = t; d->text_local = 1;
  lv_draw_label(L, d, a);
}

// the profile name shown until the machine names one
const char *PROFILE_NAME = "Leva 9 SINGLE v0.9";

// ---- small helpers ---------------------------------------------------------------------------
lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, lv_color_t bg, int r) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_clickable(o, false);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, r, 0);
  return o;
}

// one line, cut with "..." past w px (0: the width set elsewhere, e.g. flex grow). LVGL's dots mode only stops at one
// line when the label's height is one line; with a content height it wraps first (seen: long profile names).
void one_line(lv_obj_t *l, int32_t w) {
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  if (w > 0) lv_obj_set_width(l, w);
  lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN)));
}
// a 1 px hairline on one side (list rows, the top of a list); the content is not inset by it
void hairline(lv_obj_t *o, lv_border_side_t side) {
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_side(o, side, 0);
  lv_obj_set_style_border_color(o, COL_LINE, 0);
}
lv_obj_t *label(lv_obj_t *p, const char *text, const lv_font_t *font, lv_color_t col) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, col, 0);
  return l;
}
// Type alignment from the font metrics (works for any pair of theme fonts; labels bottom-aligned in a flex row):
// pad that puts the small text's baseline on the big text's baseline
int base_pad(const lv_font_t *big, const lv_font_t *small) { return big->base_line - small->base_line; }
// a value's dot, centred on the digits of `f`, in a box as tall as the label (bottom-aligned next to it)
void digit_dot(lv_obj_t *row, const lv_font_t *f, lv_color_t c, int d) {
  lv_font_glyph_dsc_t g;
  lv_font_get_glyph_dsc(f, &g, '0', 0);
  const int h = lv_font_get_line_height(f), mid = h - f->base_line - (g.ofs_y + g.box_h / 2);
  lv_obj_t *o = lv_obj_create(row);
  lv_obj_remove_style_all(o);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_clickable(o, false);
  lv_obj_set_size(o, d, h);
  box(o, 0, mid - d / 2, d, d, c, d / 2);
}

// ---- the Brewdeck logo: three wisps in the value colours (blue pressure, gold flow, coral temperature) on
// the controls' card square. Same geometry as docs/images/logo.svg (96-unit grid); drawn, not an image.
static void logo_draw(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target_obj(e);
  lv_layer_t *ctx = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const float k = lv_area_get_width(&a) / 96.0f;
  lv_draw_rect_dsc_t r;
  lv_draw_rect_dsc_init(&r);
  r.bg_color = COL_CARD;
  r.radius = (int32_t)(22 * k);
  lv_area_t ca = {(int32_t)(a.x1 + 4 * k), (int32_t)(a.y1 + 4 * k), (int32_t)(a.x1 + 92 * k), (int32_t)(a.y1 + 92 * k)};
  lv_draw_rect(ctx, &r, &ca);
  lv_draw_line_dsc_t l;
  lv_draw_line_dsc_init(&l);
  l.width = (int32_t)(4.9f * k + 0.5f) < 2 ? 2 : (int32_t)(4.9f * k + 0.5f);
  l.round_start = l.round_end = 1;
  static const int X[3] = {30, 48, 66};
  const lv_color_t C[3] = {COL_PRESS, COL_FLOW, COL_RED};
  for (int w = 0; w < 3; w++) {
    l.color = C[w];
    const float x = X[w];
    // two cubic segments (bottom 70 -> middle 48 -> top 26), swinging 8 units left then right
    const float seg[2][8] = {{x, 70, x - 8, 57.9f, x - 8, 52, x, 48}, {x, 48, x + 8, 44, x + 8, 35.9f, x, 26}};
    lv_point_t prev = {(int32_t)(a.x1 + x * k), (int32_t)(a.y1 + 70 * k)};
    for (int s = 0; s < 2; s++)
      for (int i = 1; i <= 20; i++) {
        const float u = i / 20.0f, v = 1 - u, b0 = v * v * v, b1 = 3 * v * v * u, b2 = 3 * v * u * u, b3 = u * u * u;
        const float px = b0 * seg[s][0] + b1 * seg[s][2] + b2 * seg[s][4] + b3 * seg[s][6];
        const float py = b0 * seg[s][1] + b1 * seg[s][3] + b2 * seg[s][5] + b3 * seg[s][7];
        const lv_point_t p = {(int32_t)(a.x1 + px * k + 0.5f), (int32_t)(a.y1 + py * k + 0.5f)};
        dl(ctx, &l, &prev, &p);
        prev = p;
      }
  }
}
lv_obj_t *logo(lv_obj_t *p, int size) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_clickable(o, false);
  lv_obj_set_size(o, size, size);
  lv_obj_add_event_cb(o, logo_draw, LV_EVENT_DRAW_MAIN, NULL);
  return o;
}

// ---- line icons (24 px grid, 2 px strokes, round ends; colour = the object's line colour) ----------------
static void icon_draw(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target_obj(e);
  lv_layer_t *ctx = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const float k = lv_area_get_width(&a) / 24.0f;
  const int ox = a.x1, oy = a.y1, kind = (int)(intptr_t)lv_event_get_user_data(e);
  lv_draw_line_dsc_t l;
  lv_draw_line_dsc_init(&l);
  l.color = lv_obj_get_style_line_color(o, LV_PART_MAIN);
  l.width = 2;
  l.round_start = l.round_end = 1;
  lv_draw_arc_dsc_t c;
  lv_draw_arc_dsc_init(&c);
  c.color = l.color;
  c.width = 2;
  c.rounded = 1;
  auto P = [&](float x, float y) { return lv_point_t{(int32_t)(ox + x * k), (int32_t)(oy + y * k)}; };
  auto L = [&](float x1, float y1, float x2, float y2) { lv_point_t p1 = P(x1, y1), p2 = P(x2, y2); dl(ctx, &l, &p1, &p2); };
  auto C = [&](float x, float y, float r, int a0, int a1) { lv_point_t cp = P(x, y); darc(ctx, &c, &cp, (uint16_t)(r * k), a0, a1); };
  auto D = [&](float x, float y, float r) {          // a filled dot
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = l.color;
    d.radius = LV_RADIUS_CIRCLE;
    lv_point_t cp = P(x, y);
    const int rr = (int)(r * k + 0.5f) < 1 ? 1 : (int)(r * k + 0.5f);
    lv_area_t da = {(int32_t)(cp.x - rr), (int32_t)(cp.y - rr), (int32_t)(cp.x + rr), (int32_t)(cp.y + rr)};
    lv_draw_rect(ctx, &d, &da);
  };
  switch (kind) {
    case IC_HISTORY: C(12, 12, 8.5f, 0, 360); L(12, 7.5f, 12, 12); L(12, 12, 15, 14); break;
    case IC_SCALE: {
      lv_draw_rect_dsc_t r;
      lv_draw_rect_dsc_init(&r);
      r.bg_opa = LV_OPA_TRANSP;
      r.border_color = l.color;
      r.border_width = 2;
      r.radius = (int32_t)(2.5f * k);
      lv_point_t p1 = P(4, 9), p2 = P(20, 20);
      lv_area_t ra = {p1.x, p1.y, p2.x, p2.y};
      lv_draw_rect(ctx, &r, &ra);
      L(8.5f, 9, 8.5f, 6.5f); L(8.5f, 6.5f, 15.5f, 6.5f); L(15.5f, 6.5f, 15.5f, 9); L(12, 14.5f, 14.2f, 12.7f);
      break;
    }
    case IC_DISPLAY:
      C(12, 12, 4, 0, 360);
      for (int i = 0; i < 8; i++) {                 // eight rays from r 7 to r 10
        const float an = i * 0.785398f, cs = cosf(an), sn = sinf(an);
        L(12 + 7 * cs, 12 + 7 * sn, 12 + 10 * cs, 12 + 10 * sn);
      }
      break;
    case IC_WIFI: C(12, 20, 13, 225, 315); C(12, 20, 8.5f, 225, 315); D(12, 19, 1.4f); break;
    case IC_ABOUT: C(12, 12, 8.5f, 0, 360); L(12, 11, 12, 16.5f); D(12, 7.8f, 1); break;
    case IC_PROFILES: L(9, 7, 20, 7); L(9, 12, 20, 12); L(9, 17, 20, 17); D(5, 7, 1.2f); D(5, 12, 1.2f); D(5, 17, 1.2f); break;
    case IC_BELL:                                  // dome, straight sides flaring at the rim, clapper under it
      C(12, 9, 6, 180, 360); L(6, 9, 6, 13.5f); L(6, 13.5f, 4, 17); L(18, 9, 18, 13.5f); L(18, 13.5f, 20, 17);
      L(4, 17, 20, 17); C(12, 19, 2, 0, 180);
      break;
    case IC_SETTINGS: {                             // a cog: outline with eight teeth (r 7.5 .. 10) and a hole (r 3)
      C(12, 12, 3.2f, 0, 360);
      float px = 0, py = 0, fx = 0, fy = 0;
      for (int i = 0; i < 32; i++) {
        static const float R[4] = {7.4f, 10, 10, 7.4f}, A[4] = {-15, -8, 8, 15};
        const float an = ((i / 4) * 45 + A[i % 4]) * 0.0174533f, x = 12 + R[i % 4] * cosf(an), y = 12 + R[i % 4] * sinf(an);
        if (i) L(px, py, x, y); else { fx = x; fy = y; }
        px = x; py = y;
      }
      L(px, py, fx, fy);
      break;
    }
  }
}
lv_obj_t *line_icon(lv_obj_t *p, int kind, int size, lv_color_t col) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_clickable(o, false);
  lv_obj_set_size(o, size, size);
  lv_obj_set_style_line_color(o, col, 0);
  lv_obj_add_event_cb(o, icon_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)kind);
  return o;
}

// ---- empty state: icon, title, one line saying what happens next (lists with nothing to show) ----------
EmptyState empty_state(lv_obj_t *p, int kind) {
  EmptyState s;
  s.box = flex(p, LV_FLEX_FLOW_COLUMN, 10);
  lv_obj_set_flex_align(s.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_width(s.box, 752);
  lv_obj_set_style_pad_top(s.box, 72, 0);
  lv_obj_t *ic = line_icon(s.box, kind, 32, COL_MUTED);
  lv_obj_set_style_pad_bottom(ic, 6, 0);
  s.title = label(s.box, "", F_ROW, COL_TEXT);
  s.body = label(s.box, "", F_TEXT, COL_MUTED);
  return s;
}
void empty_set(const EmptyState &s, const char *title, const char *body) {
  set_text(s.title, title);
  set_text(s.body, body);
}

lv_obj_t *flex(lv_obj_t *p, lv_flex_flow_t flow, int gap) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_clickable(o, false);
  lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(o, flow);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (flow == LV_FLEX_FLOW_ROW) lv_obj_set_style_pad_column(o, gap, 0);
  else lv_obj_set_style_pad_row(o, gap, 0);
  return o;
}

void make_button(lv_obj_t *o, lv_color_t pressed) {
  lv_obj_set_clickable(o, true);
  lv_obj_set_style_bg_color(o, pressed, LV_STATE_PRESSED);
}

lv_obj_t *line(lv_obj_t *p, const lv_point_t *pts, uint16_t n, int width, lv_color_t col) {
  lv_obj_t *l = lv_line_create(p);
  lv_line_set_points(l, (const lv_point_precise_t *)pts, n);
  lv_obj_set_pos(l, 0, 0);
  lv_obj_set_style_line_width(l, width, 0);
  lv_obj_set_style_line_color(l, col, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  lv_obj_set_clickable(l, false);
  return l;
}

// change-only setters: LVGL redraws on every set even when the value is the same, and every redraw is PSRAM traffic
void set_text(lv_obj_t *l, const char *s) {
  const char *cur = lv_label_get_text(l);
  if (!cur || strcmp(cur, s) != 0) lv_label_set_text(l, s);
}
void set_bg(lv_obj_t *o, lv_color_t c) {
  if (!lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), c)) lv_obj_set_style_bg_color(o, c, 0);
}
void set_tc(lv_obj_t *o, lv_color_t c) {
  if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) lv_obj_set_style_text_color(o, c, 0);
}
void set_line_c(lv_obj_t *o, lv_color_t c) {
  if (!lv_color_eq(lv_obj_get_style_line_color(o, LV_PART_MAIN), c)) lv_obj_set_style_line_color(o, c, 0);
}
void set_hidden(lv_obj_t *o, bool h) {
  if (lv_obj_is_hidden(o) != h) { if (h) lv_obj_set_hidden(o, true); else lv_obj_set_hidden(o, false); }
}
void set_w(lv_obj_t *o, int32_t w) {
  if (lv_obj_get_style_width(o, LV_PART_MAIN) != w) lv_obj_set_width(o, w);
}
void set_h(lv_obj_t *o, int32_t h) {
  if (lv_obj_get_style_height(o, LV_PART_MAIN) != h) lv_obj_set_height(o, h);
}
void set_y(lv_obj_t *o, int32_t y) {
  if (lv_obj_get_style_y(o, LV_PART_MAIN) != y) lv_obj_set_y(o, y);
}

// "Leva 9 SINGLE v0.9 (community)" -> "Leva 9 SINGLE v0.9" + community tag
void display_name(const char *in, char *out, size_t n, bool *community) {
  static const char SUF[] = " (community)";
  snprintf(out, n, "%s", in);
  size_t l = strlen(out), sl = sizeof SUF - 1;
  bool c = l > sl && strcasecmp(out + l - sl, SUF) == 0;
  if (c) out[l - sl] = 0;
  if (community) *community = c;
}

// ---- icons ----------------------------------------------------------------------------------
lv_obj_t *icon_box(lv_obj_t *p, int w, int h) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_clickable(o, false);
  lv_obj_set_size(o, w, h);
  return o;
}

lv_obj_t *icon_tank(lv_obj_t *p, int pct, lv_color_t fill) {
  lv_obj_t *c = icon_box(p, 18, 26);
  box(c, 6, 1, 6, 3, COL_OUTLINE, 2);                       // cap
  lv_obj_t *body = lv_obj_create(c);                         // outline
  lv_obj_remove_style_all(body);
  lv_obj_set_scrollable(body, false);
  lv_obj_set_clickable(body, false);
  lv_obj_set_pos(body, 2, 4);
  lv_obj_set_size(body, 14, 20);
  lv_obj_set_style_radius(body, 4, 0);
  lv_obj_set_style_border_width(body, 2, 0);
  lv_obj_set_style_border_color(body, COL_OUTLINE, 0);
  lv_obj_set_style_border_opa(body, LV_OPA_COVER, 0);
  lv_obj_set_style_clip_corner(body, true, 0);
  int fh = 16 * pct / 100;
  if (fh < 2) fh = 2;
  return box(body, 0, 16 - fh, 10, fh, fill, 0);
}

void icon_chevron_right(lv_obj_t *p, lv_color_t col) {
  static const lv_point_t pts[] = {{6, 4}, {10, 8}, {6, 12}};
  lv_obj_t *c = icon_box(p, 16, 16);
  line(c, pts, 3, 2, col);
}

void icon_check(lv_obj_t *p, lv_color_t col) {     // 24-unit design path scaled to 20 px
  static const lv_point_t pts[] = {{4, 10}, {8, 14}, {16, 6}};
  lv_obj_t *c = icon_box(p, 20, 20);
  line(c, pts, 3, 2, col);
}

void icon_close(lv_obj_t *p, lv_color_t col) {
  static const lv_point_t a[] = {{5, 5}, {15, 15}};
  static const lv_point_t b[] = {{15, 5}, {5, 15}};
  lv_obj_t *c = icon_box(p, 20, 20);
  line(c, a, 2, 2, col);
  line(c, b, 2, 2, col);
}

void icon_minus(lv_obj_t *p, lv_color_t col) {
  static const lv_point_t a[] = {{4, 10}, {16, 10}};
  lv_obj_t *c = icon_box(p, 20, 20);
  line(c, a, 2, 2, col);
}

void icon_plus(lv_obj_t *p, lv_color_t col) {
  static const lv_point_t a[] = {{4, 10}, {16, 10}};
  static const lv_point_t b[] = {{10, 4}, {10, 16}};
  lv_obj_t *c = icon_box(p, 20, 20);
  line(c, a, 2, 2, col);
  line(c, b, 2, 2, col);
}

void icon_backspace(lv_obj_t *p, lv_color_t col) {
  static const lv_point_t outline[] = {{10, 6}, {26, 6}, {26, 24}, {10, 24}, {2, 15}, {10, 6}};
  static const lv_point_t x1[] = {{14, 11}, {20, 19}};
  static const lv_point_t x2[] = {{20, 11}, {14, 19}};
  lv_obj_t *c = icon_box(p, 30, 30);
  line(c, outline, 6, 2, col);
  line(c, x1, 2, 2, col);
  line(c, x2, 2, 2, col);
}

// ---- chart helpers shared by the brewing, review and steam screens -------------------------------------------------
void ch_hline(lv_layer_t *ctx, int x1, int x2, int y, lv_color_t col) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = col;
  d.width = 1;
  lv_point_t a = {(int32_t)x1, (int32_t)y}, b = {(int32_t)x2, (int32_t)y};
  dl(ctx, &d, &a, &b);
}
// text at (x, y top); align: LV_TEXT_ALIGN_LEFT (x = left), CENTER (x = middle), RIGHT (x = right edge)
void ch_text_c(lv_layer_t *ctx, int x, int y, const char *s, lv_text_align_t align, lv_color_t col) {
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.font = F_SMALL;
  d.color = col;
  d.align = align;
  const int w = 200;
  const int x1 = align == LV_TEXT_ALIGN_LEFT ? x : (align == LV_TEXT_ALIGN_CENTER ? x - w / 2 : x - w);
  lv_area_t a = {(int32_t)x1, (int32_t)y, (int32_t)(x1 + w - 1), (int32_t)(y + 16)};
  dlabel(ctx, &d, &a, s);
}
// time labels under a chart's baseline: every step_s, "N s" under a minute, then "m:ss" / "N min", the span at the end
void axis_time(lv_layer_t *ctx, int x0, int w, int y, int span_s, int step_s) {
  char t[16];
  for (int s = step_s; s < span_s; s += step_s) {
    const int x = x0 + s * w / span_s;
    if (x > x0 + w - 60) break;                      // keep clear of the end label
    if (s < 60 || s % 60) snprintf(t, sizeof t, s < 60 ? "%d s" : "%d:%02d", s < 60 ? s : s / 60, s % 60);
    else snprintf(t, sizeof t, "%d min", s / 60);
    ch_text_c(ctx, x, y, t, LV_TEXT_ALIGN_CENTER, COL_AXIS);
  }
  if (span_s < 60) snprintf(t, sizeof t, "%d s", span_s);
  else if (span_s % 60) snprintf(t, sizeof t, "%d:%02d", span_s / 60, span_s % 60);
  else snprintf(t, sizeof t, "%d min", span_s / 60);
  ch_text_c(ctx, x0 + w, y, t, LV_TEXT_ALIGN_RIGHT, COL_AXIS);
}
// one value of the row under a chart: dot in the value's colour, the number (F_VALUE), unit on its baseline
lv_obj_t *strip_value(lv_obj_t *row, lv_color_t dot, const char *unit) {
  lv_obj_t *it = flex(row, LV_FLEX_FLOW_ROW, 8);
  lv_obj_set_flex_align(it, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
  digit_dot(it, F_VALUE, dot, 8);
  lv_obj_t *v = label(it, "0.0", F_VALUE, COL_TEXT);
  lv_obj_t *u = label(it, unit, F_ROW, COL_MUTED);
  lv_obj_set_style_pad_bottom(u, base_pad(F_VALUE, F_ROW), 0);
  return v;
}

// a stored time as "today 14:05", "yesterday 09:12" or "4 Nov 16:55" ("-" when the machine's clock was not set)
void fmt_when(uint32_t ts, char *b, size_t n) {
  const time_t now = time(NULL), t = (time_t)ts;
  if (ts < 1600000000u) { snprintf(b, n, "-"); return; }
  struct tm a, c;
  localtime_r(&t, &a);
  localtime_r(&now, &c);
  static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const bool synced = now > 1600000000;
  if (synced && a.tm_year == c.tm_year && a.tm_yday == c.tm_yday) snprintf(b, n, "today %02d:%02d", a.tm_hour, a.tm_min);
  else if (synced && a.tm_year == c.tm_year && a.tm_yday + 1 == c.tm_yday)
    snprintf(b, n, "yesterday %02d:%02d", a.tm_hour, a.tm_min);
  else snprintf(b, n, "%d %s %02d:%02d", a.tm_mday, MON[a.tm_mon % 12], a.tm_hour, a.tm_min);
}
// a pill-shaped row (round ends, filled): the state pills of the brewing, review and steam headers, steam's Refill
lv_obj_t *pill_row(lv_obj_t *p) {
  lv_obj_t *r = flex(p, LV_FLEX_FLOW_ROW, 8);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_height(r, 40);
  lv_obj_set_style_radius(r, RAD_PILL, 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  return r;
}
