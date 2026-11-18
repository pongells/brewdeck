// Touch test: shows raw and mapped touch coordinates and leaves a trail of dots.
// Tap the four labelled corners to check the axes. Tap CLEAR to wipe the trail.
#include <Arduino.h>
#include "board.h"
#include "touch_gt911.h"

static uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return gfx->color565(r, g, b); }

static const int CLR_X = 340, CLR_Y = 400, CLR_W = 120, CLR_H = 60;

static void corner(int x, int y, const char *name) {
  gfx->fillRect(x, y, 90, 60, C(30, 30, 34));
  gfx->setTextColor(C(245, 192, 74));
  gfx->setTextSize(3);
  gfx->setCursor(x + 12, y + 18);
  gfx->print(name);
}

static void draw_base() {
  gfx->fillScreen(C(10, 10, 11));
  corner(0, 0, "TL");
  corner(SCREEN_W - 90, 0, "TR");
  corner(0, SCREEN_H - 60, "BL");
  corner(SCREEN_W - 90, SCREEN_H - 60, "BR");
  gfx->fillRoundRect(CLR_X, CLR_Y, CLR_W, CLR_H, 12, C(26, 26, 29));
  gfx->setTextColor(C(242, 242, 243));
  gfx->setTextSize(3);
  gfx->setCursor(CLR_X + 14, CLR_Y + 18);
  gfx->print("CLEAR");
  gfx->setTextSize(2);
  gfx->setTextColor(C(142, 142, 150));
  gfx->setCursor(200, 130);
  gfx->print("Touch the screen. Dots show mapped position.");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  board_init();
  bool ok = touch_begin();
  draw_base();
  gfx->setTextSize(3);
  gfx->setCursor(200, 60);
  if (ok) {
    gfx->setTextColor(C(74, 222, 128));
    gfx->printf("GT911 found at 0x%02X", touch_address());
  } else {
    gfx->setTextColor(C(229, 72, 77));
    gfx->print("No touch controller found");
  }
  Serial.println(ok ? "touch ok" : "touch NOT found");
  int xm, ym; touch_cfg(&xm, &ym);
  gfx->setTextSize(2);
  gfx->setTextColor(C(142, 142, 150));
  gfx->setCursor(200, 100);
  gfx->printf("chip range %d x %d", xm, ym);
}

void loop() {
  static bool was_down = false;
  static uint32_t last_print = 0;
  int16_t rx, ry, x, y;
  static int16_t lx = -1, ly = -1;
  if (touch_raw(&rx, &ry)) {
    touch_map(rx, ry, &x, &y);
    if (was_down && x == lx && y == ly) { delay(10); return; }   // unchanged: don't redraw
    lx = x; ly = y;
    if (x >= CLR_X && x < CLR_X + CLR_W && y >= CLR_Y && y < CLR_Y + CLR_H) {
      if (!was_down) draw_base();
    } else {
      gfx->fillCircle(x, y, 6, C(245, 192, 74));
    }
    static int16_t mnx = 32767, mny = 32767, mxx = -32768, mxy = -32768;
    if (rx < mnx) mnx = rx; if (rx > mxx) mxx = rx;
    if (ry < mny) mny = ry; if (ry > mxy) mxy = ry;
    gfx->fillRect(200, 160, 400, 120, C(10, 10, 11));
    gfx->setTextSize(2);
    gfx->setTextColor(C(142, 142, 150));
    gfx->setCursor(200, 245);
    gfx->printf("seen raw x %d..%d  y %d..%d", mnx, mxx, mny, mxy);
    gfx->setTextColor(C(242, 242, 243));
    gfx->setTextSize(3);
    gfx->setCursor(200, 165);
    gfx->printf("raw    %4d, %4d", rx, ry);
    gfx->setCursor(200, 200);
    gfx->printf("mapped %4d, %4d", x, y);
    if (millis() - last_print > 200) {
      last_print = millis();
      Serial.printf("raw %d,%d mapped %d,%d\n", rx, ry, x, y);
    }
    was_down = true;
  } else {
    was_down = false;
  }
  delay(10);
}
