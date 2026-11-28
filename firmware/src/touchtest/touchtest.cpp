// Touch test: a UI that shows where the core says the screen is touched (bd_touch_read: the board's mapped
// coordinates, the same every UI gets) and leaves a trail of dots. Tap the four labelled corners to check the axes;
// tap CLEAR to wipe the trail. Both boards: UI=touchtest ./build.sh ui --flash (or ui-p4). Uses no machine data.
#include <stdio.h>
#include "bd_lvgl9.h"

extern "C" const bd_ui_desc BD_UI = {"touchtest", "1.0", 1};

static const lv_color_t BG = lv_color_hex(0x0A0A0B), CARD = lv_color_hex(0x1E1E22), TEXT = lv_color_hex(0xF2F2F3),
                        MUTED = lv_color_hex(0x8E8E96), GOLD = lv_color_hex(0xF5C04A);
enum { DOTS = 200, DOT = 12, CORNER_W = 90, CORNER_H = 60, CLR_W = 120, CLR_H = 60 };
static lv_obj_t *g_xy, *g_seen, *g_dot[DOTS];
static int g_next, g_lx = -1, g_ly = -1, g_minx, g_miny, g_maxx, g_maxy;
static bool g_down;
static uint32_t g_logged;

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, int radius) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *t) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, t);
  return l;
}
static void corner(lv_obj_t *scr, int x, int y, const char *name) {
  lv_obj_center(label(box(scr, x, y, CORNER_W, CORNER_H, CARD, 0), &lv_font_montserrat_20, GOLD, name));
}
static void clear_trail(void) {
  for (int i = 0; i < DOTS; i++) lv_obj_add_flag(g_dot[i], LV_OBJ_FLAG_HIDDEN);
  g_next = 0;
  g_minx = g_miny = 32767;
  g_maxx = g_maxy = -1;
  lv_label_set_text(g_seen, "");
}

void bd_ui_init(void) {
  bd_lvgl9_setup();
  const int w = bd_display()->width, h = bd_display()->height;
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  corner(scr, 0, 0, "TL");
  corner(scr, w - CORNER_W, 0, "TR");
  corner(scr, 0, h - CORNER_H, "BL");
  corner(scr, w - CORNER_W, h - CORNER_H, "BR");
  lv_obj_center(label(box(scr, (w - CLR_W) / 2, h - 80, CLR_W, CLR_H, CARD, 12), &lv_font_montserrat_20, TEXT,
                      "CLEAR"));
  lv_obj_t *l = label(scr, &lv_font_montserrat_14, MUTED, "Touch the screen: dots show where the core places it.");
  lv_obj_set_pos(l, 200, 130);
  g_xy = label(scr, &lv_font_montserrat_48, TEXT, "-, -");
  lv_obj_set_pos(g_xy, 200, 170);
  g_seen = label(scr, &lv_font_montserrat_14, MUTED, "");
  lv_obj_set_pos(g_seen, 200, 240);
  char t[48];
  snprintf(t, sizeof t, "screen %d x %d", w, h);
  lv_obj_set_pos(label(scr, &lv_font_montserrat_14, MUTED, t), 200, 100);
  for (int i = 0; i < DOTS; i++) g_dot[i] = box(scr, 0, 0, DOT, DOT, GOLD, DOT / 2);
  clear_trail();
  bd_log("touchtest: tap the corners; CLEAR wipes the trail");
}

void bd_ui_tick(uint32_t now) {
  int x, y;
  const bool down = bd_touch_read(&x, &y);
  if (down && !(g_down && x == g_lx && y == g_ly)) {   // a new position
    const int w = bd_display()->width, h = bd_display()->height;
    if (x >= (w - CLR_W) / 2 && x < (w + CLR_W) / 2 && y >= h - 80 && y < h - 80 + CLR_H) {
      if (!g_down) clear_trail();
    } else {
      lv_obj_set_pos(g_dot[g_next], x - DOT / 2, y - DOT / 2);
      lv_obj_remove_flag(g_dot[g_next], LV_OBJ_FLAG_HIDDEN);
      g_next = (g_next + 1) % DOTS;
    }
    if (x < g_minx) g_minx = x;
    if (x > g_maxx) g_maxx = x;
    if (y < g_miny) g_miny = y;
    if (y > g_maxy) g_maxy = y;
    char t[64];
    snprintf(t, sizeof t, "%d, %d", x, y);
    lv_label_set_text(g_xy, t);
    snprintf(t, sizeof t, "seen x %d..%d  y %d..%d", g_minx, g_maxx, g_miny, g_maxy);
    lv_label_set_text(g_seen, t);
    if (now - g_logged > 200) {
      g_logged = now;
      bd_log("touch %d, %d", x, y);
    }
    g_lx = x;
    g_ly = y;
  }
  g_down = down;
  bd_lvgl9_tick(now);
}

void bd_ui_event(const bd_event *e) { (void)e; }   // no machine data here
void bd_ui_wake(void) {}
