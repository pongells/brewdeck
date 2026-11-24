// The part of the board layer (brewdeck_core.h) both boards share: start-up, touch (the GT911 driver next to this
// file, plus the debug server's remote touch), the backlight call and memory. Each board's own folder (src/board/s3,
// src/board/p4; ui_select.py builds one) has its panel, display functions and board.h (pins, touch orientation).
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "board.h"
#include "touch_gt911.h"
#include "../../core/brewdeck_core.h"
#include "board_core.h"

// On a software restart (an update, the way back after a failed trial, esp_restart) the panel loses its picture
// before the new start sets it up again, and the P4's panel shows that as a blue screen: the backlight goes off
// first. Not on a crash (no shutdown handlers then).
static void backlight_off_on_restart(void) { board_backlight(0); }

bool board_begin(void) {
  board_init();
  esp_register_shutdown_handler(backlight_off_on_restart);
  return touch_begin();
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
