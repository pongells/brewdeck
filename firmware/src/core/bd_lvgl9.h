#pragma once
// LVGL 9 adapter for the Brewdeck core (brewdeck_core.h). The core has no graphics library; this connects LVGL 9 to
// bd_display_flush / bd_touch_read. A UI that uses LVGL 9:
//
//   #include "bd_lvgl9.h"
//   extern "C" const bd_ui_desc BD_UI = {"myui", "1.0", 1};   // 1: the API version it was written for
//   void bd_ui_init(void) { bd_lvgl9_setup(); lv_label_set_text(lv_label_create(lv_screen_active()), "Hello"); }
//   void bd_ui_tick(uint32_t now) { bd_lvgl9_tick(now); }
//   void bd_ui_event(const bd_event *e) { (void)e; }
//   void bd_ui_wake(void) {}
//
// Header-only (static functions): include it in ONE file of the UI. lv_conf.h: LV_COLOR_FORMAT_DEFAULT
// LV_COLOR_FORMAT_RGB565 (the core takes RGB565 in native byte order).
#include "brewdeck_core.h"
#include "lvgl.h"

// optional: called with each finished rectangle just before it goes to the screen (e.g. an effect over everything);
// px is its top-left pixel, rows stride pixels apart (the strip's width)
static void (*bd_lvgl9_flush_filter)(int x1, int y1, int x2, int y2, uint16_t *px, int stride) = NULL;

static lv_display_t *bd_lv_disp;
static uint32_t bd_lv_last;

static void bd_lv_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  if (bd_lvgl9_flush_filter) bd_lvgl9_flush_filter(a->x1, a->y1, a->x2, a->y2, (uint16_t *)px, a->x2 - a->x1 + 1);
  bd_display_flush(a->x1, a->y1, a->x2, a->y2, (const uint16_t *)px);   // returns once px is copied
  lv_display_flush_ready(d);
}
static void bd_lv_touch(lv_indev_t *in, lv_indev_data_t *data) {
  (void)in;
  int x, y;
  if (bd_touch_read(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;            // LVGL keeps the last point for the release
  }
}

// Call once from bd_ui_init(). One draw strip of the board's draw_lines in internal RAM (bd_display_flush
// copies synchronously, so a second strip would never be drawn into while the first is sent; and LVGL 9 walks the
// object tree once per strip, so fewer, taller strips are faster: 20-line pairs took ~128 ms for a full Ready
// redraw on the ESP32-S3, one 40-line strip ~100 ms). false if it does not fit (then PSRAM, slower).
static bool bd_lvgl9_setup(void) {
  const bd_display_info *di = bd_display();
  const uint32_t bytes = (uint32_t)di->width * (uint32_t)di->draw_lines * 2;
  lv_init();
  void *b1 = bd_alloc(bytes, BD_MEM_INTERNAL), *b2 = NULL;
  const bool ok = b1 != NULL;
  if (!ok) {
    b1 = bd_alloc(bytes, BD_MEM_PSRAM);
    bd_log("bd_lvgl9: draw strips in PSRAM (internal RAM short)");
  }
  bd_lv_disp = lv_display_create(di->width, di->height);
  lv_display_set_color_format(bd_lv_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(bd_lv_disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(bd_lv_disp, bd_lv_flush);
  lv_indev_t *in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, bd_lv_touch);
  lv_indev_set_display(in, bd_lv_disp);
  return ok;
}

// Call from bd_ui_tick(): advances LVGL's clock by the core's (the emulator's fast-forward works through it) and
// lets it draw.
static void bd_lvgl9_tick(uint32_t now_ms) {
  if (bd_lv_last) lv_tick_inc(now_ms - bd_lv_last);
  bd_lv_last = now_ms ? now_ms : 1;
  lv_timer_handler();
}
