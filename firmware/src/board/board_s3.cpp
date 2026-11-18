// Board layer of the Brewdeck core API (brewdeck_core.h) for the Sunton ESP32-S3 4.3" (800x480 RGB panel, GT911
// touch): the display, touch, backlight and memory functions, on top of src/shared (board.cpp, touch_gt911.cpp).
// The panel is driven by Arduino_GFX with its frame buffer in PSRAM and bounce buffers (see board.cpp); a flushed
// rectangle is copied into that frame buffer by the CPU.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "board.h"
#include "touch_gt911.h"
#include "../core/brewdeck_core.h"
#include "board_internal.h"

bool board_begin(void) {
  board_init();
  return touch_begin();
}

static const bd_display_info g_info = {SCREEN_W, SCREEN_H, BD_PIXEL_RGB565, 40};   // one 40-line strip: 64 KB internal RAM
const bd_display_info *bd_display(void) { return &g_info; }

// The flushed rectangles are copied by the CPU. A DMA copy (esp_async_memcpy) was 10 ms faster per full screen but
// scrambled the picture on the panel: its writes to PSRAM compete with the panel's bounce-buffer reads from the same
// frame buffer, and the panel lost sync.
static uint32_t g_flush_us = 0, g_flush_px = 0;   // time spent copying into the frame buffer (bd_display_stats)
void bd_display_flush(int x1, int y1, int x2, int y2, const uint16_t *px) {
  const uint32_t w = (uint32_t)(x2 - x1 + 1), h = (uint32_t)(y2 - y1 + 1);
  const uint32_t t0 = micros();
  gfx->draw16bitRGBBitmap(x1, y1, (uint16_t *)px, w, h);
  g_flush_us += micros() - t0;
  g_flush_px += w * h;
}
void bd_display_stats(uint32_t *us, uint32_t *px, bool reset) {
  if (us) *us = g_flush_us;
  if (px) *px = g_flush_px;
  if (reset) g_flush_us = g_flush_px = 0;
}

// remote touch (debug server, GET /touch): pressed at (x, y) until the time runs out
static volatile int g_ft_x, g_ft_y;
static volatile uint32_t g_ft_until = 0;
void board_fake_touch(int x, int y, int ms) {
  g_ft_x = x;
  g_ft_y = y;
  g_ft_until = millis() + (uint32_t)(ms < 30 ? 30 : ms);
}
bool bd_touch_read(int *x, int *y) {
  if (g_ft_until && (int32_t)(g_ft_until - millis()) > 0) { *x = g_ft_x; *y = g_ft_y; return true; }
  int16_t tx, ty;
  if (!touch_pressed(&tx, &ty)) return false;
  *x = tx;
  *y = ty;
  return true;
}

void bd_set_backlight(int pct) { board_backlight(pct); }

void *bd_alloc(size_t n, bd_mem where) {
  if (where == BD_MEM_INTERNAL) return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (where == BD_MEM_PSRAM || n > 4096) return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  return malloc(n);
}
void bd_free(void *p) { heap_caps_free(p); }
