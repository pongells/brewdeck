#include "touch_gt911.h"
#include "board.h"
#include <Wire.h>

static uint8_t g_addr = 0;
static bool    g_down = false;
static int16_t g_rx = 0, g_ry = 0;
static uint32_t g_last_ready = 0;
static int g_xmax = SCREEN_W, g_ymax = SCREEN_H;   // from the chip's own config


static bool reg_write(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(g_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool reg_read(uint16_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(g_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((int)g_addr, (int)n);
  if (got != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

uint8_t touch_address() { return g_addr; }
void touch_cfg(int *xmax, int *ymax) { *xmax = g_xmax; *ymax = g_ymax; }

bool touch_begin() {
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000);
  if (PIN_TOUCH_RST >= 0) {
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(80);
  }

  const uint8_t candidates[2] = {0x5D, 0x14};
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(candidates[i]);
    if (Wire.endTransmission() == 0) {
      g_addr = candidates[i];
      break;
    }
  }
  if (g_addr == 0) return false;

  uint8_t pid[4] = {0, 0, 0, 0};
  if (reg_read(0x8140, pid, 4)) {
    Serial.printf("GT911 at 0x%02X, product id %c%c%c%c\n", g_addr, pid[0], pid[1], pid[2], pid[3]);
  }
  uint8_t cfg[4];
  if (reg_read(0x8048, cfg, 4)) {
    int xm = cfg[0] | (cfg[1] << 8), ym = cfg[2] | (cfg[3] << 8);
    Serial.printf("GT911 config range %d x %d\n", xm, ym);
    if (xm >= 100 && xm <= 4096 && ym >= 100 && ym <= 4096) { g_xmax = xm; g_ymax = ym; }
  }
  reg_write(0x814E, 0);
  return true;
}

bool touch_raw(int16_t *rx, int16_t *ry) {
  if (g_addr == 0) return false;
  uint8_t st = 0;
  if (reg_read(0x814E, &st, 1)) {
    if (st & 0x80) {
      g_last_ready = millis();
      uint8_t n = st & 0x0F;
      if (n > 0 && n <= 5) {
        uint8_t p[8];
        if (reg_read(0x8150, p, 8)) {
          g_rx = (int16_t)(p[0] | (p[1] << 8));
          g_ry = (int16_t)(p[2] | (p[3] << 8));
          g_down = true;
        }
      } else {
        g_down = false;
      }
      reg_write(0x814E, 0);
    }
  }
  if (g_down && millis() - g_last_ready > 120) g_down = false;   // no report for a while (or the bus failing): lifted
  if (g_down) {
    *rx = g_rx;
    *ry = g_ry;
  }
  return g_down;
}

void touch_map(int16_t rx, int16_t ry, int16_t *x, int16_t *y) {
  // scale from the chip's own range to screen pixels
#if TOUCH_SWAP_XY
  int32_t mx = (int32_t)ry * SCREEN_W / g_ymax, my = (int32_t)rx * SCREEN_H / g_xmax;
#else
  int32_t mx = (int32_t)rx * SCREEN_W / g_xmax, my = (int32_t)ry * SCREEN_H / g_ymax;
#endif
#if TOUCH_FLIP_X
  mx = (SCREEN_W - 1) - mx;
#endif
#if TOUCH_FLIP_Y
  my = (SCREEN_H - 1) - my;
#endif
  if (mx < 0) mx = 0;
  if (my < 0) my = 0;
  if (mx > SCREEN_W - 1) mx = SCREEN_W - 1;
  if (my > SCREEN_H - 1) my = SCREEN_H - 1;
  *x = (int16_t)mx;
  *y = (int16_t)my;
}

bool touch_pressed(int16_t *x, int16_t *y) {
  int16_t rx, ry;
  if (!touch_raw(&rx, &ry)) return false;
  touch_map(rx, ry, x, y);
  return true;
}
